# Slotted Page for a Heap File — Implementation Plan

## Prompt (original request)

> Let us now add a class the represents a slotted page for records for a heap file. The file consists of a doubly linked list of slotted pages in disk. Each page has an id, which is the offset (number of pages) to skip from the beginning to reach the page. The class takes care of managing the layout of records. The buffer in a data frame gets picked up by the slotted page. The page imposes the following logic: the last 4 bytes represents an integer for the offset (page id) of the next page, the previous 4 bytes to it represent an integer for the offset (page id) of the previous page, the previous 4 bytes to it represent the number of records (slots) that can be stored. Prior to it is an slotted array of bytes, each byte can be 0 o 1. 0 means that there is not record in the slot. 1 means it is occupied by a record. There is one slot element in this array per slot to keep records. Thus the number of records depends on the record length plus one extra byte to keep track of the use of the slot. between the slots used to store records and the slotted array there is an array of unused bytes (dead space) that is too small to fit a record. This dead space. Generate a plan for this implementation into file page.md. Do not write any code yet. Put this prompt as part of the contents of page.md

## 1. Goals

- Add a `SlottedPage` class that interprets a `DataFrame::buffer` (4096 bytes) as a
  fixed-length-record slotted page with an explicit per-slot occupancy byte.
- Each page carries its position in a doubly linked list of pages: `prev` and `next`
  page ids stored in the page itself, so a heap file is a chain of pages on disk.
- Page id == block number in `BlockFile` == number of 4096-byte pages to skip from
  the start of the file, so the ids stored in the chain are directly usable as
  `BufferManager::pin(file, id, ...)` arguments.
- The class manages layout only (bytes in one frame). It performs no I/O; pin/unpin,
  dirty marking, eviction, and write-back stay in `BufferManager`, exactly as today.

Non-goals (this iteration): variable-length records, page compaction, a free-page
list for reusing deleted pages, and concurrency.

## 1a. Design principles — clarity over performance

Every choice below deliberately favors the most readable implementation, even
when it costs cycles or bytes:

- **Occupancy is one byte per slot**, exactly as specified — never bit-packed.
  The waste (185 bytes out of 4096) buys an array you can read at a glance.
- **Derived values are recomputed, not cached.** Occupied count, free count, and
  first free slot are linear scans over the occupancy array on every call
  (O(C)); no running counters are maintained anywhere. With C = 185 the scans
  are trivially fast anyway, and having the bytes be the *only* state removes a
  whole class of "counter drifted from reality" bugs.
- **Minimal object state.** The class holds only the bound buffer pointer, the
  record size, and the capacity read (and validated) at attach. Everything else
  lives in the page bytes — one source of truth.
- **Byte math is written once, with names.** Small helpers such as
  `record_offset(slot)`, `occupancy_offset(slot)`, and fixed header offsets
  replace scattered arithmetic, so the layout table in section 2 has exactly one
  counterpart in code.
- **No clever encodings.** Plain `memcpy` of native-endian 4-byte integers; no
  endianness handling (single-host format, documented). No free-count in the
  header, no bump-pointer dead space, no doubly-linked-list shortcuts — those
  are performance/complexity trades this exercise does not need.
- **Favor assertable behavior.** Each rule above is directly testable (test plan
  items 1–8), which matters more than speed in a teaching codebase.

## 2. Page byte layout (kPageSize = 4096)

Fixed-size records grow forward from the start of the page; the header grows
backward from the end. With `R` = record size in bytes and `C` = slot capacity:

| Region          | Offset range (bytes)                    | Contents                                              |
|-----------------|-----------------------------------------|-------------------------------------------------------|
| Records         | `[0, R*C)`                              | Slot `i` occupies `[i*R, (i+1)*R)`                     |
| Dead space      | `[R*C, R*C + D)`                        | `D = (kPageSize - 12) - C*(R + 1)` unused bytes        |
| Occupancy array | `[kPageSize - 12 - C, kPageSize - 12)`  | One byte per slot; `0` = free, `1` = occupied          |
| Slot count      | `[kPageSize - 12, kPageSize - 8)`       | 4-byte integer: `C` (slots the page can store)         |
| Prev page id    | `[kPageSize - 8, kPageSize - 4)`        | 4-byte integer: page id of previous page in the chain  |
| Next page id    | `[kPageSize - 4, kPageSize)`            | 4-byte integer: page id of next page in the chain      |

