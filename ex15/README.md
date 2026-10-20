# ex15 — grouping: GROUP BY and HAVING over a static hash operator

ex15 adds **grouped aggregation** to the ex14 stack: `GROUP BY`, `HAVING`,
and aggregate select items (`COUNT(*)`, `COUNT(c)`, `SUM`, `AVG`, `MIN`,
`MAX`) execute through a **static hash operator** — lecture 11's fixed-N
hashing discipline carried by an aggregation operator instead of an
index. Fixed N buckets, `h(key) mod N` routing, per-bucket chains, no
splits, no resize, ever. When the group table overflows its entry
budget, the operator **restarts**: the record stream re-partitions into
K raw-block run files by level-shifted hash bits and each run is
aggregated by the same operator recursively. Everything else — the
storage modules, the SQL front-end, the index modules, the bulk loader,
the CLI, and the benchmark — is vendored from ex14 unchanged; the new
work is the `group/` module, the grouped binder/evaluator contexts, the
grouped executor branch, and the group check suite with a differential
oracle. Plain C++20, no external dependencies, single-threaded by
contract.

```sql
CREATE TABLE person (pid INTEGER, name VARCHAR(10), age INT, city VARCHAR(3));
INSERT INTO person VALUES (1, 'Ruth', 42, 'PR'), (2, 'Abe', 35, 'PR'),
                          (3, 'Mia', 28, 'MA');
SELECT city, COUNT(*), AVG(age) FROM person GROUP BY city;
-- (via heap scan + hash group)
SELECT city, MAX(name) FROM person GROUP BY city HAVING COUNT(*) > 1;
SELECT COUNT(*), SUM(age) FROM person;            -- the implicit single group
SELECT city FROM person GROUP BY city HAVING MIN(age) >= 30;
```

## Project layout

```text
tablecatalog/   table catalogs (JSON), written by the engine
                (vendored ex14, plus suite fixtures person/car)
tuple/          Column, Tuple — the logical row            (vendored ex11)
schema/         TupleSerializer, TableSchema, catalog loader
                + catalog WRITER (the DDL seam)            (vendored ex11)
heapfile/       SlottedPage, HeapFile                      (vendored ex11)
bufpool/        BufferManager, BufferPool, DataFrame       (vendored ex11)
policy/         ReplacementPolicy + LRUPolicy              (vendored ex11)
file/           BlockFile — raw 4096-byte block I/O        (vendored ex11)
sql/            Lexer, Parser, AST, printer, drawing       (vendored ex14,
                + HAVING in the SELECT grammar)
index/          KeyCodec, the three page views, IsamIndex  (vendored ex14)
hashindex/      the two hash index kinds (extensible,      (vendored ex14)
                static) — unchanged
exec/           Binder, Evaluator, Planner, Executor       (vendored ex14,
                + the aggregate/group binding and evaluation
                contexts, the grouped SELECT branch, the
                restart spill)
group/          NEW: the static hash group operator —
                GroupBy (key hashing, the fixed-N table,
                fold, drain, final values) and PartitionRun
                (the spill run writer/reader)
tools/          BulkLoader — the bootstrap loader for empty
                heap files                                 (vendored ex14)
ex15_cli.cpp    the session CLI (help names the grouped surface)
parts_bench.cpp the benchmark over generated parts rows    (vendored ex14)
main.cpp        the check suite (the ex15 binary)
EXPLAIN.md      the design and implementation, module by module
```

## Building

Requires CMake 4.3+ and a C++20 compiler.

```sh
cmake -S . -B build
cmake --build build
```

Binaries: `build/ex15` (checks), `build/ex15_cli` (session), and
`build/parts_bench` (benchmark).

## Running the check suite

```sh
build/ex15
```

One `PASS:`/`FAIL:` line per check, eight suites in order: the vendored
ex11 storage suite, the vendored ex12 sql suite, the ISAM-module suite,
the extensible-hash-module suite, the static-hash-module suite, the
exec-layer suite, and the bulk-loader suite — all vendored — then the
**group module suite**: hash routing against hand-computed placements,
fold/drain typing, parameter invariance, engine-equivalent run files,
forced spills (including multi-level recursion), and the **differential
oracle** — 300 random rows, one grouping query through the in-memory
path, forced-spill paths at several depths, and a brute-force
no-hashing reference, all answer sets required to be identical across
varied N, K, and budgets. Exit code 0 when everything passes.

## Running the session CLI

