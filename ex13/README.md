# ex13 — a SQL front door with an ISAM index

ex13 bolts a query language and the engine's first access path onto the
ex11 storage stack. The storage modules and the ex12 SQL front-end are
vendored in unchanged; the new work is an **ISAM index** on one column
(bulk-built, never rebalanced), an **exec layer** that binds the parsed
statements against the catalog and picks an access path, a **session
CLI** that speaks SQL, a **bulk loader** that bootstraps empty heap
files at raw-write speed, and a **parts benchmark** that measures the
index against a full heap scan. Plain C++20, no external dependencies,
single-threaded by contract.

```sql
CREATE TABLE person (pid INTEGER, name VARCHAR(10), age INT, city VARCHAR(3));
INSERT INTO person VALUES (1, 'Ruth', 42, 'PR');
CREATE INDEX person_pid ON person (pid);
SELECT name, age FROM person WHERE pid > 10 AND pid < 50;   -- (via index scan)
SELECT name FROM person WHERE city = 'PR';                  -- (via heap scan)
```

## Project layout

```text
tablecatalog/   table catalogs (JSON), now written by the engine:
                CREATE TABLE / CREATE INDEX land here
person.csv      100 sample person rows (bulk/escape hatches)
car.csv         100 sample car rows
tuple/          Column, Tuple — the logical row            (vendored ex11)
schema/         TupleSerializer, TableSchema, catalog loader
                + NEW catalog WRITER (the DDL seam)        (vendored ex11)
heapfile/       SlottedPage, HeapFile                      (vendored ex11)
bufpool/        BufferManager, BufferPool, DataFrame       (vendored ex11)
policy/         ReplacementPolicy + LRUPolicy              (vendored ex11)
file/           BlockFile — raw 4096-byte block I/O        (vendored ex11)
sql/            Lexer, Parser, AST, printer, drawing       (vendored ex12,
                + CREATE INDEX / DROP INDEX statements)
index/          NEW: KeyCodec, the three page views, IsamIndex
exec/           NEW: Binder, Evaluator, Planner, Executor
tools/          NEW: BulkLoader — the bootstrap loader for empty
                heap files (raw block writes, no buffer pool)
ex13_cli.cpp    the session CLI: SQL REPL + escape hatches
parts_bench.cpp the index-vs-heap benchmark over generated parts rows
main.cpp        the check suite (the ex13 binary)
EXPLAIN.md      the design and implementation, module by module
```

## Building

Requires CMake 4.3+ and a C++20 compiler.

```sh
cmake -S . -B build
cmake --build build
```

Binaries: `build/ex13` (checks), `build/ex13_cli` (session), and
`build/parts_bench` (benchmark).

## Running the check suite

```sh
build/ex13
```

One `PASS:`/`FAIL:` line per check, five suites in order: the vendored
ex11 storage suite, the vendored ex12 sql suite, the index-module suite,
the exec-layer suite — ending in the **differential oracle**: 300 random
predicates over 500 random rows, each SELECT executed twice (index
allowed, index suppressed) with both answers required to be identical —
and the bulk-loader suite. Exit code 0 when everything passes.

## Running the session CLI

```sh
build/ex13_cli
```

One BufferManager (LRU, 8 frames) lives for the whole session and is
shared by every heap file and index file. SQL statements accumulate
across lines until a `;`; errors print a message and the prompt returns;
the prompt is suppressed when input is piped; `quit` (or EOF) flushes
and closes everything once — exit code 1 only if a flush fails.

### Statement surface

| Statement | Notes |
|---|---|
| `CREATE TABLE t (col type, ...)` | types: `INTEGER`/`INT` (4 bytes), `VARCHAR(n)`, `DOUBLE` (8). Writes `tablecatalog/t.json` through the catalog writer. |
| `DROP TABLE t` | removes the catalog, the data file `data/t.tbl`, and every index file. |
| `CREATE INDEX name ON t (col)` | one index per column; builds over the table's current rows and records itself in the catalog. |
| `DROP INDEX name` | erases the index file and the catalog entry. |
| `INSERT INTO t VALUES (...), ...` | all-or-nothing per statement; the heap row lands first, then every index of the file. |
| `SELECT cols FROM t WHERE expr` | single table; projection is `*` or plain columns (an alias may rename one); WHERE is the full expression language. |

Refused with a named error (the grammar still accepts them): joins,
`GROUP BY`, `ORDER BY`, `LIMIT`, aggregates, `UPDATE`, `DELETE` — UPDATE
and DELETE are deliberately left as student project work.

### Access paths

Every SELECT announces its path:

- `(via index scan)` — the WHERE contains an equality or an AND-combined
  range on the indexed column, pinned to literal bounds (`pid = 42`,
  `pid > 10 AND pid < 50`); remaining conjuncts filter the fetched rows.
  Further conjuncts on the same column only tighten the bounds
  (`id > 2 AND id = 1` answers nothing); a literal of the other numeric
  type (`id = 2.0` on an int column) stays a filter, and a string bound
  wider than the column is cut to the column width, so the index path
  answers what the heap path answers.
- `(via heap scan)` — anything else, including any top-level `OR`.

The choice never changes the answer: both paths return the same rows
(as a set — without ORDER BY, row order is unspecified and the index
walks keys while the heap scans row ids). The check suite enforces this
with the differential oracle.

### Expression semantics

