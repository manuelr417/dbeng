# EXPLAIN.md — how ex10 works: SlottedPage and HeapFile

This document explains the technical design and implementation of the two
modules added in `ex10`: the **SlottedPage** (a fixed-length-record page with
a per-slot occupancy array and chain links) and the **HeapFile** (a file of
records built from a chain of such pages, addressable by row id). It is
written to be read top to bottom — the HeapFile section assumes the
SlottedPage section. For quick build/run instructions see `README.md`. The
document is self-contained: it states the requirements both modules were
built against, the full on-disk formats, every design decision with its
rationale, and what the check suite verifies.

## Intro

`ex10` is the tenth exercise in a course that builds a small database engine
piece by piece. The earlier exercises produced:

- `Person` records and their 21-byte on-disk layout, stored in fixed
  4096-byte blocks (`ex07`),
- a buffer pool of frames with pin counts and dirty flags (`ex08`),
- a buffer manager with LRU replacement, dirty write-back, and an
  `alloc_page` new-page operation (`ex09`).

`ex10` adds the layer that turns pages into a *file of records*:

```text
main.cpp (check suite)
        │
        ▼
     HeapFile ── owns the chain ──▶ SlottedPage (a view over one frame)
        │                                │
        │ pins pages through             │ interprets bytes:
        │ BufferManager                  │ slots, occupancy, prev/next
        ▼                                ▼
  BufferManager ── frames ──▶ DataFrame (4 KB buffer each) ──▶ BlockFile
                                                                     │
                                                          the file on disk
```

The division of labor is strict:

- **SlottedPage manages layout only.** It reads and writes bytes inside one
  4096-byte buffer. It performs no I/O, holds no file handle, and knows
  nothing about pins, dirty flags, or eviction.
- **HeapFile manages the file.** It owns the chain topology (which page
  follows which), walks pages one pin at a time through the injected
  `BufferManager`, and never interprets bytes itself — every byte-level
  operation is delegated to a `SlottedPage` bound to the frame it pinned.

Both follow the codebase's error convention: a `bool` return plus an
`error` string out-param that is **cleared on entry and filled only on
failure** — an empty string after the call means success, and callers may
reuse one string across calls.

Out of scope for both modules, by explicit decision: variable-length
records, page compaction, a free-page list or whole-page deletion,
concurrency, crash recovery, and generic (non-`Person`) record types.

## Part A — SlottedPage

### The problem the page solves

The assignment, distilled: add a class that represents a slotted page of
records for a heap file whose pages form a doubly linked list on disk. Each
page has an id — the number of pages to skip from the beginning of the file
to reach it — and the buffer manager hands the class a raw 4096-byte buffer
to interpret. The required layout logic:

- the **last 4 bytes** hold an integer: the page id of the *next* page;
- the **4 bytes before it**: the page id of the *previous* page;
- the **4 bytes before that**: an integer, the number of slots (records)
  the page can store;
- before those sits an **occupancy array** with exactly one byte per slot,
  `0` = no record in the slot, `1` = occupied;
- the number of records depends on the record length plus **one extra byte
  per slot** of bookkeeping, and between the record slots and the occupancy
  array there is an array of **unused bytes (dead space)** too small to fit
  one more record.

### Design principles

Five deliberate choices, all favoring clarity over performance:

- **One occupancy byte per slot — never bit-packed.** The waste (185 bytes
  out of 4096) buys an array you can read at a glance and assert
  byte-exactly in tests.
- **Derived values are recomputed, not cached.** No running occupied/free
  counters anywhere; a counter can drift from the bytes, the bytes cannot
  drift from themselves.
- **Minimal object state.** Buffer pointer, record size, capacity —
  everything else lives in the page bytes, so there is one source of truth.
- **Byte math written once, with names.** Small helpers and fixed header
  constants replace scattered arithmetic.
- **No clever encodings.** Plain `memcpy` of native-endian integers, no
  endianness handling (single-host format), no free-count in the header, no
  bump-pointer dead space, no linked-list shortcuts.

### Role and boundaries

A slotted page is the unit of record storage inside one 4 KB block. The
class is deliberately a **non-owning view**: it binds to a `char*` (in
practice `mgr.frame(f).buffer`) plus a record size, and its only members are

```cpp
char* buffer_ = nullptr;
std::size_t record_size_ = 0;
std::size_t capacity_ = 0;   // unset (0) until format() or attach() runs
```

Nothing derived is cached — occupied counts, free slots, and chain links are
all recomputed from the page bytes on every call. The bytes are the single
source of truth, which removes a whole class of "counter drifted from
reality" bugs and makes every behavior directly assertable in tests. The
cost (an O(C) linear scan per call, C ≤ 185) is irrelevant at course scale.

