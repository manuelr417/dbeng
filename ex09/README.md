# ex09 — Buffer Manager

A small database-engine exercise that adds a **buffer manager** on top of the
earlier layers: `Person` records live in fixed 4096-byte blocks on disk
(ex07 format), blocks are cached in a pool of frames (ex08), and the buffer
manager moves blocks between disk and frames with pin counting, dirty
write-back, and a pluggable replacement policy (LRU included).

Two binaries come out of the build: `ex09` runs a PASS/FAIL check suite that
exercises every layer and finishes with a demo scan of a Person file through
the buffer pool, while `ex09_demo` is the ex07-style CLI (`append`, `read`,
`seek`, `scan`, `bulk`) running on top of the buffer pool, byte-for-byte
compatible with ex07's binary files.

## Building

```bash
cmake -S . -B build
cmake --build build
```

This produces two executables:

- `build/ex09` — the PASS/FAIL check suite + demo scan.
- `build/ex09_demo` — the ex07-style CLI (`append`, `read`, `seek`, `scan`,
  `bulk`) running on top of the buffer pool; byte-for-byte compatible with
  ex07's binary files.

(CMake ≥ 4.3 is required; CLion builds into `cmake-build-debug/` instead —
both directories are gitignored.)

## Running

```bash
./build/ex09                     # 74 checks + demo scan, exit 0 iff all pass
./build/ex09_demo append sample.csv people.bin
./build/ex09_demo read people.bin 0
./build/ex09_demo seek people.bin 0 2 0 1   # list of blocks; invalid ones
                                            # are reported and skipped
./build/ex09_demo scan people.bin
./build/ex09_demo bulk 4000 people.bin
```

The program creates its scratch files under `/tmp`, runs 74 checks, prints a
demo scan, and exits 0 only if every check passed:

```
PASS: person block 0 pins into frame 0
...
scan of /tmp/ex09-bufman-<pid>-people.bin through the buffer pool:
  block 0 (frame 0): 2 record(s)
    slot 0: pid=1, name=Ana, age=21, city=PR
    slot 1: pid=2, name=Bob, age=31, city=NY
  block 1 (frame 1): 2 record(s)
    ...
  block 2 (frame 2): 195 record(s)
  scanned 3 block(s), 199 record(s)
PASS: second scan pass hits the same frames (pool hits)

All checks passed.
```

## Structure

All code except `Person` lives in `namespace bufman` (one namespace per
project, per repo convention); `Person.h` stays a global struct, as in ex07.

Storage layer (block file I/O, from ex07):

- `Person.h` — the 21-byte record: `pid`, `name[10]`, `age`, `city[3]`.
- `PersonSerializer.{h,cpp}` — field-by-field serialization into the on-disk
  layout; 4096-byte block = 195 records, free slots zeroed (`pid == 0` is the
  free-slot sentinel). Slot-level helpers (`record_count`, `first_free_slot`,
  `get_record`, `put_record`) operate on any block-sized buffer — including a
  frame's.
- `PersonCsv.{h,cpp}` — CSV loading with per-row diagnostics.
- `PersonGenerator.{h,cpp}` — synthetic records for bulk seeding.
- `BlockFile.{h,cpp}` — POSIX fd wrapper: whole-block `read_block` /
  `write_block` (raw-pointer variants), `append_records` of pre-serialized
  records, block counts. Error reporting via `error` strings that are
  cleared on entry (empty = success).

Buffer pool (from ex08):

- `DataFrame.h` — one frame: `pin_count`, `dirty`, `char buffer[4096]`.
- `BufferPool.{h,cpp}` — fixed set of frames with `pin` / `unpin` /
  `set_dirty`; throws `out_of_range` / `logic_error` on misuse.

Buffer manager (new in ex09):

- `ReplacementPolicy.h` — abstract class fixing the policy API (`init`,
  `on_access`, `on_load`, `on_remove`, `pick_victim`).
- `LRUPolicy.{h,cpp}` — list-based LRU; evicts the least recently used
  unpinned frame.
- `BufferManager.{h,cpp}` — owns the pool; page table maps
  `(fd, block number)` → frame; never-used frames are filled first, then the
  policy chooses among unpinned frames; dirty victims are written back
  before reuse. `alloc_page(file, block)` creates brand-new pages (zeroed
  buffer, registered, dirty, pinned) — the only way new blocks come into
  existence. API: `init`, `open_file`, `open_file_read`, `pin`, `alloc_page`,
  `unpin(frame, was_dirty)`, `frame`, `flush`, `flush_all`, `close_all`.

Tests, demo and design docs:

- `main.cpp` — the check suite + demo (no separate test target).
- `ex09_demo.cpp` — the ex07-style CLI; all commands go through the buffer
  manager (`read`/`scan` pin blocks read-only; `append`/`bulk` fill the last
  partial block and allocate new pages via `alloc_page`; `seek` reads a list
  of blocks in order, marking pool hits and skipping invalid ones).
- `bufman.md` — BufferManager design plan (implemented).
- `serialized.md` — plan for Person access through frame buffers
  (implemented).
- `demo.md` — plan for the CLI demo and its parity verification against
  ex07 (implemented; outputs and files verified byte-identical).

## How the data flows

- **Read**: `pin(file, block)` → cache hit returns the frame; miss evicts an
  LRU unpinned frame (writing it back first if dirty) and fills the frame
  via `BlockFile::read_block`. Records are then read straight out of
  `mgr.frame(f).buffer` with the serializer helpers.
- **Write**: modify the frame buffer with `put_record` →
  `unpin(f, was_dirty=true)` → the block reaches disk when the frame is
  evicted or `flush`/`flush_all`/`close_all` runs — always through
  `BlockFile::write_block`.
- **New blocks**: `pin` past EOF fails by design — `alloc_page(file, block)`
  is the creation path (free frame, zeroed buffer, dirty, pinned), mirroring
  the new-page operation of real database buffer managers. Appends fill the
  last partial block first, then allocate pages one by one.

Design details and rejected alternatives: see `bufman.md` and
`serialized.md`.