Mixed int/double arithmetic and comparison promote to double; integer
division truncates; strings compare lexicographically; no NULLs;
comparing a string with a number, arithmetic on strings, non-boolean
logic operands, and unknown columns are **bind-time** errors — nothing
is read before the statement type-checks.

### Escape hatches (ex11 commands, unchanged semantics)

| Command | Syntax |
|---|---|
| `scan` | `scan <table> <file>` — every record with its row id |
| `read` | `read <table> <file> <page> <slot>` — one record, `(pool hit)` when resident |
| `seek` | `seek <table> <file> <page> <slot>...` — batch read |
| `bulk` | `bulk <table> <count> <file>` — generated rows, numbering continuing after the existing records, indexes maintained |
| `checkindex` | `checkindex <table> <file>` — verifies every declared index: entry counts, dangling row ids, key order |
| `help` | prints the SQL and escape-hatch summary |
| `quit` | flushes and closes the session |

A `<file>` argument with no `/` resolves inside `data/` and gains the
`.tbl` extension unless it already has it (`scan parts parts` reads
`data/parts.tbl`); a path containing a `/` is used exactly as given.

### Where files live

A SQL table's data file is `data/<table>.tbl` (the `data` directory is
created on demand); its index files are `data/<table>.tbl.<column>.idx`
beside it. Opening a data
file builds any catalog-declared index file that is missing (lazy
rebuild). The escape hatches keep ex11's arbitrary-path behavior.

### Bulk loading

Two routes fill a table with many rows:

- **Session route — the `bulk` escape hatch.** `bulk <table> <count>
  <file>` generates `count` rows for a catalog-declared table (seed 42;
  numbering continues after the existing records, starting at 1 on a
  fresh table) and inserts them per-row through the executor — the data
  file is created on demand and every declared index is maintained as
  it goes:

  ```sql
  CREATE TABLE person (pid INTEGER, name VARCHAR(10), age INT, city VARCHAR(3));
  bulk person 100 person                  -- appends 100 generated rows
  CREATE INDEX person_pid ON person (pid);
  ```

  Fine for hundreds of rows; the per-row insert walk grows quadratic,
  so don't feed it millions.

- **Bootstrap route — the bulk loader.** `tools::bulk_load`
  (tools/BulkLoader.h) writes an entire empty heap file in one
  sequential pass: raw 4096-byte block I/O, no buffer pool, each block
  written exactly once. Call it from C++ right after CREATE TABLE,
  before anything opens the data file through a buffer manager:

  ```cpp
  tools::BulkLoadStats stats;
  std::string error;
  const tools::RowGenerator generator =
      [&rows](std::size_t index, tuple::Tuple& out, std::string&) {
          out = rows[index];              // row `index` of the batch
          return true;                    // false + error aborts the load
      };
  tools::bulk_load("data/parts.tbl", *schema, rows.size(), generator,
                   stats, error);
  ```

  It accepts only a missing file (created and formatted) or an empty
  one-block file (verified and reused); anything else is refused by
  name before any write. The layout is engine-equivalent — the same
  slotted pages, row ids in the same `(page, slot)` pattern — so
  `HeapFile::open`/`scan`/`find` treat it as their own, and a later
  `CREATE INDEX` builds over the loaded rows. `build/parts_bench` is
  the worked end-to-end example: CREATE TABLE, bulk load, CREATE INDEX,
  measure.

With either route the order is load first, index second: `CREATE INDEX`
builds over the rows already in the file, and the escape-hatch `bulk`
maintains existing indexes as it inserts.

## Running the parts benchmark

```sh
build/parts_bench                 # default: 1,000,000 rows
build/parts_bench --rows 100000   # smaller run
build/parts_bench --reuse         # re-measure on the existing data
```

One run: drops any leftover `parts` indexes (and the table itself,
unless `--reuse`), creates the `parts` table, bulk-loads generated rows
(sequential `partid`, seven colors, clamped-normal price and weight,
fixed seed), measures Q1 (`partid = 54645`) and Q2 (`pprice >= 1.0 AND
pprice <= 10.0`) on the heap path, builds an index on each query's
column in turn, and re-runs the same queries on the index path. The
report prints each query's full text followed by its heap vs index
times with speedups, both index build times, the load time, the data
fingerprint, and the identical-answers verdict for both paths. `--reuse` skips table creation and
loading (the row count is the existing table's; the data must be there
or the run fails by name), still regenerates the rows in memory to
verify the reused file, and marks the report as reused. Combining
`--rows` with `--reuse` is an argument error.

## Architecture overview

```text
ex13_cli      SQL REPL + escape hatches
parts_bench   index-vs-heap benchmark (--rows N, --reuse)
exec/         Executor (dispatch, session state), Binder (names -> positions,
              static types), Evaluator (Expr -> value), Planner (access path)
index/        IsamIndex: bulk build, lookup, range, insert, verify
              KeyCodec + SeparatorPage/LeafPage/OverflowPage views
tools/        BulkLoader: bootstrap loads of empty heap files (raw block
              I/O, no buffer pool, engine-equivalent result)
sql/          the ex12 front-end + the two index statements
schema/       ex11's schema layer + the catalog writer (DDL seam)
heapfile/     ex11's heap: slotted pages, page chain, RowId{page, slot}
bufpool/      ex11's buffer manager: pin/unpin, LRU eviction, per-file close
file/         ex11's BlockFile
tuple/        ex11's logical row
```

## More

`EXPLAIN.md` explains every module, the ISAM file format page by page,
the access-path rules, and the design decisions.