```sh
build/ex15_cli
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
| `CREATE INDEX name ON t (col) [USING HASH \| ISAM \| STATIC HASH]` | one index per column and kind; the kind defaults to HASH. Builds over the table's current rows and records itself (with its kind) in the catalog. |
| `DROP INDEX name` | erases the index file and the catalog entry. |
| `INSERT INTO t VALUES (...), ...` | all-or-nothing per statement; the heap row lands first, then every index of the file. |
| `SELECT items FROM t WHERE expr GROUP BY cols HAVING expr` | every clause optional; grouped-ness starts with a `GROUP BY`, a `HAVING`, or an aggregate item (the implicit single group). |

Refused with a named error (the grammar still accepts them): joins,
`ORDER BY`, `LIMIT`, `UPDATE`, `DELETE` — UPDATE and DELETE are
deliberately left as student project work.

### Grouping rules

- `GROUP BY` names **plain columns** of the table; the select items and
  the HAVING predicate may use group columns, aggregate calls, and
  expressions over both. Anything else is a bind-time error:
  `column '...' must appear in GROUP BY or be inside an aggregate`.
- `SELECT *` under GROUP BY is refused (`name the grouped columns`).
- Aggregates are refused in WHERE (`aggregates are not allowed in
  WHERE`) and cannot nest (`aggregate calls cannot nest`); DISTINCT
  aggregates are refused at parse time.
- Typing: `COUNT` is Int, `AVG` is Double, `SUM` takes its argument's
  type (numeric required), `MIN`/`MAX` take their argument's type
  (numeric or string). This engine has no NULLs.
- WHERE filters rows **before** grouping; HAVING filters finished
  groups **after** aggregation.
- Empty input: with GROUP BY the answer is zero rows; without GROUP BY
  the implicit single group answers **one identity row** (`COUNT(*)` is
  0, `SUM` is 0, `AVG` is 0.0, `MIN`/`MAX` are their type's zero value)
  — which HAVING may then filter away.

### Access paths

Every SELECT announces its path, and grouped SELECTs compose the
grouping with it:

- `(via hash scan)`, `(via static scan)`, `(via index scan)`,
  `(via heap scan)` — exactly ex14's rules (equality on a hash or
  static-hash index column, equality or range on an ISAM column,
  heap otherwise); the choice is unaffected by grouping.
- `(via <scan> + hash group)` — the grouped answer, folded in memory.
- `(via <scan> + hash group (spilled, K partitions))` — the grouped
  answer that restarted into K partition runs.

The choice never changes the answer: the check suite holds the
in-memory path, the spilled path at several depths, and a brute-force
no-hashing reference to the same answer set (as a set — row order is
unspecified and follows bucket or partition order).

### Group operator parameters

The operator's settings are **executor-injectable, never SQL surface**
(like ex14's oracle gates): bucket count (default 8, the static index's
SQL default), partition fan-out K (default 8, must be a power of two),
and the group table's entry budget (default large, 100000). The check
suite injects small values to force spills and to prove parameter
invariance; a real session always runs the defaults.

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
| `bulk` | `bulk <table> <count> <file>` — generated rows, indexes maintained |
| `delrow` | `delrow <table> <file> <page> <slot>` — erases one record |
| `checkindex` | `checkindex <table> <file>` — verifies every declared index |
| `help` | prints the SQL and escape-hatch summary |
| `quit` | flushes and closes the session |

A `<file>` argument with no `/` resolves inside `data/` and gains the
`.tbl` extension unless it already has it; a path containing a `/` is
used exactly as given.

### Where files live

A SQL table's data file is `data/<table>.tbl` (the `data` directory is
created on demand); its index files sit beside it —
`data/<table>.tbl.<column>.idx` for ISAM,
`data/<table>.tbl.<column>.hdx` for hash, and
`data/<table>.tbl.<column>.shdx` for static hash. A spilled grouping
writes temporary run files `data/<table>.tbl.gspill<r>.tmp` beside the
data file and deletes each one as soon as it is drained (see the
documented known hole in EXPLAIN.md). Opening a data file builds any
catalog-declared index file that is missing (lazy rebuild). The escape
hatches keep ex11's arbitrary-path behavior.

## Running the parts benchmark

```sh
build/parts_bench                 # default: 1,000,000 rows
build/parts_bench --rows 100000   # smaller run
build/parts_bench --reuse         # re-measure on the existing data
```

Vendored from ex14 unchanged: the same generated `parts` rows, the same
Q1 equality and Q2 range queries, the same heap vs hash vs ISAM
comparison and identical-answers verdict.

## Architecture overview

```text
ex15_cli      SQL REPL + escape hatches (help names the grouped surface)
parts_bench   benchmark: heap vs hash vs ISAM (--rows N, --reuse)
exec/         Executor (dispatch, session state, the grouped branch,
              the restart spill), Binder (names -> positions, static
              types, the aggregate inventory), Evaluator (row context
              and group context, shared operator application),
              Planner (kind-aware access path)
group/        GroupBy: the ex14 hash convention, the fixed-N table
              (fold, drain, final values), the entry budget
              PartitionRun: the spill run writer/reader (raw blocks,
              pool-free, engine-equivalent)
hashindex/    ExtensibleHashIndex and StaticHashIndex (vendored ex14)
index/        IsamIndex: bulk build, lookup, range, insert, erase, verify
tools/        BulkLoader: bootstrap loads of empty heap files
sql/          the ex12 front-end + the index statements + HAVING
schema/       ex11's schema layer + the catalog writer (DDL seam, kind)
heapfile/     ex11's heap: slotted pages, page chain, RowId{page, slot}
bufpool/      ex11's buffer manager: pin/unpin, LRU eviction, per-file close
file/         ex11's BlockFile
tuple/        ex11's logical row
```

## More

`EXPLAIN.md` explains every module, the group table and run file
formats, the grouping rules and access-path composition, the restart
spill, and the design decisions.
