# EXPLAIN.md — how ex09 works, module by module

This document explains the technical elements of `ex09` in detail: what each
module does, why it is built the way it is, and how the layers cooperate.
It is written to be read top to bottom — later sections build on earlier
ones. For quick build/run instructions see `README.md`; for the design
deliberations that led here see `bufman.md`, `serialized.md` and `demo.md`.

## Intro

`ex09` is the ninth exercise in a course that builds a small database engine
piece by piece. The earlier exercises produced:

- `Person` records (pid, name, age, city) and their 21-byte on-disk layout,
- a block file layer that stores those records in fixed 4096-byte blocks
  (`ex07`),
- a buffer pool of fixed-size frames with pin counts and dirty flags
  (`ex08`).

`ex09` copies both layers into one project (deliberately duplicated per
exercise, not shared), unifies everything in a single namespace `bufman`,
and adds the piece that turns the parts into a small database kernel: a
**buffer manager** that decides which block lives in which frame, evicts
through a **pluggable replacement policy** (LRU included), writes dirty
frames back to disk, and can allocate brand-new pages.

The full stack, top to bottom:

```text
ex09_demo.cpp (CLI)          main.cpp (check suite)
        │                            │
        ▼                            ▼
  BufferManager  ── owns ──▶  BufferPool ── array of ──▶ DataFrame
        ││                                          (4 KB buffer each)
        ││ uses                    ▲
        │▼                         │ slot-level record access
        │ BlockFile ◀── PersonSerializer ── Person
        ▼
     the file on disk (4096-byte blocks, 21-byte records)
```

The one rule that keeps the design honest: **data crosses the disk boundary
in exactly one way per direction.** Reading a block means
`BlockFile::read_block` into a frame; writing a block means a dirty frame's
buffer goes out through `BlockFile::write_block`. Nothing else touches the
file bytes.

Two binaries are built from the same modules:

- `ex09` — a PASS/FAIL check suite (72 checks) plus a demo scan.
- `ex09_demo` — the `ex07`-style CLI (`append`, `read`, `scan`, `bulk`),
  byte-for-byte compatible with ex07's files.

Error handling follows two deliberate conventions:

- BlockFile and BufferManager report failures through an `error` string
  parameter that is **cleared on entry and filled only on failure**, so an
  empty string after the call means success and callers may reuse one string
  across calls.
- BufferPool throws exceptions (`std::out_of_range` for bad indexes,
  `std::logic_error` for unpin-at-zero); BufferManager lets them propagate.

## Person

`Person.h` is the record the whole engine stores. It is a plain C struct
with fixed-size arrays, deliberately copied into each exercise that needs it
rather than shared:

```cpp
typedef struct Person {
    int pid;          // positive; 0 means "free slot"
    char name[10];    // up to 9 characters + NUL
    int age;          // nonnegative
    char city[3];     // up to 2 characters + NUL
} Person;
```

Technical points worth understanding:

- **Fixed arrays, not `std::string`.** A record must always occupy the same
  number of bytes so it can be packed into slots. The array sizes bound the
  data: 9 characters of name, 2 of city (both NUL-terminated).
- **`pid == 0` is the free-slot sentinel.** CSV validation requires
  `pid > 0`, and the serializer uses a zeroed slot to mean "empty". This is
  what makes variable occupancy inside a fixed-size block possible without
  any extra metadata: scan slots until the first `pid == 0`.
- **The struct is *not* written to disk as-is.** The compiler may insert
  padding between members, and that padding is not guaranteed to be stable
  across compilers or platforms. `PersonSerializer` therefore copies each
  field individually into a gap-free layout (next section).

## PersonSerializer

This module owns the byte format. Three constants define it:

```cpp
constexpr std::size_t kRecordSize     = sizeof(int) + 10 + sizeof(int) + 3; // 21
constexpr std::size_t kBlockSize      = 4096;
constexpr std::size_t kRecordsPerBlock = kBlockSize / kRecordSize;           // 195
```

One 4096-byte block holds **195 records** (195 × 21 = 4095, one byte of
slack at the end, which simply stays zero).

