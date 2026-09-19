# EXPLAIN.md — how ex11 works, module by module

ex11 is a small relational storage engine built in layers. Its subject is
the step from hardwired records to **schema-driven** ones: a table is
described by a JSON catalog, its rows are generic self-describing tuples
on the wire and fixed-width records on disk, and a session CLI drives the
whole stack. Plain C++20, no external dependencies, single-threaded by
contract (one command at a time; threads, transactions, and locking are
future work).

The layers, bottom to top — each includes only the layers below it:

```text
ex11_cli        session CLI: REPL, table-name resolution, printing
schema/         TableSchema, JSON catalog loader, fixed-width record codec,
                CSV loader, row generator
heapfile/       SlottedPage (page layout), HeapFile (record chain)
bufpool/        BufferManager (pin/unpin, eviction, per-file close),
                BufferPool/DataFrame
policy/         ReplacementPolicy interface + LRU
file/           BlockFile (raw 4096-byte block I/O)
tuple/          Column, Tuple, TupleSerializer (the wire format)
```

Every layer reports failures the same way: a `bool` return plus an
`error` string out-parameter that is cleared on entry — an empty string
after the call means success. Exceptions are reserved for programming
errors (out-of-range access on a container-like object throws
`std::out_of_range`).

## tuple/ — self-describing tuples

`tuple::Column` is one value plus the two descriptions stored next to it:
its type (`ColumnType::Integer/String/Double`) and its payload length in
bytes. The constructors are the only writers, so the descriptions always
mirror the value — an `int` column has length 4, a `double` 8, a string
the size of its characters. The value itself lives in a
`std::variant<int, std::string, double>`, whose `.index()` equals the
type's wire tag by construction (0, 1, 2).

`tuple::Tuple` is an ordered vector of Columns — the logical row. It
grows through `add(int)`, `add(double)`, `add(const std::string&)`, and
`add(const Column&)`; equality compares all three fields of every
column; `column(i)` throws `std::out_of_range` for a bad index.

`TupleSerializer` moves tuples through buffers in a **self-describing
wire format**:

| Part | Size | Contents |
|---|---|---|
| count | 4 bytes | number of columns |
| per column: type | 4 bytes | the `ColumnType` value |
| per column: length | 4 bytes | payload byte count |
| per column: payload | `length` bytes | int (4), double (8), or the characters |

An empty tuple serializes to 4 bytes; a single int to 16; the worked
example `{42, "Ruth", 3.14}` to 44 (4 + 12 + 12 + 16). All integers are
native-endian `memcpy` copies — a single-host format. `serialized_size()`
computes the byte count without writing; `serialize()` refuses a buffer
smaller than that and leaves trailing bytes untouched; `deserialize()`
validates every column's tag and length against the type (a "Double"
tag with a 4-byte length is refused, as is an unknown tag), clears the
out-tuple first, and leaves it empty on any failure. Trailing bytes after
the last column are ignored, so a whole 4096-byte block can hold one
tuple.

Because each column carries its own tag and length, a generic reader can
walk the bytes without knowing the table — that is what makes this
format the **transport** encoding. Storage uses a different one (below).

## schema/ — TableSchema: the table's logical structure

`schema::ColumnSchema` is one column's metadata: name, type, position
(its slot in a schema-shaped tuple), and **declared width**
(`column_size` — a String's maximum character count, the byte width the
catalog declares for it; 0 means undeclared). `schema::TableSchema`
holds a table's name, its columns (added through `add_column`, the one
writer of positions and the name map), a `column_order` permutation, and
the operations built on them:

- `validate(tuple, error)` — the tuple must have one column per schema
  column, each with the right type at the right position. For String
  columns **with a declared width**, the value must fit it and must not
  contain NUL; undeclared columns stay unbounded.
- `make_tuple()` — builds a default-valued tuple for the schema (empty
  strings, zeros), the starting point for filling rows.
- `column_index(name)` — lookup by name; `column_info(i)` — metadata by
  index; both used by every higher layer to address columns by name.
- `install_column_order(...)` / `column_order()` — a permutation of
  positions. The default is identity; a non-identity order changes the
  **wire** order: the schema serializer writes columns in
  `column_order[0], column_order[1], ...` order and restores the logical
  order on read, refusing tuples whose shape disagrees. This exists to
  demonstrate that logical order and physical order can differ.
- `schema::serialize` / `schema::deserialize` — the wrapper pair that
  drives the tuple wire format from the schema: identical bytes to the
  plain serializer under identity order, plus per-column type guards on
  both directions (a tuple with the wrong type in a slot is refused, not
  silently reinterpreted). This is the **schema guard**: a record that
  disagrees with the declared structure fails with a named error instead
  of yielding wrong data.

