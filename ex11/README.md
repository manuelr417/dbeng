# ex11 — a schema-driven storage engine

ex11 is the next step of a small database engine built up exercise by
exercise. The exercises before it hardwired their record layouts into
structs and serializers; ex11 makes the table's structure **data**: a
table is described by a JSON catalog, its rows are generic tuples, and a
heap file stores them in fixed-width records whose layout is derived from
the catalog rather than baked into code. A session CLI drives the whole
stack — loading rows from CSV, generating rows, reading them back by row
id. Plain C++20, no external dependencies, single-threaded by contract.

## Project layout

```text
tablecatalog/   table catalogs (JSON): person.json, car.json
tuple/          Column, Tuple, TupleSerializer — the self-describing wire format
schema/         TableSchema, catalog loader (TableSchemaJson),
                fixed-width record codec (RecordCodec),
                CSV loader (TableCsv), row generator (TableGenerator)
heapfile/       SlottedPage (page layout), HeapFile (records in a page chain)
bufpool/        BufferManager, BufferPool, DataFrame
policy/         ReplacementPolicy interface + LRUPolicy
file/           BlockFile — raw 4096-byte block I/O
main.cpp        the check suite (the ex11 binary)
ex11_cli.cpp    the session CLI (the ex11_cli binary)
EXPLAIN.md      the design and implementation, module by module
```

All sources compile once into the `ex11core` static library; the two
executables link it.

## Building

Requires CMake 4.3+ and a C++20 compiler.

```sh
cmake -S . -B build
cmake --build build
```

The binaries end up in `build/ex11` and `build/ex11_cli`. (CLion builds
into `cmake-build-debug/` instead; both directories are gitignored.)

## Running the check suite

```sh
build/ex11
```

Takes no arguments. It prints one `PASS:`/`FAIL:` line per check — 334 of
them, covering the tuple wire format, the schema layer, the catalog
loader, the record codec, the heap file (insert/find/update/erase/scan,
page splits, persistence, schema guards), the CSV and generator modules,
and the buffer manager — then a summary. Exit code is `0` when all checks
pass, `1` otherwise.

```sh
build/ex11 | tail -1        # "All checks passed."
```

## Running the session CLI

```sh
build/ex11_cli
```

Takes no command-line parameters and starts an interactive session: one
buffer manager (LRU, 8 frames) lives for the whole session and is shared
by every heap file, files are opened on first use and kept open, and
`quit` (or end of input) flushes and closes everything once — exit code 1
only if a flush fails. Errors print a message and the session continues.
When input is piped in, the prompt is suppressed, so sessions script:

```sh
printf 'scan person people.bin\nquit\n' | build/ex11_cli
```

Commands select a table by **name** — the CLI resolves it to
`tablecatalog/<name>.json` itself (never type the directory or the
`.json`):

| Command | Syntax | Effect |
|---|---|---|
| `append` | `append <table> <csv-file> <file>` | Loads the CSV (invalid rows are reported as `line N: reason` and skipped) and inserts every valid row, creating the file when missing. Ends with `appended N record(s), skipped M row(s)`. |
| `read` | `read <table> <file> <page> <slot>` | Prints one record by row id, with a `(pool hit)` marker when the page was already in the pool. |
| `seek` | `seek <table> <file> <page> <slot>...` | The batch form of `read`: one or more row ids, duplicates included; a bad row id prints a message and the loop continues. |
| `scan` | `scan <table> <file>` | Prints every record with its row id, ending with `scanned N record(s)`. |
| `bulk` | `bulk <table> <count> <file>` | Appends `<count>` generated rows (numbering continues after the existing records). |
| `quit` | `quit` | Flushes and closes the session. |

Example session:

```text
> append person people.csv people.bin
line 1: column 'pid': expected an integer
appended 2 record(s), skipped 1 row(s)
> bulk person 420 people.bin
appended 420 generated record(s)
> scan person people.bin
(1,0)  pid=1, name=Alice, age=30, city=PR
(1,1)  pid=2, name=Bob, age=25, city=NY
...
scanned 422 record(s)
> read person people.bin 1 0
(1,0)  (pool hit)  pid=1, name=Alice, age=30, city=PR
> seek person people.bin 1 1 1 0
(1,1)  (pool hit)  pid=2, name=Bob, age=25, city=NY
(1,0)  (pool hit)  pid=1, name=Alice, age=30, city=PR
> quit
```

### CSV input format

One record per line, comma-separated, one field per column in catalog
order — no quoting or escaping, so a comma cannot appear inside a value.
A header row is accepted (it fails type parsing and counts as one skipped
row); blank lines are skipped; files with Windows line endings load fine.
Numbers are parsed strictly and fully consumed; strings are checked
against their declared width at insert time. Invalid rows never abort
the session — they are reported and skipped.

### Adding a table

A table is a JSON file in `tablecatalog/` — no code changes:

```json
{
  "table_name": "part",
  "columns": [
    { "column_name": "part_id", "column_type": "Integer", "size": 4 },
    { "column_name": "name",    "column_type": "String",  "size": 15 }
  ]
}
```

`column_type` is `Integer`, `String`, or `Double`; `size` is a String's
declared width (the byte width of fixed-size columns is documentation).
Once the file exists, the CLI's commands accept its name immediately.

## Architecture overview

The layers, bottom to top — each includes only the layers below it:

```text
ex11_cli     session CLI: REPL, table-name resolution, printing
schema/      TableSchema, catalog loader, record codec, CSV loader, generator
heapfile/    SlottedPage (page layout), HeapFile (records in a page chain)
bufpool/     BufferManager (pin/unpin, eviction, per-file close), BufferPool
policy/      ReplacementPolicy interface + LRU
file/        BlockFile (raw 4096-byte block I/O)
tuple/       Column, Tuple, TupleSerializer (the self-describing wire format)
```

The engine carries **two encodings for one schema**. On the wire, a tuple
is self-describing — a column count plus, per column, a type tag, a
length, and the payload — so a generic reader can walk it without knowing
the table. On disk, records are fixed-width: each column sits at an
offset derived from the catalog's declared widths (for the `person`
catalog: 4+10+4+3 = 21 bytes per record, 185 records per 4096-byte page),
with the schema acting as the tag. The heap file stores them in a chain
of slotted pages addressed by `RowId{page, slot}`; block 0 is a header
page carrying the chain head plus the table's name, so a file can be
verified against the catalog it is opened with. All of it is driven by
one `BufferManager` per session: pages are pinned into a pool keyed by
`(file, block)`, dirty pages reach disk on flush or eviction, and closing
one file never disturbs another.

## More

`EXPLAIN.md` explains every module, the formats, and the design
decisions behind them — read it top to bottom after skimming this file.