```
byte offset:  0 ............ R*C ...... R*C+D ........ kPageSize-12-C .. kPageSize-12 ...... kPageSize-8 ...... kPageSize-4 ...... kPageSize
              |  slot 0 | ... | slot C-1 | dead | occ[0] .. occ[C-1] |  slot count  |  prev page id  |  next page id  |
```

## 3. Capacity and dead space math

- Header is the last 12 bytes (three 4-byte integers).
- Per-slot cost is `R + 1` (the record plus its occupancy byte).
- `C = floor((kPageSize - 12) / (R + 1))`; the remainder is the dead space
  `D = (kPageSize - 12) - C * (R + 1)`, which by construction is
  `0 <= D <= R` — always too small to hold one more record-plus-slot-byte.
- Worked example with the existing `Person` (`R = 21`):
  - `C = floor(4084 / 22) = 185` slots
  - records: 185 * 21 = 3885 bytes, offsets `[0, 3885)`
  - dead space: 14 bytes, offsets `[3885, 3899)`
  - occupancy array: 185 bytes, offsets `[3899, 4084)`
  - header: `[4084, 4096)` — slot count, prev id, next id
  - sanity: 3885 + 14 + 185 + 12 = 4096

Note this layout is independent of the current `PersonSerializer` block format
(195 records, `pid == 0` sentinel, no occupancy bytes). The two formats coexist;
a given file uses one or the other, never both.

## 4. Header conventions

