# ex14 — a second access path: the extensible hash index

ex14 adds the engine's second index structure to the ex13 stack: an
**extensible hash index** (lecture 11) — a growable directory of bucket
pointers, splits with directory doubling, overflow chains for
duplicate-crowded buckets — alongside the vendored ISAM index. The
storage modules, the SQL front-end, the ISAM module, the exec layer,
the bulk loader, the CLI, and the benchmark are vendored in unchanged;
the new work is the `hashindex/` module, the `USING HASH | ISAM` clause
(default HASH), entry **erase on both index kinds** (ISAM leaves gaps,
hash removes from the bucket chain; neither merges), a third announced
access path, a `delrow` escape hatch, and a benchmark and check suite
extended to the hash path. Plain C++20, no external dependencies,
single-threaded by contract.

```sql
CREATE TABLE person (pid INTEGER, name VARCHAR(10), age INT, city VARCHAR(3));
INSERT INTO person VALUES (1, 'Ruth', 42, 'PR');
CREATE INDEX person_pid ON person (pid);                     -- hash (default)
CREATE INDEX person_pid_isam ON person (pid) USING ISAM;     -- both kinds, one column
SELECT name, age FROM person WHERE pid = 42;                 -- (via hash scan)
SELECT name, age FROM person WHERE pid > 10 AND pid < 50;    -- (via index scan)
SELECT name FROM person WHERE city = 'PR';                   -- (via heap scan)
```

## Project layout

```text
tablecatalog/   table catalogs (JSON), written by the engine:
                CREATE TABLE / CREATE INDEX land here; index entries
                carry a "kind" ("hash"/"isam", absent meaning isam)
tuple/          Column, Tuple — the logical row            (vendored ex11)
schema/         TupleSerializer, TableSchema, catalog loader
                + catalog WRITER (the DDL seam)            (vendored ex11)
heapfile/       SlottedPage, HeapFile                      (vendored ex11)
bufpool/        BufferManager, BufferPool, DataFrame       (vendored ex11)
policy/         ReplacementPolicy + LRUPolicy              (vendored ex11)
file/           BlockFile — raw 4096-byte block I/O        (vendored ex11)
sql/            Lexer, Parser, AST, printer, drawing       (vendored ex12,
                + CREATE INDEX [USING HASH|ISAM] / DROP INDEX)
index/          KeyCodec, the three page views, IsamIndex  (vendored ex13,
                + NEW entry erase: gaps left, never merged)
hashindex/      NEW: the two hash index kinds — ExtensibleHashIndex
                (directory/bucket/overflow views, the hash function;
                build, lookup, insert with split and directory
                doubling, erase, verify) and StaticHashIndex (lecture
                11's fixed-N baseline: h(key) mod N routing, overflow
                spill without splits; module-level, exercised by its
                own check suite)
exec/           Binder, Evaluator, Planner, Executor       (vendored ex13,
                + kind-aware access paths, erase fan-out)
tools/          BulkLoader — the bootstrap loader for empty
                heap files (raw block writes, no buffer pool)
ex14_cli.cpp    the session CLI: SQL REPL + escape hatches (+ delrow)
parts_bench.cpp the benchmark over generated parts rows: heap vs hash
                vs ISAM for equality (Q1), heap vs ISAM for ranges (Q2)
main.cpp        the check suite (the ex14 binary)
EXPLAIN.md      the design and implementation, module by module
```

## Building

Requires CMake 4.3+ and a C++20 compiler.

```sh
cmake -S . -B build
cmake --build build
```

Binaries: `build/ex14` (checks), `build/ex14_cli` (session), and
`build/parts_bench` (benchmark).

## Running the check suite

```sh
build/ex14
```

One `PASS:`/`FAIL:` line per check, seven suites in order: the vendored
ex11 storage suite, the vendored ex12 sql suite, the ISAM-module suite
(with its erase checks), the hash-index-module suite (extensible), the
static-hash-module suite, the exec-layer suite, and the bulk-loader
suite — ending with the **differential
oracle**: 300 random predicates over 500 random rows, each SELECT
executed once per available path (both kinds, hash only, ISAM only,
heap) with all answers required to be identical. Exit code 0 when
everything passes.

## Running the session CLI