### The page byte layout

Fixed-size records grow forward from the start of the page; the header grows
backward from the end. With `R` = record size and `C` = slot capacity:

| Region          | Offset range                           | Contents                                  |
|-----------------|----------------------------------------|-------------------------------------------|
| Records         | `[0, R*C)`                             | slot `i` occupies `[i*R, (i+1)*R)`         |
| Dead space      | `[R*C, R*C + D)`                       | `D` unused bytes, too small for a record   |
| Occupancy array | `[kPageSize-12-C, kPageSize-12)`       | one byte per slot: `0` free, `1` occupied  |
| Slot count      | `[kPageSize-12, kPageSize-8)`          | 4-byte integer: `C`                        |
| Prev page id    | `[kPageSize-8, kPageSize-4)`           | 4-byte integer: previous page in the chain |
| Next page id    | `[kPageSize-4, kPageSize)`             | 4-byte integer: next page in the chain     |

```text
byte offset:  0 ............ R*C ...... R*C+D ........ kPageSize-12-C .. kPageSize-12 .. kPageSize-8 .. kPageSize-4 .. kPageSize
              |  slot 0 | ... | slot C-1 | dead | occ[0] .. occ[C-1] | slot count |  prev page id |  next page id |
```

The constants name every fixed offset once:

```cpp
constexpr std::size_t kPageHeaderSize   = 12;
constexpr std::size_t kSlotCountOffset  = bufman::kPageSize - kPageHeaderSize; // 4084
constexpr std::size_t kPrevPageIdOffset = bufman::kPageSize - 8;
constexpr std::size_t kNextPageIdOffset = bufman::kPageSize - 4;
constexpr std::uint32_t kNoPage         = 0xFFFFFFFFu;  // "no page this way"
```

`kNoPage` is the chain-end sentinel. A page id of `0` is *legal* (it is the
header page), so `0` can never mean "no page" — hence a value that no real
page id can take. The three header integers are copied with `memcpy`
(native endianness), matching the existing serializer's single-host approach;
there is deliberately no endianness handling.

### Capacity and dead space

Each slot costs the record plus its occupancy byte:

```cpp
C = floor((kPageSize - 12) / (R + 1));
D = (kPageSize - 12) - C * (R + 1);      // 0 <= D <= R by construction
```

For the 21-byte `Person`: `C = floor(4084 / 22) = 185`, `D = 14`. The sanity
identity `185*21 + 14 + 185 + 12 = 4096` fills the page exactly, and the
check suite verifies the formula for other record sizes (`R = 1` packs 2042
slots with no dead space; `R = 25` packs 157 slots with 2 bytes left over).
The dead space exists because the header is a fixed 12 bytes taken off the
top — the remainder after whole slots is always smaller than one more
slot-plus-byte, so it is simply left unused.

### Lifecycle: bind → format or attach

- **`bind(buffer, record_size)`** stores the pointer and record size and
  resets `capacity_` to 0. It touches no bytes. Every slot operation is
  bounds-checked against `capacity_`, so a page used before `format`/`attach`
  has capacity 0 and rejects every slot index — misuse fails closed rather
  than corrupting memory.
- **`format(prev = kNoPage, next = kNoPage)`** prepares a fresh page: writes
  the slot count derived from the record size, zeroes the occupancy array,
  and stores the two chain ids (defaulting both directions to `kNoPage`).
  Record bytes and dead space are left untouched — a fresh page's records are
  already zeroed by `alloc_page`, and formatting an existing page is not
  something the heap layer ever does except on a brand-new page.
- **`attach(error)`** binds to *existing* bytes: it reads the stored slot
  count and compares it against the value recomputed from the bound record
  size. A mismatch is a hard error
  (`"slotted page header holds slot count N, expected M for record size R"`)
  because it means corrupted or foreign bytes — for example, block 0 of a
  file written in the old ex07 serializer format keeps record bytes in the
  region where the slot count would live, so `attach` reads garbage and
  refuses. HeapFile::open reuses exactly this check to validate that a file
  really is a heap file.

### Slot arithmetic

Two private helpers own all the byte math:

- `record_offset(slot) = slot * record_size_` — records start at byte 0.
- `occupancy_offset(slot) = kSlotCountOffset - capacity_ + slot` — the
  occupancy array sits **directly below the header**: `occ[0]` is at
  `kPageSize - 12 - C`, and `occ[C-1]` ends exactly at `kSlotCountOffset`.
  The layout tests assert both endpoints byte-exactly.

