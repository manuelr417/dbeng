# FILES.md — POSIX file I/O concepts in BlockFile (ex09)

`BlockFile.{h,cpp}` is the only module that talks to the operating system:
everything above it (BufferPool, BufferManager, the CLI) sees 4096-byte
blocks, never raw system calls. This note explains the POSIX concepts the
module is built on — binary files, file descriptors, `open`, `close`,
`write`, `read` and `lseek` — and how each one shows up in the code.

## The POSIX binary file

To POSIX, a file is nothing but a **sequence of bytes** with a length. The
kernel has no notion of lines, records, types, or encodings — `people.bin`
is not "a file of Persons", it is a file of bytes. All structure is imposed
by software, and in this project that software is `PersonSerializer` plus
BlockFile:

- A file is a sequence of **4096-byte blocks** (`kBlockSize`).
- Block `N` starts at byte offset `N * 4096`.
- Each block holds up to **195 records** of 21 bytes each (`kRecordSize`,
  `sizeof(int) + 10 + sizeof(int) + 3`); free slots are zeroed.
- Consequently the file size must be a multiple of 4096. BlockFile enforces
  this invariant on every measurement (`file_size` in BlockFile.cpp):

  ```cpp
  if (size % static_cast<off_t>(bufman::kBlockSize) != 0) {
      error = "binary file size is not a multiple of 4096 bytes";
      return false;
  }
  ```

Two more properties of a POSIX file matter here. First, the file carries a
**file offset** — a cursor into the byte sequence shared by `read` and
`write` (see `lseek` below). Second, a `write` positioned past the current
end of file **extends the file**, which is exactly how `write_block` and
`append_records` grow a file block by block.

## The file descriptor

A **file descriptor (fd)** is a small nonnegative integer the kernel hands
back from `open` and which every subsequent call (`read`, `write`, `lseek`,
`close`) uses to name the open file. Descriptors 0, 1 and 2 are the standard
input, output and error streams; everything the process opens gets the next
free number.

BlockFile wraps the fd in a tiny struct instead of passing raw ints around:

```cpp
struct File {
    int fd = -1;
};
```

Two conventions make this safe:

- **`fd == -1` means "not open"** (the default). Every BlockFile entry point
  checks it first: `error = "read requested on a closed file"`.
- The kernel **recycles descriptors**: after `close(4)`, the next `open` may
  return 4 again. A stale fd left lying around could silently point at a
  different file, which is why `close` resets `file.fd = -1` and why
  `BufferManager::close_all` drops all page-table state when it closes its
  files.

Note that a fd refers to *an open file description* in the kernel, not to a
path: the file can be renamed or deleted after `open` and the fd keeps
working against the same inode.

## `open`

```cpp
file.fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);   // open_for_append
file.fd = ::open(path.c_str(), O_RDONLY);                 // open_for_read
```

`open` is the only door into a file: it resolves the path, checks
permissions, creates the file if asked, and returns a fresh descriptor.

- **Flags** select behavior. `open_for_append` uses `O_RDWR | O_CREAT` —
  read-write so the descriptor serves both reads and write-backs, creating
  the file if it does not exist. `open_for_read` uses `O_RDONLY` and fails
  on a missing file. (`O_APPEND` is deliberately *not* used: the module
  positions explicitly with `lseek` instead of letting the kernel pin the
  offset at the end.)
- **Mode** `0644` is octal — owner read/write, group/others read — and only
  applies at the moment the file is created; it is ignored otherwise.
- **Failure** is signaled by `-1` plus a reason stored in the global `errno`.
  The module's `set_error` translates it:

  ```cpp
  error = action + ": " + std::strerror(errno);
  ```

- The `::` prefix (`::open`, `::read`, …) reaches the global C function from
  inside `namespace bufman`, where names like `close` and `read_block`
  already exist. Without `::`, calls would be ambiguous or silently resolve
  to the wrong function.

## `close`

```cpp
void close(File& file) {
    if (file.fd >= 0) {
        ::close(file.fd);
        file.fd = -1;
    }
}
```

`close` releases the descriptor: the kernel drops the open-file description
and the number becomes reusable. Closing is also when the OS is free to
flush its own caches for the file, so it belongs at the end of every code
path — `BufferManager::close_all` calls it for every file it opened, after
flushing dirty frames.