### Record layout

`serialize` copies the four fields back to back at fixed offsets —
`pid` (4 bytes), `name` (10), `age` (4), `city` (3) — using `memcpy` per
field. This is why struct padding never leaks into the file: only this
explicit, gap-free layout is written, and `deserialize` walks the same
offsets in the same order. As long as both sides use the same layout, the
file is portable even though the in-memory struct is not.

### Block-level functions

- `initialize_block(block)` — zero-fills all 4096 bytes. Because a zeroed
  record has `pid == 0`, every unused slot reads back as empty.
- `serialize_block(persons, block)` — zero-fills, then lays out up to 195
  records back to back (record *i* starts at byte `i * kRecordSize`).
- `deserialize_block(block)` — returns the records of a block as a vector,
  stopping at the first `pid == 0` slot.

### Slot-level helpers (the frame bridge)

These four functions were added in this exercise so that queries can work
on any block-sized buffer — crucially, a `DataFrame`'s buffer, which is a
plain `char[4096]` and therefore plugs straight in:

```cpp
std::size_t record_count(const char* block);                 // occupied slots
std::optional<std::size_t> first_free_slot(const char* block); // nullopt if full
bool get_record(const char* block, std::size_t slot, Person& out);
bool put_record(char* block, std::size_t slot, const Person& person);
```

All of them bounds-check `slot < kRecordsPerBlock`. `get_record` returns
false for an empty slot; `put_record` refuses a `Person` with `pid == 0`
(writing it would corrupt the free-slot sentinel and silently truncate the
block). They are one-liners over `serialize`/`deserialize` — their value is
that the slot arithmetic lives in exactly one place.

## PersonCsv

Loads a CSV file into memory, mirroring ex07's rules exactly:

```cpp
struct LoadResult {
    std::vector<Person> people;   // valid rows, in file order
    std::size_t skipped = 0;      // rejected rows (and blank lines)
};
LoadResult load(const std::string& path, std::ostream& diagnostics);
```

Parsing details:

- Each line must have **exactly four** comma-separated fields. This is
  enforced by a chain of `std::getline(..., ',')` calls plus one extra
  `getline` that must *fail* — a fifth field makes the line invalid.
- Integers are parsed with `std::from_chars` and must be consumed to the
  very last character (`result.ptr == last`), so `"12x"` or an empty field
  is rejected — no silent truncation, no locale-dependent streams.
- Validation: `pid > 0`, `age >= 0`, name ≤ 9 characters, city ≤ 2
  characters. Any violation produces a reason string, the row is skipped,
  and `line N: reason` goes to the diagnostics stream (the demo sends this
  to stderr).
- Blank lines count as skipped rows.
- An unopenable file is not fatal: it reports to diagnostics, sets
  `skipped = 1`, and returns no people — which makes the CLI exit 1 (see
  `ex09_demo.cpp`).

## PersonGenerator

Produces synthetic records for bulk-insert testing:

```cpp
std::vector<Person> generate(std::size_t count, int first_pid);
```

- pids are sequential: `first_pid + i`. The CLI uses
  `record_count(file) + 1` as `first_pid`, so repeated `bulk` runs never
  reuse a pid.
- `age = 18 + (pid % 63)` — deterministic, spread over 18..80.
- The name is `"P" + pid` (e.g. `P4001`), truncated to 9 characters.
- The city cycles through a fixed list of 8 two-letter codes (`PR`, `NY`,
  `LA`, `TX`, `FL`, `CA`, `WA`, `OH`) by record index.

Everything is deterministic on purpose: tests can assert exact values, and
byte-parity with ex07's generator output is verifiable.

## BlockFile

The lowest layer: raw POSIX file I/O, wrapped so the rest of the code never
sees a file descriptor except through this module. A file is represented by

```cpp
struct File { int fd = -1; };   // -1 means "not open"
```

- `open_for_append(path)` — `open(path, O_RDWR | O_CREAT, 0644)`: read and
  write access, creates the file if needed (this is what lets `append`/`bulk`
  work on new paths).