### Chain navigation

`prev_page()` / `next_page()` read the two header ids; `set_prev_page()` /
`set_next_page()` write them. That is the entire doubly-linked-list API the
page offers — splicing pages in and out is the *file's* job (Part B), the
page only knows how to store and retrieve the ids.

### Slot inspection

All inspection is a linear scan over the occupancy array, recomputed per
call:

- `occupied_count()` — counts non-zero occupancy bytes.
- `first_free_slot()` — lowest-index free slot as an `std::optional`;
  `nullopt` when the page is full. Lowest-index-first is what makes an
  erased slot the *next* one an insert reuses.
- `has_free_slot()` — a one-liner over `first_free_slot()`.
- `is_occupied(slot, error)` — the triple-state workhorse: `true` if
  occupied; `false` with an **empty** error if the slot is free; `false` with
  a filled error (`"slot N is out of range (capacity C)"`) if the index is
  bad. Callers distinguish "not here" from "invalid question" by the error
  string.

### Record operations

- **`get(slot, person, error)`** — requires the slot occupied (a free slot
  fails with `"slot N is free; no record to get"`), then deserializes the
  record bytes through the shared `PersonSerializer`. If deserialization
  fails (record bytes that cannot form a `Person`), that is reported too.
- **`put(slot, person, error)`** — bounds-checked; serializes the record into
  the slot and sets the occupancy byte to 1. It **overwrites either way** —
  an occupied slot is updated in place, which is what makes `HeapFile::update`
  a plain `put`. Note an asymmetry with the ex07 helpers: `put` does *not*
  reject a `Person` with `pid == 0`. In this format occupancy is the
  occupancy *byte*, not the pid sentinel, so a pid-0 record is storable here;
  the sentinel rule belongs to the coexisting ex07 block format, not to
  slotted pages.
- **`erase(slot, error)`** — sets the occupancy byte back to 0. The record
  bytes stay behind as garbage by design: the byte is the single source of
  truth, no zeroing and no compaction, and the slot is immediately reusable.

All three clear `error` on entry and fill it only on failure, matching the
module-wide convention.

### Invariant and test coverage

The class maintains one invariant: **for every occupied slot, the record
bytes deserialize to a `Person`; free slots may contain stale bytes.**
`main.cpp` checks the layout offsets byte-for-byte, the capacity/dead-space
formula, fresh-page state, a full put/get round-trip over all 185 slots,
erase-and-reuse, bounds rejections, that put/erase cycles never touch the
dead space, attach validation with a foreign record size, and — through the
pool — that chain links and records survive write-back and reload.

## Part B — HeapFile

### The problem the file solves

The assignment, distilled: a file of fixed-length `Person` records stored in
a chain of slotted pages, where each record is addressed by a **row id** of
(page id, slot number). The required operations:

a) **create** — a new file for the given record type;
b) **open** — an existing file;
c) **scan** — an iterator whose `next()` yields one `Person` at a time;
d) **insert** — a new `Person` into a page that has space;
e) **find** — the record with a given row id, error if not found;
f) **update** — the record at a row id with new `Person` information;
g) **erase** — the record at a row id;
h) **close** — the heap file;
i) **erase file** — delete the file itself.

The heap file uses the slotted pages to get at data through the buffer pool
only; its path is given to the API and maps 1:1 to one binary file on disk.
The first page of the file is a **header** that stores no data — just the
pointer to the next page, everything else unused. Once pages are allocated
they are never removed, and inserting means walking the chain until a slot
is found, or a new page needs to be created.

### Chain topology

The file is a chain of slotted pages on disk, and **page ids are block
numbers** — an id stored in a chain link is directly usable as a
`BufferManager::pin(file, id, ...)` argument.

```text
block 0            block 1          block 2         block 3
+-----------+ next +--------+ next +--------+ next +--------+
| HEADER    |----->| data   |----->| data   |----->| data   |--> kNoPage
| page      |<-----| page 1 |<-----| page 2 |<-----| page 3 |
+-----------+ prev +--------+ prev +--------+ prev +--------+
   ^ prev = kNoPage (always)
```

