# ex10 — Heap File of Slotted Pages

A small database-engine exercise that adds the **record-file layer** on top of
the earlier stack: `Person` records live in fixed 4096-byte blocks on disk
(ex07), cached in a pool of frames (ex08) behind a buffer manager with LRU
replacement (ex09). New in ex10:

- `SlottedPage` interprets a frame's buffer as a page of fixed-length records
  with a per-slot occupancy byte and a doubly linked page chain stored in the
  page itself.
- `HeapFile` turns a chain of slotted pages into a file of records addressable
  by row id `(page id, slot)`, with create / open / scan / insert / find /
  update / erase / close and a free `erase_file`.

Two binaries come out of the build: `ex10` runs a PASS/FAIL check suite that
exercises all three layers (block file, buffer pool/manager, slotted pages,
heap file), while `ex10_demo` is the ex07-style CLI (`append`, `read`, `seek`,
`scan`, `bulk`) running on top of the buffer pool — byte-for-byte compatible
with ex07's binary files.

## Building

```bash
cmake -S . -B build
cmake --build build
```

This produces two executables:

- `build/ex10` — the PASS/FAIL check suite (222 checks), exit 0 iff all pass.
- `build/ex10_demo` — the ex07-style CLI over the buffer pool.

(CMake ≥ 4.3 and C++20 are required; CLion builds into `cmake-build-debug/`
instead — both directories are gitignored.)

## Running

```bash
./build/ex10                                  # 222 checks, ends "All checks passed."
./build/ex10_demo append sample.csv people.bin
./build/ex10_demo read people.bin 0
./build/ex10_demo seek people.bin 0 2 0 1     # list of blocks; invalid ones
                                              # are reported and skipped
./build/ex10_demo scan people.bin
./build/ex10_demo bulk 4000 people.bin
```

The check suite creates its scratch files under `/tmp/ex10-bufman-<pid>-*.bin`
and removes them at the end. It covers the manager→policy contract (through a
recording fake policy), dirty write-back on eviction, `alloc_page`, the
slotted-page layout byte-for-byte, and all nine heap-file operations including
persistence across close/reopen with a two-frame pool.

### `ex10_demo` commands

The CLI is unchanged from ex09 (same commands, same on-disk format — the
195-records-per-block layout with the `pid == 0` free-slot sentinel, *not*
slotted pages; wiring heap-file commands into the demo is a follow-up):

- `append <csv-file> <binary-file>` — load CSV rows (`pid,name,age,city`),
  skipping invalid ones with a per-line diagnostic; fills the last partial
  block first, then allocates new pages.
- `read <binary-file> <block-number>` — print one block's records.
- `seek <binary-file> <block-number>...` — print each requested block in
  order, marking pool hits, skipping invalid block numbers.
- `scan <binary-file>` — print every block, then a summary line.
- `bulk <count> <binary-file>` — append `<count>` generated records with
  sequential pids continuing from the file's contents.

## Structure

Code lives in two namespaces: everything except `Person` and the heap-file
layer is in `namespace bufman`; the new file-of-records layer introduces
`namespace heapfile`. `Person.h` stays a global struct, as in ex07.

```
ex10/
├── part/       Person record + serialization (ex07)
├── file/       BlockFile: POSIX fd wrapper, whole-block I/O (ex07)
├── bufpool/    DataFrame, BufferPool (ex08), BufferManager (ex09)
├── policy/     ReplacementPolicy interface, LRUPolicy (ex09)
├── heapfile/   SlottedPage + HeapFile (new in ex10)
├── main.cpp        check suite (no separate test target)
├── ex10_demo.cpp   the CLI demo
├── page.md     SlottedPage design plan (implemented)
└── heap.md     HeapFile design plan (implemented)
```

Record and storage layer (from ex07):

- `part/Person.h` — the 21-byte record: `pid`, `name[10]`, `age`, `city[3]`.
- `part/PersonSerializer.{h,cpp}` — field-by-field serialization; block-level
  and slot-level helpers (`record_count`, `first_free_slot`, `get_record`,
  `put_record`) over any 4096-byte buffer.
- `part/PersonCsv.{h,cpp}`, `part/PersonGenerator.{h,cpp}` — CSV loading with
  per-row diagnostics; synthetic records for bulk seeding.
- `file/BlockFile.{h,cpp}` — whole-block `read_block` / `write_block`,
  `append_records`, block counts. Errors are `bool` return + `error` string
  out-param, cleared on entry (empty = success) — the convention every layer
  below follows too.

Buffer pool and manager (from ex08/ex09):

- `bufpool/DataFrame.h` — one frame: `pin_count`, `dirty`, `char buffer[4096]`.
- `bufpool/BufferPool.{h,cpp}` — fixed set of frames; throws on misuse.
- `policy/ReplacementPolicy.h`, `policy/LRUPolicy.{h,cpp}` — pluggable
  eviction; the manager feeds `on_access` / `on_load` / `on_remove` hooks and
  asks `pick_victim` among unpinned frames.
- `bufpool/BufferManager.{h,cpp}` — page table `(fd, block)` → frame,
  dirty write-back on eviction/flush/close, and `alloc_page` as the only way
  new blocks come into existence.

Slotted pages and heap file (new in ex10):

- `heapfile/SlottedPage.{h,cpp}` — non-owning view over a frame buffer.
  Layout: records grow forward from byte 0; the last 12 bytes are
  three native-endian 4-byte integers — slot count, prev page id, next page id
  — with a one-byte-per-slot occupancy array directly below them and the dead
  space in between. For the 21-byte `Person`: capacity 185 slots, 14 bytes of
  dead space. `format` prepares a fresh page; `attach` validates the stored
  slot count against the record size; `put`/`get`/`erase` are bounds-checked;
  `kNoPage = 0xFFFFFFFF` is the chain sentinel.
- `heapfile/HeapFile.{h,cpp}` — block 0 is the header page (formatted like a
  slotted page, only its `next` field is meaningful); data pages 1, 2, … are
  doubly linked and never removed. `RowId{page_id, slot}` addresses a record;
  a row id stays valid only while its record exists (erase frees the slot and
  the next insert may reuse it). Insert walks the chain for the first free
  slot and splices a new tail page when full; scan returns a
  `HeapFileIterator` that pins one page at a time. `close` flushes through
  `close_all` (single-file pattern: one `HeapFile` per manager);
  `erase_file(manager, path)` refuses while the file is open.

## How the data flows

- **Heap-file read** (`find`, scan): `manager.pin(file, page_id)` → bind a
  `SlottedPage` to `mgr.frame(f).buffer` → `attach` → read the slot →
  `unpin` clean.
- **Heap-file write** (`insert`, `update`, `erase`): same pin/bind path, mutate
  the page (`put`/`erase`/`set_next_page`), `unpin(f, was_dirty=true)` — the
  bytes reach disk on eviction or `flush_all`/`close_all`.
- **New pages**: `alloc_page` (zeroed, dirty, pinned) then `format` — used by
  `create` for the header and by `insert` for a new tail page.
- The demo CLI bypasses the heap-file layer: it reads/writes records through
  the same frames but with the ex07 serializer helpers, over ex07-format
  files.

Design details, byte-layout tables, and the test plan: see `EXPLAIN.md`.
