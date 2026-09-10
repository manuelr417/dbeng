# serialized.md — Person access through buffer-pool frames (ex09)

Status: implemented. The four slot-level helpers are in PersonSerializer and
main.cpp runs the full check suite plus the pool scan demo (61 checks, all
passing).

## Goal

`main` can read and write `Person` records that live in buffer-pool frames:
pin a block with `BufferManager`, use the existing serializer directly on the
frame's 4 KB buffer, unpin with the dirty flag when records were modified.

## Core design decision

The serializer already speaks the right language: its functions take a
`char*` / `const char*` block buffer, and `DataFrame::buffer` **is** a
`char[4096]` in the exact on-disk layout (21-byte records, 195 per block,
`pid == 0` marks a free slot). So:

- **No signature changes** to `serialize` / `deserialize` — a frame buffer
  plugs straight in: `bufman::deserialize(mgr.frame(f).buffer + slot * kRecordSize, ...)`.
- **No changes** to `BufferManager`, `BufferPool`, `DataFrame`, or `BlockFile`.
- The dirty-flag coupling is already solved: `unpin(frame, was_dirty)` marks
  the frame for write-back. The write flow is
  pin → `put_record(frame.buffer, ...)` → `unpin(frame, was_dirty=true)`.
- Serialization format is untouched, so blocks written through the pool stay
  byte-compatible with ex07 tooling (`append`, `scan`) and vice versa.

## PersonSerializer additions (thin, block-level helpers)

Free functions in `namespace bufman`, taking a raw block pointer exactly like
the existing API — no `DataFrame` dependency, still unit-testable standalone.
All bounds-checked against `kRecordsPerBlock`:

- `std::size_t record_count(const char* block)` — occupied slots; scans for
  the first `pid == 0` slot (same rule as `deserialize_block`, without
  building the vector).
- `std::optional<std::size_t> first_free_slot(const char* block)` — first
  `pid == 0` slot; `nullopt` when the block is full (195 records).
- `bool get_record(const char* block, std::size_t slot, Person& out)` —
  false if `slot` is out of range or the slot is empty (`pid == 0`).
- `bool put_record(char* block, std::size_t slot, const Person& person)` —
  false if `slot` is out of range; overwrites the slot's 21 bytes.

These are one-liners over `serialize` / `deserialize`; the slot arithmetic
(`slot * kRecordSize`) lives in one place instead of being repeated in main.

## main.cpp integration

Keep the existing check() suite and append a "person access" section plus a
short demo, using the established helpers (`seed_file`, `read_disk_byte`):

1. **Seed**: create a file where block 0 holds two persons (via
   `serialize_block` into a zeroed buffer + `write_block`), so the on-disk
   format matches what the pool will read.
2. **Read through the pool**: `pin(file, 0)` → `record_count(frame.buffer)`
   == 2 → `get_record(slot 0/1)` → verify pid/name/age/city fields.
3. **Update in place**: `put_record(frame.buffer, 0, modified_person)` →
   `unpin(frame, was_dirty=true)` → re-pin (after eviction pressure on a
   small pool) → modified fields come back; verify on-disk bytes through a
   separate BlockFile handle.
4. **Append into a free slot**: `first_free_slot` → `put_record` →
   `unpin(dirty)` → verify on disk; a second append lands in the next free
   slot; `record_count` reflects growth.
5. **Full block**: fill all 195 slots → `first_free_slot` returns `nullopt`;
   the next append needs the following block (documented behavior, asserted
   as an error path in the demo).
6. **Scan through the pool**: iterate all blocks of the file, pin → print
   records via `get_record` → unpin; a second pass hits the same frames
   (page-table hits, no reloads).
7. **Bounds/sentinel guards**: `get_record`/`put_record` with
   `slot >= kRecordsPerBlock` return false; a `pid == 0` person cannot be
   stored via `put_record` (rejected, it is the free-slot sentinel).

### Note on creating new blocks

The pool only reads existing blocks (a `pin` past EOF fails by design). To
grow a file with new person blocks, main seeds the block first (zeroed block
via `BlockFile::write_block`, or `append_records`), then pins and fills it
through the pool. This keeps "blocks are always read with BlockFile and put
into the pool; dirty frames are always written back" as the only data path.

## What deliberately does not change

- `serialize` / `deserialize` signatures and the 21-byte record layout.
- `BufferManager` API (dirty marking rides on `unpin(frame, was_dirty)`).
- Block format compatibility with ex07.
- The existing check suite in main (new checks are appended, not replaced).

## Decisions made (adjust if wrong)

- Helpers live in `PersonSerializer` (block-buffer level) rather than a new
  `PersonPage` module — fewest moving parts, same namespace, no new files
  beyond the plan doc.
- `put_record` validates but does not mark anything dirty — dirtiness is a
  pool concern, handled by `unpin(frame, true)` in main.
- Person `pid == 0` remains invalid data (free-slot sentinel), same as ex07.
- main gains a small demo printout (scan through the pool) in addition to
  PASS/FAIL checks, matching the course style of showing the mechanism work.

## Testing plan

All new behavior lands as `check()` cases appended to main.cpp:

1. Seeded block reads back identical person fields through pin + `get_record`.
2. `record_count` matches seeded/append counts at every step.
3. Update + dirty unpin persists (frame reload and separate-handle disk check).
4. Append into free slots fills 0, 1, 2… sequentially and persists.
5. Full block: 195 puts fill it; `first_free_slot` = `nullopt` afterwards.
6. Slot-index guards reject out-of-range and `pid == 0` misuse.
7. Second scan pass over the same blocks returns the same frame indices
   (pool hits, no eviction churn on a large-enough pool).