- **Block 0 is the header page.** It is formatted exactly like a
  `SlottedPage` — same 12-byte header convention, so no new serialization
  code — but its slots are never used. The only meaningful field is `next`
  (the first data page's id, `kNoPage` for an empty file); `prev` is
  `kNoPage` forever.
- Data pages are doubly linked among themselves and with the header: the
  first data page's `prev` is `0`, the tail's `next` is `kNoPage`.
- **Pages are allocated once and never removed or reordered.** A page id,
  once allocated, is valid forever; only slot occupancy inside a page
  changes. There is no free-page list, no compaction, no page deletion —
  non-goals of the exercise.

### RowId

```cpp
struct RowId { std::uint32_t page_id; std::uint32_t slot; };
```

A row id addresses one record. Two lifetime rules, documented in the header
and enforced through errors rather than hidden:

1. A row id is valid only while its record exists. Erasing frees the slot,
   and a later insert may hand out the *same* row id again — callers must not
   reuse saved row ids after erase.
2. `(0, anything)` is always invalid for find/update/erase: page 0 is the
   header and never stores records.

### Object state and invariants

```cpp
bufman::BufferManager& manager_;  // injected at construction
bufman::File file_;               // the one open file
bool open_ = false;
```

Nothing else — **no cached chain metadata**. The class does not remember the
tail page, the record count, or a "page with space" hint. Insert re-walks
the chain from the header on every call (O(pages) per insert); that walk
*is* the specification, and with one source of truth on disk there is
nothing to drift. The invariants maintained across operations:

1. Block 0 exists and is a valid header page (`prev = kNoPage`; `next` is
   `kNoPage` or an existing data page).
2. The chain is reachable head→tail via `next` and tail→head via `prev`;
   every page's slot count matches the `Person` record size (validated by
   `SlottedPage::attach` on every pin).
3. After `close` (or `create`'s internal close), the on-disk file alone
   represents the state; reopening yields the same records.

One structural simplification, documented in the header: `close()` flushes
via the manager's `close_all`, so a `HeapFile` expects to be the only user
of its manager — the academic single-file pattern used throughout the test
suite.

### `create(path, error)`

Refuses if *this* heap object is already open. Opens the path through the
manager (`open_for_append` creates a missing file, so the fresh-path case is
covered) and checks the block count: a path that already holds blocks is
refused (`"cannot create \"path\": the file already holds N block(s)"`); an
existing **empty** path is allowed and formatted, since it holds no data
yet. It then `alloc_page`s block 0, binds and `format`s it as the header
(`prev`/`next` = `kNoPage`: an empty chain), unpins dirty, and `close_all`s
— flushing the header so the process leaves exactly one block on disk.

### `open(path, error)`

Opening a foreign or missing file must fail *as such*, so `open` probes with
a read-only `open_for_read` first — `manager_.open_file` would silently
create a missing file (`O_CREAT`), which would turn "no such file" into a
confusing validation error. It then opens read-write (insert/update/erase
need writes) and validates:

1. the file has at least one block (`"the file has no header page"`), and
2. block 0 attaches as a slotted page for the `Person` record size — the
   `SlottedPage::attach` slot-count check doubles as the format check, so a
   file in the old serializer format is rejected with a readable message.

Any failure unwinds completely (`close_all`, reset handle): an unusable file
must not stay open. On success the heap is marked open.

### `insert(person, row_id, error)` — the heart of the class

Stated as the loop it is, with one pin alive at any moment:

1. Pin block 0 (header), read `next`, unpin. Set `current = header.next`,
   `previous = 0`.
2. While `current != kNoPage`: pin block `current`, `attach` a `SlottedPage`,
   ask for the first free slot.
   - **Found**: `put` the record, unpin **dirty**, return
     `RowId(current, slot)`.
   - **Not found**: remember `previous = current`, read `next`, unpin clean,
     continue with `current = next`.
3. Chain exhausted — allocate and splice a new tail:
   - The new page id is **`previous + 1`**, the old tail's id plus one (the
     header itself plays tail for an empty chain). It is deliberately *not*
     the on-disk block count: dirty pages reach disk only on eviction/flush,
     so the on-disk count lags behind the pool and would collide with a page
     that exists dirty in memory. Because pages are allocated strictly in
     increasing order and never removed, old-tail + 1 is provably the first
     unused id. A guard rejects the 32-bit overflow case
     (`"insert would need page id N, beyond the 32-bit row id range"`) —
     the documented cap of ~2³² pages.
   - `alloc_page(new_page_id)` (refusing existing blocks is a built-in
     safety net), `format(prev = previous, next = kNoPage)`, `put(0, person)`,
     unpin **dirty**.
   - Re-pin block `previous`, `set_next_page(new_page_id)`, unpin **dirty**.
     This re-pin-and-splice is the only two-step sequence in the class, and
     it runs strictly sequentially.
   - Return `RowId(new_page_id, 0)`.