The wrapper adds two guards worth copying: it no-ops on an already-closed
file, and it resets `fd` to `-1` afterwards. Calling `close` twice on the
same descriptor is a real bug — the second call might close a *different*
file that happened to be assigned the same recycled number.

## `write`

```cpp
const ssize_t written = ::write(fd, buffer + completed, length - completed);
if (written < 0 && errno == EINTR) {
    continue;
}
```

`write(fd, buf, count)` copies at most `count` bytes from memory into the
file, starting at the current file offset, and returns **how many bytes it
actually transferred**. The crucial subtlety: it may transfer **fewer bytes
than requested** (a *partial write*) — the kernel buffer might be full, the
disk nearly so, or a signal may have interrupted the call (`errno == EINTR`).
A short write is not an error, but ignoring it silently corrupts data.

BlockFile therefore never calls `write` directly from its public API; it
goes through `write_all`, which loops until the whole buffer is written,
retries on `EINTR`, and treats a zero-byte result as failure
(`"write: no progress"`) rather than looping forever.

The other job `write` does here is **extending the file**: writing at an
offset beyond the current end (via `write_block`) makes the file grow to
cover it. That is the mechanism behind block-granular appends.

## `read`

```cpp
const ssize_t read_count = ::read(fd, buffer + completed, length - completed);
```

`read(fd, buf, count)` copies up to `count` bytes from the file — starting
at the file offset — into memory, advancing the offset by the number of
bytes read. It returns that count, and the same partial-transfer rules as
`write` apply (loop until done, retry on `EINTR` — `read_all`).

Its return value has two special cases:

- **`0` means end of file**: nothing was read because the offset sits at the
  end. In `read_all` this is reported as `"read: unexpected end of file"`.
- **`-1` means error**, with `errno` explaining why (again `EINTR` for
  signals).

In this module a bare EOF should never happen: `read_block` checks
`block_number < block_count(file)` *before* touching the file, so any EOF
reached by `read_all` means the file shrank or got corrupted underneath us.

## `lseek`

```cpp
const off_t offset = static_cast<off_t>(block_number * bufman::kBlockSize);
if (::lseek(fd, offset, SEEK_SET) == static_cast<off_t>(-1)) {
```

`lseek(fd, offset, whence)` **repositions the file offset** — the cursor
that `read` and `write` share. It moves no data; it just sets where the next
one starts. `whence` picks the reference point: `SEEK_SET` (from the start,
used here), `SEEK_CUR` (relative to the current position), or `SEEK_END`
(relative to the end of file).

BlockFile uses it in two ways:

- **`seek_block`** converts a block number to a byte offset
  (`block_number * 4096`) and seeks there with `SEEK_SET` before every
  `read_block` / `write_block`. Since `off_t` is a *signed* type, the offset
  is range-checked first so a huge block number cannot overflow
  (`"block offset exceeds off_t range"`).
- **`file_size`** seeks with `::lseek(fd, 0, SEEK_END)`; the returned offset
  *is* the file size in bytes. That is how `block_count` computes
  `size / kBlockSize` without `stat`.

Two footnotes: seeking past EOF and writing creates a *sparse file* (a hole
of implicit zeros) — BlockFile never relies on that, it always writes real
block contents. And the offset is per open-file description, so two
descriptors on the same file have independent cursors; within this project
each `File` is used by one code path at a time, so sharing the cursor is
safe.

## How BlockFile ties it together

The public API is strictly **block-granular** so the whole-blocks file
invariant cannot be broken by callers:

- `read_block` / `write_block` require exactly a 4096-byte buffer, then do
  `seek_block` + `read_all` / `write_all`.
- `block_count` = file size ÷ 4096 (via `lseek(SEEK_END)`); `read_block`
  refuses blocks at or past that count.
- `append_records` keeps "every block except possibly the last is full":
  it locates the last block's occupancy, resumes filling it, and rolls over
  into fresh blocks as needed.
- Errors follow one convention everywhere: each function clears `error` on
  entry, fills it only on failure (from `strerror(errno)` for system-call
  failures), and an **empty `error` string after the call means success**.

Everything above — one cursor per fd, partial transfers, EOF detection,
offset arithmetic — is the raw material; the block abstraction is the only
thing the rest of the engine ever sees.
