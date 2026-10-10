# EXPLAIN.md — how ex13 works, module by module

ex13 connects the two halves built so far: the ex11 storage engine (a
schema-driven heap over a buffer pool) and the ex12 SQL front-end (a
lexer, parser, and AST that knew nothing of engines). Between them sits
the new work — an ISAM index on one column and an exec layer that binds,
evaluates, plans, and dispatches — behind a session CLI that speaks SQL.
Plain C++20, zero dependencies, single-threaded by contract.

Every layer reports failures the ex11 way: a `bool` return plus an
`error` string out-parameter, cleared on entry. Errors are named; the
CLI prints and continues; the check suite asserts. Pin windows stay
short: descent pins one page, reads one child id, unpins.

## What was vendored

`file/`, `bufpool/`, `policy/`, `heapfile/`, `tuple/`, and `schema/` are
ex11's modules verbatim (one friend declaration added so the extended
catalog loader can still set declared widths). `sql/` is ex12's
front-end verbatim plus the two index statements. The regression base is
their check suites, ported into `check_storage.cpp` and
`check_sql.cpp`, run by the `ex13` binary before the new suites.

## sql/ — two new statements

The keyword table gains `INDEX` (sql/Token.h). `Ast.h` gains
`CreateIndexStatement{index_name, table, column}` and
`DropIndexStatement{index_name}`, their equality, and their
`Statement::Kind` values. The parser's `CREATE`/`DROP` dispatch peeks
the second token to choose tables from indexes; each new rule is five
lines of cursor plumbing. `AstPrinter` renders both for round-trips;
`AstDraw` draws them. The rest of the grammar — joins, GROUP BY, ORDER
BY, LIMIT, aggregates, UPDATE, DELETE — still parses: the *executor*
refuses those by name, so the subset contract lives in the dispatcher,
not the grammar.

## schema/ — the catalog writer (the DDL seam)

ex11 could only read catalogs. ex13 adds the write side:

- The catalog JSON gains an optional `"indexes"` array of
  `{"name", "column"}` objects. The loader accepts its absence (older
  catalogs load unchanged), refuses duplicate index names, empty names,
  and unknown keys, and stays all-or-nothing.
- `save_table_schema(path, schema, indexes, error)` validates first (no
  empty table name, no empty columns, no unnamed/duplicate indexes, no
  index naming an unknown column), then builds the text, writes
  `<path>.tmp`, and renames over the target — a failed save never
  leaves a half-written catalog.

CREATE TABLE composes the two: it builds the catalog text from the SQL
column defs, parses it through the loader (the sanctioned writer of
declared widths), and installs it through the writer.

## index/ — the ISAM module

One file per (data file, column): `<data file>.<column>.idx`, 4096-byte
blocks, native-endian u32s by memcpy. The structure has two castes:

- **Index pages** — separator pages, built bottom-up during bulk load
  from the leaves' first keys, and **frozen from then on**. No insert
  ever touches one; the check suite proves it byte-for-byte.
- **Data pages** — leaf pages holding sorted `(key, rid)` entries,
  chained left to right for range scans, plus per-leaf overflow chains
  that absorb post-build inserts. Leaves never split; a full leaf
  chains a new sorted overflow page at the front of its chain.

### Block layout

```
block 0  header: magic "ISAM" (u32), key type (u32), key width (u32),
         table name (len + bytes, 200 max), column name (len + bytes),
         root block (u32), first leaf (u32), level count (u32),
         entry count (u64), last block (u32)
separator page: count (u32), count x (key + child u32), first child (u32)
leaf page:      count (u32), next leaf (u32), first overflow (u32),
                count x (key + rid {u32 page, u32 slot})
overflow page:  count (u32), next overflow (u32), count x entries
```

Capacities: separator `(4096-8)/(keyw+4)` (511 for int keys), leaf
`(4096-12)/(keyw+8)` (340), overflow `(4096-8)/(keyw+8)` (340).

Keys are fixed width — 4 bytes per Integer, 8 per Double, the declared
width per String, NUL-padded like record bytes — so byte comparison IS
value comparison for strings, while ints and doubles decode before
comparing (numeric order, negatives included).

### The descent rule