Because the header is the chain's head, the very first insert splices page 1
after the header with no special case. The insert-order rule — first page
with a free slot in chain order wins — combined with `SlottedPage`'s
lowest-index `first_free_slot` means an erased slot is always the next to be
reused.

### `pin_record_page` — the shared validate path

`find`, `update`, and `erase` share one helper that turns a `RowId` into one
pinned, attached, occupied page — and on success the caller owns that pin:

1. Reject `page_id == 0` (`"page 0 is the header page; it never stores
   records"`).
2. Existence check: the page must be **resident in the pool or already
   written back**. The on-disk block count alone would reject live pages
   that exist only as dirty frames, so the check is
   `page_id >= blocks && !manager.resident(...)`.
3. Pin, bind, `attach` (revalidating the header every time).
4. Require the slot occupied; a free slot fails with a row-id-specific
   message (`"no record at row id (page P, slot S): the slot is free"`).

On any failure the pin is released inside the helper, so callers never leak
a pin down an error path. The result is four *distinct* not-found errors —
free slot, header page, slot beyond capacity, page beyond EOF — which the
check suite verifies are pairwise different, readable messages.

- **`find`** then `get`s the record and unpins clean.
- **`update`** then `put`s the new record (occupancy stays 1) and unpins
  dirty on success — a failed `put` (bad slot) unpins clean, since nothing
  changed.
- **`erase`** then clears the occupancy byte and unpins dirty on success.

### `close(error)` and the closed discipline

`close` flushes and closes via `close_all`, marks the heap closed, and
resets the handle. Every later operation fails with exactly
`"heap file is not open"` — insert, find, update, erase, scan, and even a
second `close` (which reports the same error rather than pretending to
succeed). The check suite exercises all six.

### `scan()` and `HeapFileIterator`

`scan` returns a small value holding three things: the owning `HeapFile`, the
current page id, and the next slot to inspect. No buffers, no cached pages —
each `next()` call pins exactly one page:

1. If the file closed meanwhile, fail with `"heap file is not open"`.
2. Pin the current page, `attach`, read its `next` id once.
3. Scan occupancy from the remembered slot forward; on an occupied slot,
   copy the record out, advance the remembered position (stay on this page
   at slot+1, or move to the next page at slot 0 when this was the last
   slot), unpin, and report the record.
4. If the page is exhausted from the remembered slot on, move to its `next`
   id and continue the loop.

The walk starts at `(0, 0)`: the header page never holds occupied slots, so
the first `next()` call naturally skips straight to the first data page —
no special case. When the chain ends (`kNoPage`), `next` reports exhaustion:
`false` with an **empty** error, and every further call keeps reporting
exhaustion the same way, so callers cannot confuse "done" with "failed"
(failures fill the error string, per the global convention). The one
documented precondition: do not insert/erase while an iterator is alive —
the iteration order under concurrent mutation is deliberately unspecified.

### `erase_file(manager, path, error)`

A free function, not a method — deleting a file is not an operation on an
open object. It refuses while the file is open *through the given manager*
(`manager.is_open(path)` — the registry every `HeapFile` of the
single-manager pattern reports to) with
`"cannot erase \"path\": the file is open"`, then deletes with the standard
`std::remove`, mapping `errno` into the error string on failure. Refusing
rather than auto-closing keeps "one object owns the open file" simple and
explicit.

### Persistence model

The heap never writes a page to disk directly. Every mutation marks a frame
dirty via `unpin(f, was_dirty=true)`, and the bytes reach disk when the
frame is evicted, `flush_all` runs, or `close`/`create` calls `close_all`.
The check suite proves this the hard way: 400 records inserted with a
**two-frame** pool — forcing evictions and write-backs throughout — then
close, reopen, and verify the scan count, spot-checked records, and the
0 ↔ 1 ↔ 2 ↔ 3 chain links in both directions, plus the same checks reading
the on-disk bytes directly through an independent handle.

## How the two cooperate through the pool

The canonical flow every heap operation follows:

```cpp
std::size_t f;
manager_.pin(file_, page_id, f, error);            // page id == block number
SlottedPage page;
page.bind(manager_.frame(f).buffer, bufman::kRecordSize);
page.attach(error);                                // validate the header
// ... page.put/get/erase/set_next_page ...
manager_.unpin(f, /*was_dirty=*/true, error);      // or false if read-only
```

New pages are the one variation: `alloc_page` returns a zeroed, dirty,
pinned frame, which `format` then turns into a valid slotted page. The
dependency points one way — `heapfile` is a view over `bufpool`'s buffers
and never the reverse — and the buffer manager remains the only code that
crosses the disk boundary.