## schema/TableSchemaJson — the catalog loader

A table's structure lives in a JSON catalog next to the engine, one file
per table:

```json
{
  "table_name": "person",
  "columns": [
    { "column_name": "pid",  "column_type": "Integer", "size": 4 },
    { "column_name": "name", "column_type": "String",  "size": 10 },
    { "column_name": "age",  "column_type": "Integer", "size": 4 },
    { "column_name": "city", "column_type": "String",  "size": 3 }
  ]
}
```

`load_table_schema(path, schema, error)` reads such a file and produces a
`TableSchema`; `parse_table_schema(text, ...)` is the string-level core
the file wrapper calls (tests use it directly). The mapping is one-for-one:
`table_name` becomes the schema name, each `columns` entry becomes one
`add_column` in file order (so positions are 0..n-1 and `column_order` is
identity), and `size` becomes the column's declared width. `column_type`
accepts exactly `Integer`, `String`, `Double`.

The JSON parser is hand-rolled recursive descent (~200 lines) covering
the grammar the catalog needs: objects, arrays, strings (standard
escapes, `\uXXXX` for BMP code points), and integers. Booleans, nulls,
and floating-point numbers are recognized and rejected with a message —
clearer than silently accepting more JSON than the format uses. Every
parse error names its line (`line 3: expected ':'`); every content error
names the offending key or column (missing keys, unknown keys, empty
names, unknown types, non-positive sizes, duplicate column names, an
empty columns array). Loading is all-or-nothing: the out-schema is empty
unless the whole document parsed and validated, so a failure never leaves
a half-loaded schema behind.

## schema/RecordCodec — fixed-width records (the storage encoding)

The heap file stores records of **one fixed size per table**, derived
entirely from the catalog. `record_size(schema)` sums the column widths:
4 bytes per Integer, 8 per Double, `column_size` per String. For the
person catalog that is 4+10+4+3 = **21 bytes** — the same layout older
exercises hardwired for their `Person` record, now derived from the
catalog instead of baked into a struct.

The record layout, in tuple position order:

| Column | Width | Contents |
|---|---|---|
| Integer | 4 | the int, native endianness |
| Double | 8 | the double |
| String | `column_size` | value bytes, then NUL padding to the full width |

`schema::pack(schema, tuple, buffer, max_len, error)` validates the tuple
first (arity, types, widths — failing before touching the buffer), then
copies each column to its fixed offset. `schema::unpack` rebuilds the
tuple and verifies that every String field's padding is all NUL ("padding
contains garbage") — the fixed-format integrity check that replaces the
wire format's tags. Both refuse String columns with no declared width:
record packing needs catalog-fed schemas.

Two encodings, two jobs: the tagged wire format is self-describing and
generic (any reader can walk it); the fixed-width record format is
compact and position-addressed (the schema is the tag). Neither knows the
other; both are driven by the same `TableSchema`. The trade-off is
documented: without tags there is no per-field integrity for numerics —
a corrupted int/double field unpacks as silent garbage; only String
padding catches damage.

## heapfile/SlottedPage — the page layout

A `SlottedPage` is a non-owning view over one page-sized buffer (a
buffer-pool frame) that interprets the bytes as a grid of fixed-length
records. The record size is a `bind()` parameter — the page never knows
what the bytes mean, only how many fit.

Page layout (4096 bytes, all integers native-endian):

| Region | Contents |
|---|---|
| `[0, record_size * capacity)` | record slots, back to back |
| occupancy array | one byte per slot, growing down from the header |
| header (last 12 bytes) | slot count, prev page id, next page id |

`slot_capacity(record_size) = (4096 - 12) / (record_size + 1)` — each
slot costs its record plus one occupancy byte. For 21-byte person records
that is 185 slots with 14 bytes dead. The page operations are the layout
primitives: `format` (zero the occupancy array, set the chain links),
`attach` (validate the stored slot count against the record size —
corrupted or foreign bytes are a hard error), `first_free_slot` (lowest
index first), `is_occupied`, `get_record`/`put_record` (bounds-checked
copies of exactly `record_size` bytes), and `erase` (clear the occupancy
byte; the record bytes stay behind as garbage and the slot becomes
reusable).

## heapfile/HeapFile — records in a page chain

A `HeapFile` is a file of fixed-width tuple records stored in a chain of
slotted pages, addressed by `RowId{page_id, slot}` (page ids are block
numbers; block 0 is the header). `create`/`open` take the table's
`TableSchema`, which fixes the record size for the file's lifetime.