- Slot count, prev id, next id are 4-byte integers copied with `memcpy`
  (native endianness, matching the existing serializer's approach).
- `kNoPage`: sentinel for "no page in this direction" (empty chain end).
  Proposal: `0xFFFFFFFF` (invalid as a page id, since valid ids are offsets of
  pages that exist). Prev of the first page and next of the last page hold it.
- A page id of `0` is legal (first page of the file), so `0` must never be used
  as the "no page" sentinel.

## 5. Class design

Placement: `heapfile/SlottedPage.h` and `heapfile/SlottedPage.cpp` — a new
`heapfile/` subdirectory that will also hold the future `HeapFile` helper, so
all heap-file concepts live together. The dependency points one way: `heapfile`
is a *view* over `bufpool`'s `DataFrame::buffer` and needs no dependency on
`BlockFile` (its consumers already hold a `bufman::File`); `bufpool` itself
never includes `heapfile`.

Shape: a non-owning view bound to a `char*` (the frame's buffer) plus a fixed
record size chosen at bind time. Its only members are the buffer pointer, the
record size, and the capacity read at attach — no other state is cached
(design principles above). `const` methods for inspection, non-`const`
for mutation; the caller marks the frame dirty (existing `unpin(f, true, ...)`
style). Constants exposed: `kPageHeaderSize = 12`, `kNoPage`.

Responsibilities:

1. **Format / attach**
   - *Format* a fresh page: write slot count `C` (derived from `R`), occupancy
     bytes all `0`, prev/next ids as given (default `kNoPage`).
   - *Attach* to existing bytes: read `C` from the header; optionally verify it
     matches the value recomputed from `R` (guards against a record-size mismatch
     or a buffer in the old serializer format).
2. **Chain navigation** — read prev/next ids; update either direction (the
   future heap-file layer uses these to splice pages in and out).
3. **Slot inspection** — capacity `C` (from header, read at attach), number of
   occupied slots (linear scan of the occupancy array), whether any free slot
   exists, occupancy test for a given slot, first free slot (lowest index,
   linear scan) as an optional value.
4. **Record operations** — all bounds-checked against `C`:
   - `get` record from slot: requires the slot occupied; copies `R` bytes out.
   - `put` record into slot: copies `R` bytes in, marks the occupancy byte `1`.
   - `erase` slot: marks the occupancy byte `0`; the record bytes stay behind
     as garbage inside the slot (documented; no zeroing, no compaction — the
     slot is immediately reusable).
5. **Invariant** the class maintains: occupancy byte and record bytes are always
   consistent for occupied slots; free slots may contain stale bytes.

Error style: follow the codebase's two conventions — out-param `std::string&
error` returning `bool` where a failure is expected (bad slot, capacity
mismatch), and no exceptions from this class itself (the pool already throws
for out-of-range frames).

## 6. Integration with the buffer pool

Typical flow for the future heap-file layer (and for tests):

1. `mgr.pin(file, page_id, f, error)` — page id doubles as block number.
2. Bind a `SlottedPage` to `mgr.frame(f).buffer` with the record size.
3. Read/mutate slots or prev/next ids.
4. `mgr.unpin(f, /*was_dirty=*/true, error)` if anything changed.

Because write-back happens on eviction/flush/`close_all`, chain edits and record
edits persist with no extra code. `alloc_page` is the natural way to create a
new tail page: it returns a zeroed, dirty, pinned frame that is then formatted.

## 7. Heap file layer (consumer, sketched for context)

The doubly linked list itself is *not* part of `SlottedPage`; a thin `HeapFile`
helper (future step, placed alongside it in `heapfile/HeapFile.h/.cpp`) would own:

- Locating the head: simplest proposal — the chain starts at block 0 if the
  file is non-empty; the tail is reached via `next` links (or tracked as
  `block_count - 1` while pages are only appended).
- `append(record)`: pin the tail, take its first free slot; if full, `alloc_page`
  a new tail, splice it via prev/next ids, then insert.
- `scan()`: walk `next` from the head, yielding occupied slots only.

Open decision (deferred): whether to reserve block 0 as a file header page that
stores head/tail ids explicitly, vs. deriving head/tail from file size. The plan
proposes deriving for now and revisiting when deletions of whole pages arrive.

## 8. Test plan (added to the `main.cpp` harness style)

1. **Layout offsets**: format a page for `R = 21`; verify the slot-count bytes
   sit exactly at `[kPageSize-12, kPageSize-8)`, prev at `[kPageSize-8, ...)`,
   next at the last 4 bytes, and occupancy bytes end exactly at `kPageSize-12`.
2. **Capacity math**: `C = 185`, dead space = 14 for `R = 21`; spot-check other
   record sizes (e.g. `R = 1`, `R = 25`) for the `floor` and remainder behavior.
3. **Fresh page**: after format, all occupancy bytes are `0`, occupied count is
   0, first free slot is 0, prev/next default to `kNoPage`.
4. **Round-trip**: put then get on every slot index preserves bytes; occupancy
   flips to 1; occupied count reaches `C` when full.
5. **Erase and reuse**: erase a middle slot; `get` on it now reports free;
   first free slot returns it; re-put works.
6. **Bounds**: slot index `C` and beyond rejected by get/put/erase/is-occupied.
7. **Dead space untouched**: seed a marker pattern in the dead-space region
   before formatting, verify it survives put/erase cycles (format writes only
   its own regions; note format of a *fresh* page may legitimately overwrite it,
   so this check applies to attach-then-mutate).
8. **Attach validation**: attaching with a wrong record size (recomputed
   capacity differs from header) fails with an error message.
9. **Chain navigation**: format pages 0 and 1, set links both ways, read back;
   walk head→tail and tail→head through the ids.
10. **Persistence through the pool**: with a pool of 1–2 frames, fill a page,
    force eviction, re-pin, and verify records and prev/next ids came back from
    disk (dirty write-back path already covered by the manager tests).
11. **End-of-chain sentinels**: prev of page 0 and next of the last page read
    back as `kNoPage` after a reload.

## 9. Implementation steps

1. Add `heapfile/SlottedPage.h` (class, constants `kPageHeaderSize`, `kNoPage`,
   capacity formula) and `heapfile/SlottedPage.cpp` (format/attach, header
   memcpy helpers, slot operations).
2. Register both files in `CMakeLists.txt` for `ex10` and `ex10_demo`.
3. Implement the class against a raw buffer first; unit checks 1–8 above run
   without any file or pool.
4. Add pool-integration checks 9–11 to `main.cpp` using `BufferManager` +
   `LRUPolicy` exactly like the existing sections.
5. Defer the `HeapFile` helper to a follow-up; its design questions are listed
   in section 7.

## 10. Open questions

- Sentinel `kNoPage = 0xFFFFFFFF` — confirm acceptable (proposal: yes).
- Should `erase` zero the record bytes (privacy/debuggability) or leave them?
  Proposal: leave them; occupancy byte is the single source of truth.
- Capacity check on attach: hard error or silent trust of the header value?
  Proposal: hard error, since a mismatch means corrupted or foreign bytes.
- Where the heap file stores its head pointer once whole-page deletion lands
  (file header page vs. metadata sidecar) — deferred with the `HeapFile` work.
