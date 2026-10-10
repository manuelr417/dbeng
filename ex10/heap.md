# HeapFile — Implementation Plan

## Prompt (original request)

> Let us now design the HeapFile. This class implement the idea of a file of records. The records are found in a SlottedPage. Each record has a row id consisting of (page id, and slot number). The HeapFile supports the following operations: a) create - creates a new file to store given record type. For now, it would be Person, b) open - opens the file for reading, c) scan - reads all records in the file by providing an iterator to read them one by one in a loop such that the call to next return a new record represented as Person, d) inserts a new record into a page that has space, with record coming as a Person structure, e) finds and return a record with a given row id, If not found, then returns en error code, f) updates a record based on it row id and new Person information, g) erase a record based on row id, h) closes the heap file, and i) erases a heap file. The heap file uses the slotted pages to get to the data from the buffer pool. Each heap file maps to a given binary file accessed by BlockFile, so the path needs to be given from the heap file api. The first page of the heap  file is the header. It does not store data, just the pointer to next. Everything else is not used. Once pages are allocated, they are not removed. Inserting means going over the chain untils a slot is found, or a new pages needs to be created. Generate plan for this implementation into file heap.md. Store this plan as part of the heap.md file. Read the page.md file to ensure consistency. This is an academic exercise. Favor clarity over performance optimization. Do not write any code yet.

## 1. Record type

The heap file stores `Person` records — the type defined in `part/Person.h` and
serialized by `part/PersonSerializer` at `kRecordSize = 21` bytes. The API
takes and returns `Person` values throughout.

## 2. Goals

- Add a `HeapFile` class in `heapfile/` (beside `SlottedPage`, per page.md §5)
  that turns a chain of slotted pages into a file of fixed-length records.
- Every record is addressable by a **row id** `(page id, slot number)`; page id
  and slot number are exactly the identifiers `SlottedPage` and `BufferManager`
  already understand.
- The heap file maps 1:1 to a binary file on disk; the path is given to the API
  and the file is accessed through `BufferManager` + `BlockFile` (pin/unpin,
  write-back — never raw I/O from `HeapFile`).
- Full operation set: create, open, scan (iterator), insert, find, update,
  erase record, close, erase file.

Non-goals (consistent with page.md §1): variable-length records, page
compaction, free-page lists / page deletion (pages are allocated once and never
removed), concurrency, crash recovery, and generic record types (v1 is typed on
`Person`; see open questions).

## 3. Design principles — clarity over performance

Inherits every principle from page.md §1a (one occupancy byte per slot, derived
values recomputed by linear scan, minimal state, named byte math, no clever
encodings, assertable behavior). Heap-file-specific consequences:

- **No cached chain metadata.** `HeapFile` does not remember the tail page, the
  record count, or a "page with space" hint. Insert re-walks the chain from the
  header every time (O(pages) per insert); that walk *is* the specification
  ("going over the chain until a slot is found"), and with one source of truth
  on disk there is nothing to drift.
- **One pin at a time.** Every operation touches exactly one pinned frame at a
  moment; pages are unpinned before moving on. The only two-step sequence is
  splicing a new tail (re-pin old tail, update its `next`, unpin), done
  strictly sequentially.
- **Insert-order search.** The first page with a free slot in chain order wins;
  erase makes that slot the next to be reused (lowest-index first-free-slot
  semantics from `SlottedPage`). No tombstones, no versioned row ids.
- **Errors are values.** The codebase convention — `bool` return plus out-param
  `std::string& error` — is the "error code" mechanism, including
  "record not found" for find/update/erase.

## 4. Chain topology

```
block 0            block 1          block 2         block 3
+-----------+ next +--------+ next +--------+ next +--------+
| HEADER    |----->| data   |----->| data   |----->| data   |--> kNoPage
| page      |<-----| page 1 |<-----| page 2 |<-----| page 3 |
+-----------+ prev +--------+ prev +--------+ prev +--------+
   ^ prev = kNoPage (always)      data pages hold records in slots
```

- **Block 0 is the header page.** It is formatted exactly like a `SlottedPage`
  (same 12-byte header convention, so no new serialization code), but its slots
  are never used. The only meaningful fields are: `next` = page id of the first
  data page (`kNoPage` when the file has no records/pages yet) and `prev` =
  `kNoPage` forever. All other bytes are unused, as specified.
- **The header is the permanent head of the chain.** Data pages are doubly
  linked among themselves and with the header: the first data page's `prev` is
  `0`, the tail's `next` is `kNoPage`.
- **Page ids are block numbers** (page.md §2): block 0 = header, data pages
  start at 1 and grow by one block each time a new page is needed.
- **Pages are never removed or reordered.** A page id, once allocated, is valid
  forever; only slot occupancy inside a page changes.
- A freshly created file is exactly one block (the header) with `next = kNoPage`.

## 5. Row id

- `RowId` is a pair `(page_id, slot)` of 4-byte unsigned integers, exposed in
  namespace `heapfile`. It is a pure value type returned by insert and consumed
  by find/update/erase.