- **Block 0 is the header page**: formatted like a data page but its
  slots are never used. Its record area carries a small identification
  record: the magic `0x31505554` ("TUP1"), the table name's length, and
  the name itself. `open` checks the magic ("not a tuple heap file"
  instead of a puzzling arithmetic error), compares the name ("heap file
  is for table 'person'"), and then attaches the page — which also
  rejects files whose slot grid disagrees with the schema's record size.
  The schema object itself is **not** stored in the file: the caller
  loads it from the catalog and owns it; only the name travels in-band.
- **insert** validates the tuple against the schema, packs it into a
  local buffer, pins the header, walks the chain for the first page with
  a free slot, and claims the lowest free slot; when every page is full
  it allocates a new tail page, splices it into the chain, and uses its
  slot 0. Pages are allocated densely and never removed.
- **find/update/erase** share one pin-and-validate path: the row id must
  not name the header page, must exist (on disk or resident — dirty
  pages lag the disk until eviction), and its slot must be occupied.
  `find` copies the record out and unpacks it through the schema, so a
  record that violates the catalog fails with a named error rather than
  yielding wrong data. `update` overwrites in place (records are all the
  same size, so update is a plain overwrite); `erase` frees the slot.
- **scan** returns an iterator that walks the chain head to tail, one
  pinned page at a time, yielding each occupied slot's tuple — plus its
  `RowId` via `row_id()`, the address the other commands consume.
  Mutation during iteration is deliberately unspecified.
- **Lifecycle**: `close` flushes and closes *only this file* (see
  `close_file` below); later operations fail with "heap file is not
  open". `resident(page_id)` reports whether a page is currently in the
  pool — the introspection a CLI needs to print pool hits.
- `erase_file` removes a file from disk, refusing while it is open.

## file/, bufpool/, policy/ — blocks, frames, the manager

`file/BlockFile` is the raw block layer: every file is a whole number of
4096-byte blocks; `open_for_append`/`open_for_read`/`close` manage
descriptors, `read_block`/`write_block` move exactly one block, and
`block_count` reports the size. It knows nothing about records.

`bufpool/BufferPool` holds a fixed set of frames (`DataFrame`: the
4096-byte buffer plus a pin count and a dirty flag). `BufferManager` sits
above it: a page table keyed by `(file descriptor, block number)` maps
resident pages to frames — files never collide in the pool — `pin` brings
a block in (evicting an unpinned victim through the injected
`ReplacementPolicy` when the pool is full, writing dirty victims back),
`alloc_page` formats a brand-new page beyond the file's end (allocated
densely: no gaps are ever created), and `unpin` releases the pin and
marks dirtiness. `flush`/`flush_all` write dirty pages back; `close_all`
flushes and closes everything.

`policy/ReplacementPolicy` is the eviction interface (`init`, `on_access`,
`on_load`, `on_remove`, `pick_victim`); `LRUPolicy` is the shipped
implementation. The policy is injected at `init`, so eviction behavior is
pluggable without touching the manager.

The one addition ex11 makes to this stack: **`close_file(file, error)`**
— flush this file's dirty pages, drop its pool entries, close its
descriptor, leave every other file untouched (refusing while one of its
frames is pinned). With it, a `HeapFile` shares the session's manager
with other files and still owns its own lifecycle: closing one file no
longer slams shut the rest (which `close_all` would have done). The heap
file's contract reads "shares the session's manager; owns its file's
lifecycle".

## schema/TableCsv — loading rows from CSV

`schema::TableCsv` is the general CSV loader: one module for every table,
because everything the per-table loaders of earlier exercises hardwired
(field count, types, field names for error messages) is what
`TableSchema` declares.

- `parse_csv_row(schema, line, tuple, reason)` — the row-level core:
  split on commas (no quoting or escaping — a comma or NUL cannot appear
  inside a value), exactly one token per column in position order,
  Integer/Double parsed strictly and fully consumed
  (`from_chars`; the Double path uses guarded `strtod` for toolchain
  portability), String tokens verbatim. The module refuses a row only
  for syntax; width and NUL rules surface at insert time through
  `validate`/`pack`, with the schema's named error.
- `load_csv(schema, path, result, diagnostics, error)` — the file
  wrapper: valid rows in file order plus a skipped count; invalid rows
  print `line N: reason` and are skipped, never fatal; blank lines are
  skipped silently; a trailing `\r` is tolerated. A header row is simply
  a row that fails type parsing. The module produces tuples and knows
  nothing of `HeapFile` — the caller owns insertion.

## schema/TableGenerator — generating rows from a schema