`sep[i]` is the **first key of the child it routes to**; a separator
page stores `(sep[i], child[i+1])` pairs plus its **first child**
(leftmost). Descent takes the child after the **last separator strictly
less than the key** — the leftmost leaf that may hold it. Strictness
matters: equal keys can lag one leaf behind their separator (duplicates
spanning a leaf boundary), so a `<=` rule would overshoot and miss the
leftmost duplicates. Walkers then move forward: lookup continues into
the next leaf while its first key is `<=` the target; ranges walk the
chain until past the upper bound.

### Build, insert, verify

- **Build**: one heap scan collects `(key, rid)` pairs, an in-memory
  sort orders them, leaves fill left to right linking the chain, and
  separator levels build bottom-up — each level made of the first key
  of every page below (except the leftmost) — until one page holds a
  level: the root. An empty build is a valid empty index; the first
  insert grows its first leaf, which is then also the root (level
  count 1).
- **Insert**: descend the frozen separators to the owning leaf; insert
  sorted in place; on a full leaf, spill into the leaf's overflow
  chain (first page with room, else a new page chained at the front).
  Pages are allocated densely (block ids from the header's last-block
  field, never from the on-disk block count — dirty pages lag the
  disk). The header's entry count updates with every insert.
- **Verify**: walks the leaf chain and every overflow page, checking
  per-page sortedness, leaf-chain key order, every rid against a
  caller-supplied occupancy callback (dangling entries are named by key
  and row id), and the walked total against the header. Overflow keys
  are deliberately NOT ordered against their leaf — an insert into a
  full leaf spills regardless of value, so 6 can legally sit behind a
  leaf ending in 9.

### The page views

`KeyCodec` is the one key encoding (encode/decode/compare/display).
The two entry types carry the lecture's vocabulary: a `DataEntry` is
one **data entry** — a (search key, rid) pair living at the leaf level
— and `IndexEntry` is one **index entry** — a (search key, child page)
pair packed into a `SeparatorPage` (the deck's `P0 | K1 | P1 | …` index
page form). Both hold encoded key bytes (exactly `codec.width()` long);
pages hold and compare bytes without ever decoding.
`EntryPage` is the sorted-array machinery shared by `LeafPage` and
`OverflowPage` (binary-search `lower_bound`, sorted `insert_entry` with
a tail memmove, and `get_entry` returning a `DataEntry`, throwing
`std::out_of_range` past the end — the container-like convention).
`SeparatorPage` adds the routing rule (`descend_step`) and the named
index-entry API (`entry(i)`, `append(IndexEntry)`). All are
non-owning views bound to a pinned frame's buffer, `attach` validating
stored counts against capacity — the `SlottedPage` discipline. Entry
erase is deliberately absent (ex13 has no SQL caller); when a later
exercise adds it, it leaves gaps, never compacts.

## exec/ — bind, evaluate, plan, dispatch

- **Binder** (`Binder`): resolves table names against the catalog and
  column names to tuple positions, and infers a static type
  (int/double/string/bool) for every expression node. Type errors are
  named bind errors before any page is pinned: unknown columns,
  string-vs-number comparison, arithmetic on strings, MOD on doubles,
  non-boolean AND/OR/NOT. Aggregates are refused here too.
- **Evaluator** (`Evaluator`): a bound tree over one row yields a
  `Value` (`variant<int, double, string>`; booleans are ints 0/1).
  Mixed int/double promotes; int/int `/` truncates toward zero;
  division and modulo by zero are named runtime errors; AND/OR
  short-circuit.
- **Planner** (`Planner`): flattens top-level AND conjuncts (moving the
  bound nodes). `column op literal` (or flipped) on an indexed column
  yields bounds — equality a point, `< <= > >=` open/closed — and
  conjuncts on the same column intersect (an empty intersection is an
  index scan that answers nothing). Everything else — OR at the top
  level, `<>`, non-literal sides, unindexed columns — becomes residual,
  evaluated per fetched row. With no usable conjunct the plan is a heap
  scan. The planner is also the oracle's seam: in suppressed mode it
  sees no indexes, so every conjunct stays residual and the heap path
  runs.
- **Executor** (`Executor`): the dispatcher and the session state — the
  buffer manager reference, the catalog cache, the file table (a path
  maps to an open `HeapFile` plus its open `IsamIndex`es). Handlers:
  CREATE/DROP TABLE (catalog writer + file lifecycle), CREATE/DROP
  INDEX (catalog `indexes` array + build/erase, one index per column,
  in-session index registration), INSERT (validate every row first —
  all-or-nothing per statement — then heap insert, then each index),
  SELECT (bind, plan, execute index or heap path, residual filter,
  project). Refusals for the wider language name their feature.