- `open_for_read(path)` — `O_RDONLY`: opening a
  missing file fails, which is exactly the behavior the CLI's `read`/`scan`
  need.
- `close(file)` — closes and resets to -1.

### Robust read/write loops

POSIX `read`/`write` are allowed to transfer **fewer bytes than requested**
(signals, pipe limits, disk quirks). `write_all` and `read_all` loop until
the full count has moved, retry on `EINTR`, and treat a zero-byte result as
an error (`"write: no progress"` / `"read: unexpected end of file"`) rather
than spinning forever. Every block transfer in the module goes through one
of these two helpers.

### Block addressing

A block number becomes a byte offset with `lseek(fd, block_number * 4096,
SEEK_SET)`. `seek_block` guards against overflowing `off_t` before doing
the multiplication — a small check that documents the 64-bit assumption.
`file_size` seeks to the end and enforces the module's core invariant:

> **The file is always a whole number of 4096-byte blocks.** A size that is
> not a multiple of 4096 is corruption and is rejected outright.

### Reading and writing blocks

- `read_block(file, n, buffer, length, error)` — the raw-pointer variant.
  Requires a full 4096-byte buffer, checks `n < block_count`, seeks, and
  reads. The `std::array` overload is a thin wrapper.
- `write_block(file, n, buffer, length, error)` — block-granular only
  (`length == kBlockSize`): seek to the block's offset and write. Writing
  past the current end is allowed and leaves a zero-filled gap, which keeps
  the whole-blocks invariant (the file size is still a block multiple).
- `block_count(file)` — file size ÷ 4096.
- `record_count(file)` — total records in the file. It relies on the
  invariant: every block except possibly the last is full, so it reads
  **only the last block** and computes
  `(blocks − 1) × 195 + occupancy(last)`. One block read instead of N.
- `append_records(file, records, length)` — appends an already-serialized
  byte buffer (records packed back to back). It finds where to resume
  (inside the last block if it has free slots, otherwise a fresh block at
  the end), copies records one at a time into a block image, flushes the
  block when it fills, and writes the final partial block back. In ex09's
  demo this path is no longer used for CLI appends (the buffer manager does
  it page by page), but it remains the seeding primitive for tests.

## DataFrame

One frame of the buffer pool:

```cpp
constexpr std::size_t kPageSize = 4096;

struct DataFrame {
    int pin_count = 0;            // how many queries use this frame now
    bool dirty = false;           // buffer modified, not yet saved
    char buffer[kPageSize] = {};  // the page itself
};
```

- The buffer is a **static array**, not a heap allocation: a frame's size is
  fixed by definition, and `buffer` decays to `char*` for the serializer and
  BlockFile — no smart pointer needed because the frame owns its storage.
- `pin_count` and `dirty` are the entire bookkeeping a frame needs. Who owns
  the *data identity* of a frame (which file, which block) is deliberately
  **not** stored here — that lives in the BufferManager's page table. The
  ex08 module stays a dumb frame container; all policy and mapping
  intelligence sits above it.
- `kPageSize` and PersonSerializer's `kBlockSize` are two names for the same
  4096: one names a frame's capacity, the other a file block's size.
  `BufferManager.cpp` asserts their equality with `static_assert`.

## BufferPool

A fixed set of frames, nothing more:

```cpp
class BufferPool {
public:
    void init(std::size_t n);                    // n zeroed, clean frames
    DataFrame& frame(std::size_t index);         // throws out_of_range
    const DataFrame& frame(std::size_t index) const;
    void set_dirty(std::size_t index, bool dirty);
    void pin(std::size_t index);                 // pin_count++
    void unpin(std::size_t index);               // throws logic_error at 0
    std::size_t size() const;
private:
    std::vector<DataFrame> frames_;
};
```

Design points:

- `init` uses `assign`, so it resizes *and* resets: every frame starts with
  `pin_count == 0`, `dirty == false`, zeroed buffer. Calling it again is a
  full pool reset.
- Bounds checking is centralized in `frame()` (both overloads); every other
  method goes through it, so an invalid index always throws
  `std::out_of_range` exactly once.
