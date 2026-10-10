# HeapFile over Tuples (fixed-width records) — Implementation Plan

## Prompt (original request)

> plan how to modify heap file such that is uses Tuple instead of Person
>
> Assume that records in a tuple will be fixed size even if using string
>
> adjust the plan to account for removing Person.h, PersonSerializer.cpp,
> and PersonSerializer.h from the build — put them in a folder named
> deadcode.

The second directive decides the storage format: every column occupies a
fixed width — for a String, the width the catalog declares (`size` in
`person.json`) — so the record size is a schema constant, not a per-value
quantity. The third removes the hardwired path from the build entirely:
the legacy files retire to `deadcode/`, and everything that still leaned
on them (`file/BlockFile`'s record-append helpers, `BufferManager`'s
block zeroing, `main.cpp`'s legacy record-width check) is adjusted.

## 1. Goals

- `HeapFile` stores `tuple::Tuple` records instead of hardwired
  fixed-width records: `insert(Tuple)`, `find(RowId, Tuple)`,
  `update(RowId, Tuple)`, `scan()` yielding Tuples — same
  `RowId{page_id, slot}` addressing, same page chain, same
  one-pin-at-a-time discipline, same `bool + std::string& error` shape.
- **Records are fixed-width, sized by the schema.** A record is the
  concatenation of its columns at fixed offsets: 4 bytes per Integer, 8
  per Double, `column_size` bytes per String (NUL-padded). For
  `person.json` that is 4+10+4+3 = 21 bytes, derived from the catalog.
- **The hardwired legacy path leaves the build.** `tablesample/Person.h`,
  `PersonSerializer.h`, and `PersonSerializer.cpp` move to a new
  `deadcode/` folder — kept for reference, never compiled, never included.
  The places outside `heapfile/` that still used them (`file/BlockFile`'s
  record-append helpers and `main.cpp`'s legacy record-width check) are
  adjusted in the same step.
- `SlottedPage` keeps its fixed-grid layout (slot i at `i*record_size`,
  occupancy bytes below the header, the ex10 design) and only stops
  knowing the record type: it moves `record_size` raw bytes in and out.
- A new small codec in `schema/` packs a validated tuple into the
  fixed-width record and unpacks it back — the general, schema-driven
  replacement for the retired hardwired, field-by-field serializer.
- The file is **schema-backed**: `create`/`open` take a
  `schema::TableSchema`; the header page carries a magic number and the
  table name so `open` verifies the file matches the catalog it is handed.
- `validate` grows the one check this format makes essential: a String
  value longer than its declared width is refused ("the schema says how
  wide" — the third clause from `catalog.md`, finally enforced).

Non-goals (this iteration): persisting the schema itself in the file (the
JSON catalog stays its home; only the table name travels in block 0),
variable-length or directory-based page layouts (the fixed grid is the
point of this exercise), column permutations in the record layout
(`column_order` governs the tagged wire format only; records follow tuple
position order), compaction, concurrency, and performance tuning.

## 2. Design principles — clarity over performance

- **Two encodings, two jobs.** The tagged `[type][length][payload]` wire
  format of `tuple.md` stays what it was built for: self-describing,
  generic transport (buffer-pool tests, schema-guard demos). Heap file
  records are the *storage* encoding: fixed offsets, no tags, the schema
  is the tag. Neither format knows about the other; both are driven by the
  same `TableSchema`.
- **The schema is the record format.** Offset and width of every column
  are derived from `column_size` + type — one writer (`record_size()` and
  the pack/unpack pair), never recomputed by hand anywhere else.
- **`SlottedPage` stays a layout manager.** Its one change is
  type-erasure: the typed record parameters become raw byte buffers of
  exactly `record_size_`. Nothing about grids, occupancy, or the page
  header moves — the format on disk is unchanged from ex10.
- **The schema is out-of-band, the name is in-band.** The heap file does
  not store the catalog; it stores which catalog it belongs to (magic +
  table name in block 0). The caller loads the schema from JSON and owns
  it for the file's lifetime.
- **Validation at the boundary, again.** `insert`/`update` validate the
  tuple against the schema (count, types, and now string widths) before
  any page is pinned; reads unpack through the schema, so a record whose
  fixed fields disagree with the catalog (e.g. padding garbage) is
  refused with a named error.
- **Errors keep the house shape** — named, cleared on entry, and the
  `release()` pin-discipline helper carries over verbatim.

## 3. The fixed-width record format

Record layout, in tuple position order (column_position 0, 1, 2, …):

| Column (position p) | Width                       | Contents                                          |
|---------------------|-----------------------------|---------------------------------------------------|
| Integer             | 4                           | `memcpy` of the int, native endianness            |
| Double              | 8                           | `memcpy` of the double                            |
| String              | `column_size` (from catalog)| value bytes, then NUL padding to the full width   |

- `offset(p) = Σ widths of positions < p`;
  `record_size(schema) = Σ widths` — for `person.json`:
  pid@0 (4), name@4 (10), age@14 (4), city@18 (3) = **21 bytes**. Test 1
  proves this layout table directly against a hand-built expected
  buffer; nothing in the build references the retired code.
- Strings: the value must not contain NUL (the pad byte is NUL, so
  strings behave like fixed char arrays) and must fit its width;
  both are enforced by `validate`/`pack` with named errors.
- Page capacity math is today's, with the schema's record size:
  `slot_capacity = (4096 - 12) / (record_size + 1)` — 185 records of the
  person catalog per page, dead space 14.

## 4. The record codec (`schema/RecordCodec.h` + `.cpp`)

Free functions over any `TableSchema`, `namespace schema` — the pair that
replaces the retired hardwired, field-by-field layout:

```cpp
/// Total fixed record width: 4 per Integer, 8 per Double, column_size
/// per String.
std::size_t record_size(const TableSchema& schema);

/// validate, then pack every column at its fixed offset. Fails before
/// touching the buffer when the tuple does not match the schema
/// (arity, types, or a string wider than its declared column).
bool pack(const TableSchema& schema, const tuple::Tuple& tuple,
          char* buffer, std::size_t max_len, std::string& error);

/// Unpack a record into a fresh tuple in column_position order. Verifies
/// that String padding is all NUL ("column 'name': padding contains
/// garbage") — the fixed-format integrity check that replaces the wire
/// format's type tags. The out tuple is cleared first and stays empty on
/// any failure.
bool unpack(const TableSchema& schema, const char* buffer,
            std::size_t max_len, tuple::Tuple& tuple, std::string& error);
```

- Layout arithmetic lives here and nowhere else; `static_assert(sizeof(int)
  == 4 && sizeof(double) == 8)` as in `TupleSerializer.cpp`.
- Numeric columns use their natural C widths (4/8); a catalog's numeric
  `size` is documentation, not layout (open question 2).
- String columns with no declared width (`column_size == 0`, the default
  for code-built schemas) cannot live in a record — `record_size`, `pack`,
  and `unpack` refuse them with a named error ("column 'name' has no
  declared width; record packing needs catalog-loaded schemas"). The heap
  file is therefore catalog-fed by construction.

## 5. `SlottedPage` after the change

The ex10 fixed grid survives untouched. Only the record type is erased:

```cpp
void bind(char* buffer, std::size_t record_size);   // unchanged
void format(...);  bool attach(...);                // unchanged
// chain navigation, capacity, occupancy: all unchanged

/// Copies record_size_ bytes out of an occupied slot.
bool get_record(std::size_t slot, char* out, std::size_t max_len,
                std::string& error) const;
/// Copies record_size_ bytes in and marks the slot occupied.
bool put_record(std::size_t slot, const char* data, std::size_t len,
                std::string& error);
bool erase(std::size_t slot, std::string& error);   // unchanged
```

- The typed `get`/`put` are replaced by the two byte-level calls; the
  includes of the retired serializer leave the file. The `len`/`max_len`
  guards check the bound record size — belt and suspenders for a
  fixed-grid page.

## 6. The header page (block 0)

Block 0 keeps its unchanged SlottedPage formatting — and since the heap
file never stores records there (every row-id operation refuses page 0),
its record grid is free real estate. It needs it: a file whose record
layout is chosen at `open` time — any catalog can be handed in — must
identify itself on disk, or `open` cannot tell a tuple heap file from a
random 4096-byte file, or a file for one catalog from a file for another.
So `create` writes a small identification record at offset 0 of block 0:

| Offset | Size | Contents                            |
|--------|------|-------------------------------------|
| 0      | 4    | magic `0x31505554` ("TUP1")         |
| 4      | 4    | table name length N                 |
| 8      | N    | `schema.table_name()`, UTF-8, N ≤ 200 |

- The magic is the "this is ours" check: an ex10-era file or a text file
  fails `open` with "not a tuple heap file" instead of a puzzling
  slot-count mismatch from `attach`. The table name is the "this is that
  catalog" check: opening a file with a schema whose `table_name()`
  differs is refused by name.
- Today's `open` has no such check — it validates block 0 only by
  attaching with the hardwired record size, which any same-sized file
  passes. Once the record size comes from a catalog, that check alone
  proves nothing; the identification record restores it.
- `create(path, schema, error)`: format block 0, write the
  identification record; refuses a non-empty target file — as today.
- `open(path, schema, error)`: pins block 0, checks the magic, compares
  the name, and attaches the page with `record_size(schema)` — which
  also rejects foreign fixed-grid files whose slot count does not match.

## 7. `HeapFile` after the change

Same lifecycle, same private machinery (`require_open`,
`pin_record_page`, chain walk, splice, `release`); five changes:

```cpp
bool create(const std::string& path, const schema::TableSchema& schema,
            std::string& error);
bool open(const std::string& path, const schema::TableSchema& schema,
          std::string& error);
bool insert(const tuple::Tuple& tuple, RowId& row_id, std::string& error);
bool find(RowId row_id, tuple::Tuple& tuple, std::string& error);
bool update(RowId row_id, const tuple::Tuple& tuple, std::string& error);
// scan(): HeapFileIterator::next(tuple::Tuple&, std::string&)
```

- New private state: `const schema::TableSchema* schema_ = nullptr;` and
  `std::size_t record_size_ = 0;` (from `record_size(schema)`, refused at
  create/open when 0 or too big for one slot).
- `insert`: `require_open` → `schema_->validate` (count, types, widths —
  fail before pinning anything) → `pack` into a local
  `std::array<char, kPageSize>` → pin header, walk the chain for the
  first free slot → `put_record` → unpin dirty; splice a new tail page
  exactly as today when the chain is full.
- `find` / iterator: pin → `get_record` into a local buffer →
  `schema::unpack` → unpin clean.
- `scan` / `HeapFileIterator`: the walk itself is untouched — start at
  block 0 (whose slots are never occupied, so the first `next` skips
  straight to the first data page), walk the chain head → tail, one
  pinned page at a time, slots lowest-first, and exhaustion reported as
  `false` with an empty `error` on every later call. Two things change:
  `next`'s out parameter becomes `tuple::Tuple&`, and each occupied slot
  is decoded through `get_record` + `schema::unpack` — so a record that
  violates the schema fails the scan with a named error instead of
  yielding a wrong tuple. The existing caveat carries over: do not
  insert/erase while an iterator is alive.
- `update`: `pack` → `put_record` — always the same size, so update is a
  plain overwrite again (occupancy stays 1), exactly like ex10. The
  "update refuses growth" problem of the variable-length design does not
  exist here.
- `erase`, `close`, `erase_file`, `RowId`: unchanged.

## 8. Validation rules (all with named errors)

- `validate` (extended in `TableSchema.cpp`): beside count and per-slot
  type, a String column **with a declared width** (`column_size > 0`)
  whose value is longer → "column 'name': value of 12 bytes exceeds the
  declared width of 10"; a String containing NUL → "column 'name':
  strings must not contain NUL". Undeclared columns (width 0) stay
  unbounded — the check is conditional, so every code-built schema and
  all existing schema tests keep passing unchanged.
- `pack`: re-runs `validate` (defense in depth), refuses undeclared-width
  String columns (section 4), then `max_len < record_size` → size error.
  Fails before touching the buffer.
- `unpack`: clears the out tuple; refuses undeclared-width String columns;
  verifies every String slot's padding is all NUL; `max_len < record_size`
  fails.
- `create`/`open`: schema with `record_size == 0` or `record_size >
  kPageSize - kPageHeaderSize - 1` (no room for a slot with its occupancy
  byte) → refused by name.
- Everything else inherits the existing page/chain error messages
  unchanged; every `bind` call now passes the schema's `record_size`.

Documented limitations of the fixed-width format (stated here so nobody
rediscovers them the hard way):

- **No per-field integrity for numerics.** Without tags there is nothing
  to check: a corrupted int/double field unpacks as silent garbage. Only
  the String padding check catches page damage. (The tagged wire format
  remains the self-describing encoding for exactly this reason.)
- **`pid == 0` is legal now.** The retired hardwired serializer used
  `pid == 0` as its free-slot sentinel; the heap file's occupancy byte
  replaces that mechanism, so a tuple with pid 0 stores and reads back
  like any other.

## 9. Alternatives considered

- **Slot directory for variable-length records** (the previous draft of
  this plan): the textbook layout, ~100 extra lines, honest space reuse —
  but the fixed-size directive removes its reason to exist. Kept here as
  the recorded alternative; the fixed grid is simpler and now exact.
- **Pad the tagged wire format into fixed slots**: keeps one encoding,
  but stores useless tags and padding per record. Rejected.
- **Schema-free tuple file**: no arity/type integrity, weaker `open`
  checks. Rejected — the schema-backed file is the point of the arc.

## 10. Placement, namespace, and integration

- Modified in place: `heapfile/SlottedPage.h/.cpp`,
  `heapfile/HeapFile.h/.cpp`. New: `schema/RecordCodec.h/.cpp`
  (`namespace schema`). ex10 keeps its own untouched copy of the retired
  design.
- **`deadcode/` (new folder, outside the build):**
  `tablesample/Person.h`, `tablesample/PersonSerializer.h`, and
  `tablesample/PersonSerializer.cpp` move there unchanged — kept for
  reference, never compiled (dropped from CMake `SOURCES`), never
  included. `tablesample/` keeps the JSON catalogs (`person.json`,
  `car.json`) and remains the loader's input directory.
- **`file/BlockFile` is trimmed to the generic block layer.** Its
  fixed-record append facility — `append_records`, `record_count`, and
  the private `locate_append_position` machinery, all hardwired to the
  legacy 21-byte record layout and its block constants — is removed
  together with the include of the retired serializer. Nothing in ex11
  calls them (the heap file's page chain does that job now). What stays:
  `File`, `open_for_append`, `open_for_read`, `close`, `read_block` (both
  overloads), `write_block`, `block_count` — every one of them
  record-agnostic already.
- **`bufpool/BufferManager.cpp`** loses its one symbol from the retired
  serializer: `alloc_page`'s `initialize_block(data.buffer)` becomes a
  plain `std::memset(data.buffer, 0, kPageSize)` (that is all the helper
  did).
- **`main.cpp`** drops the include of the retired serializer; catalog
  test 2's comparison target becomes the literal 21 (the legacy record
  width, commented as such).
- `CMakeLists.txt`: `SOURCES` loses
  `tablesample/Person.h tablesample/PersonSerializer.cpp
  tablesample/PersonSerializer.h` and gains
  `schema/RecordCodec.cpp schema/RecordCodec.h`; nothing else changes.
- Includes after the change: `heapfile/` adds `tuple/Tuple.h`,
  `schema/TableSchema.h`, `schema/RecordCodec.h`; nothing in the build
  includes `deadcode/` — the folder is deliberately unreachable.

## 10a. Why plain removal is safe for BlockFile

`append_records`/`record_count` were the ex07 way to stream fixed records
into blocks — the exact job the heap file's slotted pages take over here.
Their only callers in ex11 are within `BlockFile.cpp` itself; ex09/ex10
keep their own copies. The repo has no git history for ex11 yet, so the
retired code lives on in `deadcode/` (the serialization half) and in
ex09/ex10's copies (the append half) — nothing is lost that isn't
preserved somewhere readable.

## 11. Test plan (appended to `main.cpp` in `check()` style)

1. **The layout test**: pack a person tuple (7, "Ruth", 42, "HU") through
   `schema::pack` with the loaded catalog and compare all 21 bytes against
   a hand-built expected buffer built straight from the section-3 table
   (pid@0, name@4 with NUL padding, age@14, city@18). The retired
   hardwired code produced the same bytes; it is no longer compiled,
   so the table is the source of truth.
2. **Codec round-trips**: person schema with short strings, empty
   strings, and full-width strings ("1234567890" in `name`); a car
   record through `car.json`; `unpack` returns the equal tuple.
3. **Codec errors**: string wider than its column ("12345678901" into a
   width-10 column), string containing NUL, wrong arity, wrong type, and
   a code-built schema with undeclared widths refused by `record_size`/
   `pack` — each with a non-empty named error and an untouched buffer.
4. **Padding integrity**: flip a padding byte inside a record's `name`
   field; `unpack` refuses ("padding contains garbage").
5. **Page math**: `record_size(person schema)` == 21 (the legacy record
   width); `slot_capacity(21)` == 185.
6. **Header page**: `create` writes magic + name; `open` accepts the
   matching schema and refuses a wrong-named one, a non-heap file, and a
   truncated file.
7. **Insert/find/scan round-trip**: several person tuples by `RowId`,
   fields read back by name; scan in chain order.
8. **Page split**: 200 inserts overflow the first page (capacity 185);
   `RowId`s stay valid, scan sees all 200 in insertion order.
9. **Erase and reuse**: erase mid-file, re-insert, free slot handed out
   lowest-first, scans skip erased row ids.
10. **Update**: overwrite in place (same size — always); record changed,
    occupancy unchanged.
11. **Schema guard on write**: wrong-arity/wrong-type/wide-string tuples
    refused; the file's record count unchanged afterwards.
12. **Persistence**: close, reopen with the same schema — all records
    intact; reopen with a different-named schema → refused.
13. **Discipline**: every failure leaves `error` non-empty and the
    record count unchanged.

## 12. Implementation steps

1. `schema/RecordCodec.h/.cpp`: `record_size`, `pack`, `unpack`; extend
   `TableSchema::validate` with the conditional width/NUL checks; rebuild
   — existing checks must still pass.
2. `SlottedPage`: swap the typed `get`/`put` for `get_record`/
   `put_record`; drop the `tablesample` includes; build clean.
3. `HeapFile`/`HeapFileIterator`: schema parameter, block-0
   identification record, tuple pipeline; drop the `tablesample`
   includes.
4. **Retire the legacy path**: create `deadcode/`, move
   `tablesample/Person.h`, `PersonSerializer.h`, `PersonSerializer.cpp`
   into it; trim `BlockFile` (drop `append_records`, `record_count`,
   `locate_append_position`, and the include of the retired serializer);
   swap `BufferManager.cpp`'s `initialize_block` for `std::memset`; fix
   `main.cpp` (drop the include, catalog test 2 against the literal 21).
5. Update `CMakeLists.txt`: remove the three `tablesample/Person*` source
   entries, add `schema/RecordCodec.*`; build clean — nothing in the
   build may reference `deadcode/`.
6. Append the section-11 check sections to `main.cpp`.
7. Build and run `./ex11/build/ex11`; all checks pass, exit 0.

## 13. Open questions

- **Reading of "fixed size"** — decided as column-wise fixed widths from
  the catalog, record = packed columns (§3). If you meant padding the
  tagged wire format into fixed-size slots instead, §3/§4 change.
- **Numeric widths** — natural 4/8 regardless of a catalog's numeric
  `size` (proposed), or refuse catalogs whose Integer/Double size
  differs from the natural width at load time?
- **NUL padding** — pad byte NUL, NUL forbidden inside strings, padding
  verified all-NUL on unpack (proposed). Alternative: space-padded
  (matching SQL CHAR semantics) with trailing spaces trimmed on unpack.
- **Record layout order** — tuple position order; `column_order` applies
  only to the tagged wire format (proposed).
- **Schema not persisted** — only magic + table name in block 0; the
  catalog is supplied at `open` (carried over from the previous draft).