- **Lifetime rule (documented, enforced by errors):** a row id is valid only
  while its record exists. Erasing a record frees its slot; a later insert may
  hand out the same `(page_id, slot)` again. Callers must not reuse saved row
  ids after erase.
- Row id `(0, anything)` is always invalid for find/update/erase — block 0 is
  the header and never stores records.

## 6. API design (operation by operation)

`HeapFile` is bound to a caller-owned `BufferManager&` at construction. All
operations return `bool` and report failures through an out-param
`std::string& error` (empty string on success), matching `BlockFile` and
`BufferManager` conventions. The operations, in prompt order:

| Op | Semantics | Key errors |
|----|-----------|------------|
| a) `create` | Given a path, refuse if a non-empty file already exists; open it through the manager, `alloc_page` block 0, format it as the header page (`prev`/`next` = `kNoPage`), unpin dirty, then close (write-back via `close_all`). Leaves a one-block file on disk. | path already holds a heap file; pool/manager errors |
| b) `open` | Given a path, open it through the manager (read-write: insert/update/erase need writes), then validate: file has at least one block and block 0's slot count matches the value `SlottedPage` computes for `Person`'s record size (guards against foreign/old-format files, per page.md §5 attach validation). | missing file; not a heap file (validation mismatch); already open |
| c) `scan` | Returns an iterator (section 8). Each `next` call yields the next occupied slot's record as a `Person`, walking the chain head→tail. | iterator used after close |
| d) `insert` | Takes a `Person`; walks the chain (section 7) and puts the record into the first free slot; returns the `RowId`. | manager errors; chain/page errors |
| e) `find` | Takes a `RowId`; pins that page, requires the slot occupied, deserializes and returns the `Person`. Not found (free slot, slot out of range, page 0, page beyond EOF) is reported through `error`. | "no record at row id …", "page 0 is the header", "page beyond end of file" |
| f) `update` | Takes a `RowId` + a `Person`; same pin path as find, then overwrites the slot's bytes (occupancy stays 1) and unpins dirty. | same as find |
| g) `erase` | Takes a `RowId`; same pin path, then clears the slot's occupancy byte (record bytes stay as garbage, page.md §5) and unpins dirty. | same as find |
| h) `close` | Flushes and closes via `close_all` (the manager's existing flush-all-then-close path), marks the heap file closed. After close, every operation fails with "heap file is not open". | — |
| i) `erase file` | A free function taking a path (not a method on an open file): deletes the binary file from disk. Requires the file to not be currently open by any `HeapFile`; implemented with the standard file-remove call. | file missing; deletion failed; file is open |

Open/close symmetry: one `HeapFile` object manages one open file at a time.
Simplification (documented): `close` uses `close_all`, so a `HeapFile` expects
to be the only file open in its `BufferManager` — the academic single-file
pattern used throughout `main.cpp`.

## 7. Insert algorithm (the heart of the class)

Stated as the loop it is, with one pin alive at any step:

1. Pin block 0 (header); read `next`. Unpin. Set `current = header.next`,
   `previous = 0`.