`schema::TableGenerator::generate_tuples(schema, count, seed, first_id)`
produces `count` pseudo-random tuples that always pass `validate`:
a seeded `std::mt19937` drives Integers in `[0, 1000)`, Doubles rounded
to one decimal in `[0, 100)`, and Strings of random letters with length
in `[1, column_size]` — never wider than declared, never containing NUL.
The **first Integer column** (position order) is not random: it counts
`first_id, first_id + 1, ...`, the record's visible identity — so
generated rows are readable in scans and numbering can continue after
existing records. Same inputs produce identical tuples, which makes both
the checks and the demos reproducible.

## ex11_cli — the session CLI

`ex11_cli` is a REPL over the heap file, modeled on the command surface
of the earlier exercises' demo tool. One `BufferManager` (LRU, 8 frames)
is created before the prompt loop and shared by every heap file in the
session — the shape the manager's `(fd, block)` page table was designed
for; pages one command brings in are hits for the next command. Session
state is one cohesive unit: the manager, the catalog cache, and the file
table. When multiple users and sessions arrive (with transactions and
locking), each session instantiates that state; until then the engine is
single-threaded by contract.

- **Table names, not paths.** Commands type `<table>` as a bare name —
  `person`, `car` — and the CLI resolves it to
  `tablecatalog/<name>.json`, caching one schema per name. An unknown
  name fails with `unknown table 'x' (no tablecatalog/x.json)`; typing a
  path or the extension is refused with a hint. A file binds to the
  table it was first used with; a later command naming a different table
  is refused.
- **The commands**:

  | Command | Syntax | Effect |
  |---|---|---|
  | `append` | `append <table> <csv-file> <file>` | Loads the CSV through `TableCsv`, creating the file when missing, and inserts every row; rows the schema refuses (e.g. a too-wide string) are reported and skipped. Ends with `appended N record(s), skipped M row(s)` and a `flush_all` — pages stay resident for later reads. |
  | `read` | `read <table> <file> <page> <slot>` | One record by row id, printed with its row id and a `(pool hit)` marker when the page was already resident. |
  | `seek` | `seek <table> <file> <page> <slot>...` | The batch form of `read`: one or more row ids, duplicates included; a bad row id prints a message and the loop continues. |
  | `scan` | `scan <table> <file>` | The whole chain with row ids, ending with `scanned N record(s)`. |
  | `bulk` | `bulk <table> <count> <file>` | Rows from `TableGenerator`, numbering continuing after the existing record count. |
  | `quit` | `quit` | Closes every session file exactly once — one flush per file through the shared manager; exit code 1 only if a flush fails. |

- Every failure prints a message on stderr and the prompt returns; the
  session never dies on bad input. The prompt is suppressed when input
  is piped, so sessions script cleanly.

## main.cpp — the check suite

The `ex11` binary is the regression harness: 334 PASS/FAIL `check()`
calls covering the tuple wire format (sizes, round-trips, corruption
refusals), the schema layer (validation, by-name mapping, wire-order
permutation), the catalog loader (both sample catalogs, parse- and
content-level errors, all-or-nothing semantics), the record codec (the
byte-level layout test, round-trips, padding integrity), the heap file
(insert/find/update/erase/scan, page splits at 185 records per page,
schema guards on both write and read paths, persistence, two files
sharing one manager, per-file close), the CSV and generator modules, and
the buffer manager. Exit code 0 iff every check passes.

## Design decisions worth knowing

- **Two encodings, one schema.** The self-describing wire format moves
  tuples between buffers; fixed-width records store them on disk. Both
  derive from the same catalog; neither knows the other.
- **Declared width vs actual length.** `column_size` is the catalog's
  promise about a String's maximum width; `tuple::Column`'s length is
  what a concrete value is. `validate` enforces the promise; the wire
  format ignores it; the record format is built from it.
- **The catalog is out-of-band.** Heap files store only magic + table
  name; the schema itself lives in the JSON catalog and is supplied at
  open. The file identifies itself; it does not describe itself.
- **`pid == 0` is legal.** Free slots are tracked by occupancy bytes,
  not by a sentinel value in the record — a row whose first column is 0
  stores and reads back like any other.
- **Errors are values.** Every storage-layer failure is a named message
  through the `bool` + `error` convention; the layers above decide
  whether it is fatal. The CLI prints and continues; the check suite
  asserts.
- **Single-threaded by contract.** One command at a time, no internal
  locks. Threads, multiple users and sessions, transactions, and locking
  are the planned next exercises; the session state is already kept as
  one cohesive unit so it can be instantiated per session when that day
  comes.