Physical naming: a table's data file is `data/<table>.tbl` (the
`data` directory is created on demand); indexes sit beside it as
`data/<table>.tbl.<column>.idx`. Opening
a data file lazily rebuilds any declared index file that is missing.
Files bind to the table they were first used with (ex11's rule).

## ex13_cli — the session CLI

One BufferManager (LRU, 8 frames) per session, shared by heap files and
index files. A line whose first word is an escape command (`scan`,
`read`, `seek`, `bulk`, `checkindex`, `quit`, `help`) runs immediately;
anything else accumulates until a line closes a `;`, then executes (a
pure-whitespace leftover does not count as an unfinished statement).
SELECTs print their `(via ...)` marker, then `name=value` rows, then the
count; DDL and INSERT print one report line. Errors print and the
prompt returns; piped input suppresses the prompt; `quit` closes every
session file exactly once and the exit code is 1 only if a flush fails.

## tools/ — the bulk loader

A bootstrap tool for empty tables, deliberately outside the `HeapFile`
class and deliberately pool-free: `tools::bulk_load(path, schema, count,
generator, ...)` performs its own raw 4096-byte block I/O through
`BlockFile` — no `BufferManager`, so the caller must run it before any
buffer manager opens the path (the benchmark loads right after CREATE
TABLE, before the session ever opens the file). It composes each slotted
page in a local buffer through the engine's own `SlottedPage` layout
view — K = `slot_capacity(record_size)` serialized records per page —
and writes each block exactly once, linking pages in generation order
with optimistic `next` links and one trailer patch on the final page.
Row ids land in the same (page, slot) pattern sequential inserts
produce, so the result is engine-equivalent: `HeapFile::open`/`scan`/
`find` treat it as their own, and the check suite's differential oracle
(tool-loaded vs per-row-inserted twins, identical records and row ids)
keeps that honest against format drift. Missing file: created and
formatted; one-block empty file: verified and reused; anything else
(non-empty, foreign name, corrupt header, no record layout, oversized
records): refused before any write. Where the per-row insert path costs
228.8 s per 100k rows (its chain walk is quadratic), the bulk load takes
0.2 s — the load this tool exists for.

## main.cpp — the check suite

Five suites, 1155 checks: the vendored ex11 storage suite (344), the
vendored ex12 sql suite (278, including the new index-statement checks),
the index-module suite (codec, page views, header identification, build,
duplicates spanning leaves, ranges, overflow chaining with byte-frozen
separators, verify with seeded corruption, persistence), the exec-layer
suite (bind-time refusals, evaluation semantics, access-path markers,
the DDL seam, INSERT all-or-nothing, index-file rebuild, DROP TABLE
completeness), and the bulk-loader suite (fill/link read-back,
row-id parity, the tool-vs-insert differential oracle, refusal rules,
empty and zero-record loads, generator-failure naming) — ending in the
**differential oracle**: 500 random rows, two indexes, 300 random
predicates, each SELECT executed with the index allowed and suppressed,
both answers required to be the same set of rows.

## Design decisions worth knowing

- **ISAM before B+tree, on purpose.** The frozen separators and
  overflow chains are the contrast the later latched B+tree exercise
  will eliminate; the cost shows up in data (spilled keys) before it
  shows up in structure.
- **One mutation seam per layer.** Heap writes go through `HeapFile`,
  index writes through `IsamIndex`, DDL through the catalog writer.
  INSERT applies heap first, then index; the escape hatches share the
  same path through the Executor.
- **The catalog is still out-of-band.** Index files self-identify
  (magic + table + column + key type/width) like heap files do; the
  catalog says which indexes exist. A deleted index file is a rebuild,
  not an error.
- **Bind-time safety.** Names resolve and types check before any page
  is pinned; a refused statement costs no I/O.
- **Correctness never depends on the index.** The planner can suppress
  the index entirely; the oracle holds both paths to the same answer
  set. Without ORDER BY, row order is unspecified and may differ
  between paths.
- **Known hole, documented.** No WAL: a crash between the heap write
  and the index write can leave a dangling entry; `checkindex` exists
  to catch it, and the transactions exercise will close it.