2. While `current != kNoPage`: pin block `current`; attach a `SlottedPage`
   (record size = `Person`'s); ask for the first free slot.
   - Found: put the serialized record into that slot; unpin **dirty**; return
     `RowId(current, slot)`.
   - Not found: remember `previous = current`; read `next`; unpin clean;
     `current = next`.
3. Chain exhausted — allocate a new tail:
   - New page id = the file's current block count (the first unused id);
     `alloc_page` it (it refuses existing blocks, a built-in safety net).
   - Format it as a `SlottedPage` with `prev = previous`, `next = kNoPage`;
     put the record into slot 0; unpin **dirty**.
   - Re-pin block `previous`; set its `next` = new page id; unpin **dirty**.
   - Return `RowId(new page id, 0)`.

Because the header is the chain's head, the very first insert splices page 1
after the header with no special case. Serialization/deserialization of the
record bytes reuses the existing `Person` serializer pair.

## 8. Scan / iterator design

A small `HeapFileIterator` value returned by `scan`:

- **State:** the owning `HeapFile`, current page id, current slot index. Nothing
  else — no buffers, no cached pages.
- **`next` contract:** each call pins the current page, scans occupancy from the
  remembered slot index forward; on an occupied slot it copies the record out,
  advances the remembered position, unpins, and reports a record. When a page is
  exhausted it moves to that page's `next` id and continues; when the chain ends
  (`kNoPage`) it reports exhaustion, and further calls keep reporting
  exhaustion. Pin/unpin per call means no long-lived pins and no interference
  with eviction.
- **Exhaustion vs. error:** `next` reports "no more records" and "something
  failed" through the same convention used everywhere: exhaustion leaves the
  `error` string empty, failures fill it.
- **Precondition (documented):** do not insert/erase while an iterator is
  alive — the iteration order under concurrent mutation is deliberately
  unspecified (clarity over completeness).

## 9. Object state and invariants

`HeapFile` members (all trivially inspectable): the injected `BufferManager&`,
the open `File` handle, and an open/closed flag. Nothing derived is cached
(principle §3).

Invariants maintained across operations:

1. Block 0 exists and is a valid header page (`prev = kNoPage`; `next` is
   `kNoPage` or the id of an existing data page).
2. The data-page chain is reachable head→tail via `next` and tail→head via
   `prev`; every page's slot count matches `Person`'s record size.
3. Every record byte in an occupied slot deserializes to a `Person`.
4. After `close` (or `create`'s internal close), the on-disk file alone
   represents the state; reopening yields the same records.

## 10. Consistency with page.md

| page.md decision | How HeapFile uses it |
|---|---|
| §2 page layout, 12-byte header, occupancy bytes | unchanged; header page reuses the same format |
| §4 `kNoPage = 0xFFFFFFFF` sentinel | chain ends, empty file's header `next` |
| §5 placement in `heapfile/`, one-way dependency | `heapfile/HeapFile.h/.cpp` sits beside `SlottedPage`; depends on `bufpool` views, never the reverse |
| §5 attach validation via slot count | `open` validates block 0 the same way |
| §5 error style (`bool` + `error` out-param) | all nine operations |
| §6 pin → bind → mutate → unpin-dirty flow | every operation; `alloc_page` for new pages |
| §1a clarity principles | no caching, linear walks, one pin at a time |
| §7 open decision "header page vs derived tail" | **resolved by this plan**: block 0 is the header page; the tail is found by walking (or by splicing at the end of the walk) |

## 11. Test plan (added to the `main.cpp` harness style)

1. **create**: new file has exactly one block; header `prev`/`next` = `kNoPage`;
   creating again over the same non-empty path fails with an error.
2. **open validation**: opening a missing path fails; opening a file seeded with
   the old serializer format (wrong slot count) fails; open → close → open
   round-trip works.
3. **first insert**: inserts into an empty heap file create page 1 and return
   `RowId(1, 0)`; header `next` becomes 1; page 1's `prev` is 0, `next` is
   `kNoPage`.
4. **fill and spill**: insert 185 records (page 1's capacity per page.md §3);
   the next insert lands in a new page 2 as `RowId(2, 0)`; the chain reads
   0 → 1 → 2.
5. **insert walks past partial pages**: fill page 1 halfway, erase one middle
   record, insert — the freed slot is reused (lowest free slot in the first
   page with space).
6. **scan**: empty file scans zero records; after N inserts, scan yields exactly
   N records in insertion order with matching field values; repeated `next`
   after exhaustion keeps reporting exhaustion without error.
7. **find**: each inserted row id finds its exact record; find on an erased
   slot, on `(0, 0)`, on a slot beyond capacity, and on a page beyond EOF each
   fail with a distinct, readable error message.
8. **update**: update changes only the target record (neighbors intact);
   find returns the new values; updating a free slot fails.
9. **erase**: erased row id stops being findable; scan count drops by one; the
   slot is reusable (covered by 5).
10. **persistence**: with a pool of 1–2 frames, insert several hundred records
    (forcing evictions and multiple new pages), close, reopen, and verify the
    scan count and spot-checked contents — the write-back-on-eviction path does
    the persistence work.
11. **chain integrity after splice**: after inserts that create pages 1..3,
    every page's `prev`/`next` pair matches its neighbors (read both directions
    through the pool).
12. **closed discipline**: after `close`, insert/find/scan/erase fail with
    "heap file is not open"; nothing crashes.
13. **erase file**: with the file closed, erase-file removes the path from
    disk; a second erase-file fails; erasing while open fails.

## 12. Implementation steps

1. Add `heapfile/HeapFile.h`: `RowId`, `HeapFileIterator`, `HeapFile`, and the
   free erase-file function; constants only where page.md has none yet.
2. Add `heapfile/HeapFile.cpp`: header-page formatting/validation (thin use of
   `SlottedPage`), the insert walk (section 7), the row-id operations (find /
   update / erase share one pin-and-validate helper), and the iterator.
3. Register both files in `CMakeLists.txt` for `ex10` and `ex10_demo`.
4. Implement against the pool directly (the unit layer here *is* the pool
   integration; `main.cpp` checks 1–13 above exercise both).
5. Leave `ex10_demo` CLI untouched in this step; wiring heap-file commands into
   it is a separate follow-up.

## 13. Open questions

- **create on an existing empty path** (zero blocks): proposal — allow it
  (format the header), since the file holds no data yet; a non-empty path is
  refused.
- **Read-only open**: `open` is read-write because insert/update/erase need it;
  a read-only variant using `open_file_read` is deferred until a use exists.
- **Typed vs. generic records**: v1 is typed on `Person` (clarity). Generalizing
  to arbitrary record types means parameterizing the record size and the
  (de)serialization calls; deferred with no design committed yet.
- **erase-file while open**: proposal — refuse with an error rather than
  auto-closing, keeping "one object owns the open file" simple and explicit.
- **RowId as 32-bit page id**: block counts are 64-bit in `BlockFile`; the plan
  caps heap files at 2^32 pages (~16 TiB of 4 KiB pages) and documents the
  guard (insert fails if the new page id would overflow). Fixing this means
  widening the on-disk header format — deliberately out of scope.