- `unpin` on a frame whose pin count is already zero throws
  `std::logic_error` — an unbalanced unpin is a caller bug, not a state to
  silently absorb.
- There is **no I/O, no eviction, no policy** here. The pool is the data
  structure; everything with judgment belongs to the manager.

## ReplacementPolicy

The interface that makes eviction swappable:

```cpp
class ReplacementPolicy {
public:
    virtual ~ReplacementPolicy() = default;
    virtual void init(std::size_t pool_size) = 0;
    virtual void on_access(std::size_t frame) = 0;  // pin hit on a resident page
    virtual void on_load(std::size_t frame) = 0;    // a page was placed in the frame
    virtual void on_remove(std::size_t frame) = 0;  // the page left the frame
    virtual std::optional<std::size_t> pick_victim(
        const std::vector<std::size_t>& candidates) const = 0;
};
```

The contract, and why it is shaped this way:

- **The manager filters; the policy chooses.** `candidates` contains only
  frames that currently hold a page *and* have `pin_count == 0`. The policy
  never sees a pinned frame and never touches `BufferPool` — so a policy is
  a self-contained data structure over frame indices, trivially unit-testable
  (the check suite drives it with a recording fake).
- **`pick_victim` is `const`** — choosing a victim must not mutate the
  policy; all bookkeeping happens in the hooks.
- **`nullopt` means "no candidate"** — the manager translates that into the
  `"all frames pinned"` error.
- Free (never-used) frames never reach the policy: the manager consumes them
  first, because a free frame never costs a disk write. The policy only
  arbitrates between pages worth keeping.
- The policy is injected as `std::unique_ptr<ReplacementPolicy>` — the one
  place in the project where a smart pointer is the right tool: uniquely
  owned, polymorphic at runtime, lifetime exactly the manager's.
  MRU and LFU policies would be drop-in subclasses; only LRU ships today.

## LRUPolicy

Least-recently-used, in the classic list + index form:

```cpp
std::list<std::size_t> order_;                                 // MRU front, LRU back
std::vector<std::list<std::size_t>::iterator> positions_;      // O(1) entry per frame
```

- `on_access` and `on_load` both call the private `touch(frame)`: erase the
  frame from the list if present, push it to the front, remember the new
  iterator. Both operations are O(1) thanks to the stored iterator.
- `on_remove` erases the frame and marks its slot with `end()` — the "not
  tracked" sentinel. A frame that is not in the list holds no page.
- `pick_victim(candidates)` walks the list **from the back** (least recently
  used first) and returns the first frame that appears in `candidates`.
  The candidate scan is O(n) worst case — irrelevant at course scale — but
  the ordering logic is a single reverse walk, which is the whole algorithm.
- The LRU order update happens on *pin*, not on *unpin*: recency tracks the
  last access by a query, which is the standard semantic.

## BufferManager

The module that turns frames + file + policy into a small database kernel.

### State

```cpp
BufferPool pool_;
std::unique_ptr<ReplacementPolicy> policy_;
std::unordered_map<PageKey, std::size_t, PageKeyHash> page_table_;
std::vector<std::optional<PageKey>> frame_page_;   // reverse map, per frame
std::vector<bufman::File> files_;                  // handles opened through us
```

- `PageKey { int fd; std::uint64_t block_number; }` identifies a page by
  file descriptor and block number, with a custom hash combining both.
  Files are assumed to stay open for the manager's lifetime; `close_all`
  clears all state so a reopened file (new fd) can never alias stale
  entries.
- The page table answers "is block B of file F in the pool, and where?".
  The reverse map answers the opposite question per frame — and an **empty
  optional means "this frame was never used (or was evicted)"**, which is
  the free-frame signal.

### Opening files

`open_file` (read/write, creates) and `open_file_read` (read-only, fails on
missing) both register the handle in `files_`. Registration matters: when a
dirty victim must be written back, the manager needs the file handle that
matches the victim page's fd — `find_file(fd)` searches this vector.

### Finding a victim

`find_victim_frame()` implements the two-tier rule:

1. any frame with an empty reverse entry (never used) — taken in index
   order, no policy consultation, because reusing it can never cost a disk
   write;
2. otherwise, collect the page-holding unpinned frames and ask the policy's
   `pick_victim`.

### Eviction: `evict_frame`

Shared by `pin` (miss) and `alloc_page`. If the frame holds a page: write
the buffer back with `BlockFile::write_block` **if dirty**, erase the page
table entry, fire `on_remove`, clear the reverse entry. After it returns,
the frame is anonymous again.

### `pin(file, block, frame, error)` — read path

1. **Hit**: the page table has `{fd, block}` → return that frame with
   `pin_count` incremented and `on_access` fired. No disk I/O.
2. **Miss**: `find_victim_frame()`; none available → `"all frames pinned"`.
3. `evict_frame(victim)` (may write a dirty page back — this is the moment
   eviction I/O happens).
4. `BlockFile::read_block(file, block, frame.buffer)` fills the frame.
   If the read fails, the frame is deliberately left free (metadata was
   already cleared) so the failure does not poison the pool.
5. Register `{fd, block} → frame`, mark clean, `pin`, `on_load`.

Note what `pin` does *not* do: it never creates a block. Reading a block
that does not exist fails with BlockFile's `"requested block is beyond end
of file"`.

### `alloc_page(file, block, frame, error)` — the new-page operation

This is the analogue of a real DBMS's new-page call (`buf_page_create` in
InnoDB, `new_page` in textbook designs). Appends in a real database do go
through the buffer pool — but a new page cannot be *read* from disk, so a
distinct operation creates it:

1. Refuse if the block is already resident ("already resident in the pool")
   or already exists on disk (`block_number < block_count` → "block already
   exists on disk"). `alloc_page` is strictly for brand-new pages.
2. Take a victim frame (same free-first/policy rules) and `evict_frame` it.
3. `initialize_block(frame.buffer)` — zeroed page.
4. Install `{fd, block} → frame`, mark the frame **dirty** immediately (a
   new page must reach disk even if the caller writes nothing), `pin`,
   `on_load`.

From this point the page is indistinguishable from a read one: records are
`put_record`-ed into its buffer, and ordinary dirty write-back puts it on
disk.

### `unpin(frame, was_dirty, error)`

Decrements the pin count; `was_dirty=true` sets the frame's dirty flag
first. This is the query's only way of saying "I changed this page" — and
it is deliberately decoupled from the serializer: `put_record` writes bytes
into a buffer and knows nothing about pools.

### `flush` / `flush_all` / `close_all`

- `flush(frame)` — if the frame holds a page and is dirty, `write_block` it
  and clear the flag; a clean or free frame is a no-op success.
- `flush_all` — every frame in index order.
- `close_all` — flush, close all handles, clear the page table and reverse
  entries, re-`init` the policy. A flush failure is reported through
  `error`, but teardown continues: the file descriptors are going away
  either way, and half-torn state would be worse.

### Why this keeps byte parity with ex07

The demo CLI reproduces ex07's on-disk bytes exactly because every write is
a 4096-byte block image whose record region follows the same 21-byte slot
layout, blocks are allocated densely (no gaps), and free slots are zeroed
by `initialize_block`. Verified with `cmp` against files written by ex07's
own tool.

## main.cpp

The check suite: one binary, no test framework. It follows the ex08
pattern:

```cpp
void check(bool condition, const std::string& message) {
    if (condition) std::cout << "PASS: " << message << "\n";
    else { std::cout << "FAIL: " << message << "\n"; ++failures; }
}
```

A failed check does not abort — failures are counted, the program prints a
summary and exits 1 only if anything failed. Scratch files live under
`/tmp` with the process id in the name (`temp_path`) and are removed at the
end; a few helpers seed files and read disk bytes back through a separate
BlockFile handle (`seed_file`, `seed_person_block`, `read_disk_byte`,
`read_disk_block`, `make_person`).

The 72 checks, grouped:

1. **Buffer manager core** — miss loads bytes from disk, hits return the
   same frame and stack pins, pin/unpin accounting, LRU eviction order
   (re-pinning block 0 promotes it so block 1's frame is evicted, not
   block 0's), "all frames pinned" refusal, dirty write-back on eviction
   verified both by reloading and by reading the disk through an
   independent handle, never-used-first allocation, flush write-through and
   clean-flush no-op, post-`close_all` failure, `unpin`-at-zero throwing.
2. **Policy contract** — a `RecordingPolicy` fake records every hook call:
   hits fire `on_access`, misses fire `on_remove` (if evicting) then
   `on_load`, and `pick_victim` receives only unpinned page-holding frames
   (pinned frames provably excluded).
3. **Person access through frames** — seeded blocks read back field by
   field via `get_record` on `mgr.frame(f).buffer`; an update survives
   write-back + reload; appends fill free slots 0, 1, … and persist; a
   block filled with all 195 slots reports `first_free_slot == nullopt`;
   out-of-range slots and `pid == 0` are rejected; the demo scan shows the
   caching working (second pass hits the same frames).
4. **`alloc_page`** — new pages start zeroed, dirty, pinned; resident and
   on-disk blocks are refused; a pool of one forces the second allocation
   to evict the dirty first page and write it back; both pages verify on
   disk.

## ex09_demo.cpp

The CLI binary, with behavior identical to ex07's `main.cpp` — same four
commands, same argument handling, same stdout/stderr strings, same exit
codes. Verified by running both binaries on identical inputs and diffing
outputs *and* comparing the produced files with `cmp` (byte-identical).

```text
ex09_demo append <csv-file> <binary-file>
ex09_demo read <binary-file> <block-number>
ex09_demo seek <binary-file> <block-number>...
ex09_demo scan <binary-file>
ex09_demo bulk <count> <binary-file>
```

Every command creates a `BufferManager` with `kPoolSize` (8) LRU frames —
the number of frames is the knob that controls how much of the file stays
cached in memory.

- **`append`** — `PersonCsv::load` with diagnostics on stderr (invalid rows
  are skipped, never fatal; a file with only invalid rows exits 1, a clean
  empty CSV exits 0). Valid records then go through
  `append_people(mgr, file, people)`:
  1. `pin` the **last block**; `record_count` on its buffer finds the first
     free slot; `put_record` fills free slots there.
  2. For records that do not fit, `alloc_page(file, blocks)` creates one
     zeroed page after another; each is filled with up to 195 records.
  3. Every touched frame is unpinned **dirty**; `close_all` flushes before
     the command returns, so the data is on disk when the process exits.
  This is the real-DBMS append shape: no record ever bypasses the pool; new
  blocks exist only through the manager's new-page operation.
- **`read`** — opens the file **read-only** (`open_file_read`, so a missing
  file errors instead of being created), `pin`s the block, prints
  `block N: M record(s)` plus one `slot i:` line per record from
  `deserialize_block(frame.buffer)`, unpins.
- **`scan`** — same, for every block from `block_count`, then a
  `scanned B block(s), R record(s)` summary. In a pool smaller than the
  file this naturally exercises eviction; repeat scans hit the pool.
- **`seek`** — reads a list of blocks in command-line order, duplicates
  included. Before each `pin`, `BufferManager::resident(file, block)` says
  whether the page table already holds the block; a hit prints the read
  output with a `(pool hit)` marker, a miss prints it plain. Failures
  (unparsable number, block beyond EOF) print `block <arg>: <reason>` on
  stderr and the loop continues — the command exits 1 only if anything
  failed. It is the most direct demonstration of the pool cache: ask for
  the same block twice and the second answer comes from memory.
- **`bulk`** — `record_count(file)` (one block read, thanks to the
  whole-blocks invariant) gives the existing count; `generate(count,
  existing + 1)` makes synthetic records with continuing pids; then the
  same `append_people` path as `append`.

Error handling matches ex07 exactly: usage on stderr + exit 1 for wrong
arguments, the manager's/BlockFile's error string on stderr + exit 1 for
I/O failures (`open for read: No such file or directory`, `requested block
is beyond end of file`, …).