```sh
build/ex14_cli
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
| `CREATE INDEX name ON t (col) [USING HASH \| ISAM \| STATIC HASH]` | one index per column and kind (three kinds); the kind defaults to HASH. Builds over the table's current rows and records itself (with its kind) in the catalog. SQL-created static hash indexes use a fixed default bucket count. |
| `DROP INDEX name` | erases the index file and the catalog entry. |
| `INSERT INTO t VALUES (...), ...` | all-or-nothing per statement; the heap row lands first, then every index of the file (both kinds). |
| `SELECT cols FROM t WHERE expr` | single table; projection is `*` or plain columns (an alias may rename one); WHERE is the full expression language. |

Refused with a named error (the grammar still accepts them): joins,
`GROUP BY`, `ORDER BY`, `LIMIT`, aggregates, `UPDATE`, `DELETE` — UPDATE
and DELETE are deliberately left as student project work.

### Access paths

Every SELECT announces its path:

- `(via hash scan)` — the WHERE contains an equality on a column with a
  hash index, pinned to a literal (`pid = 42`); remaining conjuncts
  filter the fetched rows. A range never takes this path — hash answers
  equality only.
- `(via static scan)` — an equality on a column with a static hash
  index, pinned to a literal; remaining conjuncts filter the fetched
  rows. A range never takes this path — the static index answers
  equality only.
- `(via index scan)` — an equality or AND-combined range on a column
  with an ISAM index, pinned to literal bounds (`pid = 42`, `pid > 10
  AND pid < 50`); remaining conjuncts filter the fetched rows. With
  several kinds on the column, an equality prefers the hash, then the
  static scan, then the ISAM scan; a range-only conjunct set takes the
  ISAM scan.
- `(via heap scan)` — anything else, including any top-level `OR`.

The choice never changes the answer: every path returns the same rows
(as a set — without ORDER BY, row order is unspecified and the index
walks keys or hash buckets while the heap scans row ids). The check
suite enforces this with the differential oracle.

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
| `delrow` | `delrow <table> <file> <page> <slot>` — erases one record: the heap slot first, then every index of the file |
| `checkindex` | `checkindex <table> <file>` — verifies every declared index (all three kinds): entry counts, dangling row ids, ISAM key order, hash and static-hash placement |
| `help` | prints the SQL and escape-hatch summary |
| `quit` | flushes and closes the session |

A `<file>` argument with no `/` resolves inside `data/` and gains the
`.tbl` extension unless it already has it (`scan parts parts` reads
`data/parts.tbl`); a path containing a `/` is used exactly as given.

### Where files live

A SQL table's data file is `data/<table>.tbl` (the `data` directory is
created on demand); its index files sit beside it —
`data/<table>.tbl.<column>.idx` for ISAM,
`data/<table>.tbl.<column>.hdx` for hash, and
`data/<table>.tbl.<column>.shdx` for static hash. Opening a data file
builds any catalog-declared index file that is missing (lazy rebuild,
with its
catalog kind). The escape hatches keep ex11's arbitrary-path behavior.

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
pprice <= 10.0`) on the heap path, builds a hash index on `partid` and
ISAM indexes on `partid` and `pprice` in turn, and re-runs Q1 on both
index kinds (hash scan, then ISAM with the hash suppressed) and Q2 on
the ISAM path. The report prints per-query heap vs index times with
speedups, all index build times, the load time, the data fingerprint,
and the identical-answers verdict for every path. `--reuse` skips
table creation and loading (the row count is the existing table's; the
data must be there or the run fails by name), still regenerates the
rows in memory to verify the reused file, and marks the report as
reused. Combining `--rows` with `--reuse` is an argument error.

## Architecture overview

```text
ex14_cli      SQL REPL + escape hatches (incl. delrow)
parts_bench   benchmark: heap vs hash vs ISAM (--rows N, --reuse)
exec/         Executor (dispatch, session state, erase fan-out), Binder
              (names -> positions, static types), Evaluator (Expr ->
              value), Planner (kind-aware access path)
hashindex/    ExtensibleHashIndex: build, lookup, insert (split +
              directory doubling), erase, verify
              HashPages: DirectoryPage/BucketPage/OverflowPage + the hash
              StaticHashIndex: the fixed-N baseline — h(key) mod N,
              overflow spill, no splits (check-suite exercised)
index/        IsamIndex: bulk build, lookup, range, insert, erase, verify
              KeyCodec + SeparatorPage/LeafPage/OverflowPage views
tools/        BulkLoader: bootstrap loads of empty heap files (raw block
              I/O, no buffer pool, engine-equivalent result)
sql/          the ex12 front-end + the index statements (USING HASH|ISAM)
schema/       ex11's schema layer + the catalog writer (DDL seam, kind)
heapfile/     ex11's heap: slotted pages, page chain, RowId{page, slot}
bufpool/      ex11's buffer manager: pin/unpin, LRU eviction, per-file close
file/         ex11's BlockFile
tuple/        ex11's logical row
```

## More

`EXPLAIN.md` explains every module, the ISAM and hash file formats page
by page, the access-path rules, and the design decisions.
