# EXPLAIN.md — how ex14 works, module by module

ex14 extends the ex13 stack — the ex11 storage engine (a schema-driven
heap over a buffer pool), the ex12 SQL front-end, the ISAM index, and
the exec layer that binds, evaluates, plans, and dispatches — with the
engine's second index structure: an **extensible hash index** (lecture
11) behind the `USING HASH | ISAM` clause, with entry erase on both
index kinds and a third announced access path. Beside it, as a
module-level addition exercised by its own check suite, sits the
lecture's baseline scheme: a **static hash index** with a fixed bucket
count. Plain C++20, zero dependencies, single-threaded by contract.

Every layer reports failures the ex11 way: a `bool` return plus an
`error` string out-parameter, cleared on entry. Errors are named; the
CLI prints and continues; the check suite asserts. Pin windows stay
short: descent pins one page, reads one child id, unpins.

## What was vendored

`file/`, `bufpool/`, `policy/`, `heapfile/`, `tuple/`, and `schema/` are
ex11's modules verbatim (one friend declaration added so the extended
catalog loader can still set declared widths). `sql/` is ex12's
front-end verbatim plus the index statements. `index/`, `exec/`, and
`tools/` are ex13's modules; the new work is `hashindex/` plus the
extensions noted below. The regression base is the vendored check
suites (`check_storage.cpp`, `check_sql.cpp`, `check_index.cpp`,
`check_exec.cpp`, `check_loader.cpp`), run by the `ex14` binary before
the new hash suite (`check_hash.cpp`).

## sql/ — the USING seam

The keyword table gains `INDEX`, `USING`, `HASH`, and `ISAM`
(sql/Token.h). `Ast.h` gains
`CreateIndexStatement{index_name, table, column, using_kind, has_using}`
— `using_kind` defaults to Hash, and `has_using` records whether the
statement spelled the clause out (the printer renders it only when
explicit) — and `DropIndexStatement{index_name}`. The parser's optional
`USING (HASH|ISAM)` after the closing paren is five lines of cursor
plumbing; anything else there is a parse error. `AstPrinter` renders
both statements for round-trips; `AstDraw` draws them, showing the kind
only when explicit. The rest of the grammar — joins, GROUP BY, ORDER
BY, LIMIT, aggregates, UPDATE, DELETE — still parses: the *executor*
refuses those by name, so the subset contract lives in the dispatcher,
not the grammar.

## schema/ — the catalog writer (the DDL seam)

ex11 could only read catalogs; ex13 added the write side; ex14 extends
both with the index kind:

- The catalog JSON's optional `"indexes"` array holds
  `{"name", "column", "kind"}` objects; `kind` is optional and absent
  means `"isam"` — ex13-era catalogs load unchanged. The loader refuses
  duplicate index names, empty names, unknown kinds (naming the bad
  value), and unknown keys, and stays all-or-nothing.
- `save_table_schema(path, schema, indexes, error)` validates first (no
  empty table name, no empty columns, no unnamed/duplicate indexes, no
  index naming an unknown column or carrying an unknown kind), then
  builds the text — always emitting `kind` — writes `<path>.tmp`, and
  renames over the target.

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
erase (`remove_entry`, ex14) removes in place with a tail memmove; the
vacated space is a gap the page may refill, never a compaction, and
`IsamIndex::erase(key, rid)` reaches exactly the pages the lookup walk
would scan (the landing leaf, its overflow chain, and any further leaf
the step-guard admits), failing with a named error when the pair is
absent. Frozen separators stay byte-identical across erases.

## hashindex/ — the extensible hash module

One file per (data file, column) of hash kind:
`<data file>.<column>.hdx`, 4096-byte blocks, native-endian u32s by
memcpy. The structure has three castes (lecture 11):

```
block 0          header: magic "HASH" (u32), key type (u32), key width
                 (u32), table name (len + bytes, 200 max), column name
                 (len + bytes), global depth (u32), first directory
                 block (u32), entry count (u64), last block (u32)
directory page:  count (u32), next directory block (u32), count x
                 bucket page id (u32)   — up to 1022 slots per page
bucket page:     count (u32), local depth (u32), first overflow (u32),
                 count x (key + rid)    — 340 entries for int keys
overflow page:   count (u32), next overflow (u32), count x entries
```

- **The hash.** `hash_key(codec, bytes)` maps an encoded key to its
  bit pattern: integer keys hash to their own value (the slides' `h(4)
  = 100`, `h(20) = 10100`), double and string keys through FNV-1a
  32-bit over the encoded bytes — deterministic, equality-faithful,
  and free of tuned parameters. The last `global depth` bits select
  the directory slot.
- **The directory** lives in a chain of `DirectoryPage`s starting at
  the header's first-directory block. It is read once at open into an
  in-memory vector and written through on every change — the lecture's
  "directory can be kept in the buffer pool", so a lookup pays one
  bucket pin, not a directory walk. Open validates that the chain
  holds exactly 2^global-depth slots.
- **Insert** routes through the directory. A roomy bucket appends; a
  bucket that owns an overflow chain spills into the chain (a full
  chain front-chains a new page, ISAM's spill rule); a full chainless
  bucket **splits**: its entries and the new one redistribute by bit
  `local depth` of their hashes, both local depths grow by one, and
  the directory **doubles** exactly when the bucket's local depth
  equals the global depth (each slot copied to its two image slots, so
  the global depth increments and the split's two slots are re-pointed;
  a split below the global depth re-points the affected slot pair
  only). A split that cannot separate — every entry including the new
  one agrees at the split bit, the duplicate-crowd case — is
  **abandoned** to a fresh overflow page, and that bucket never splits
  again: the price the duplicate flood pays, contained per bucket and
  visible to verify, exactly like ISAM's overflow chains.
- **Lookup** hashes, masks to the global depth, reads the slot, and
  scans the bucket and its whole overflow chain in arrival order —
  duplicates contiguous.
- **Erase** removes the one entry matching (key, rid) exactly, tail
  entries shifting down. No merge, no directory shrink, no depth
  change: an emptied bucket stays allocated, an emptied overflow page
  stays in its chain. Erasing an absent pair is a named error.
- **Verify** walks the directory, every distinct bucket (corresponding
  slots share one bucket), and every overflow chain, reporting named
  findings: dangling rids (caller callback), entries whose low
  local-depth bits disagree with the bucket's pattern (misplaced),
  routing slots at odds with their bucket's pattern, a bucket deeper
  than the global depth, and a walked total that disagrees with the
  header.

`KeyCodec` is the one key encoding both index kinds share, and a row
address is one `Rid` type — the executor's session indexes speak one
vocabulary. `HashEntryPage` is the unsorted entry machinery shared by
`BucketPage` and `OverflowPage` (arrival-order `append_entry`, tail-
shifting `remove_entry`, `get_entry` with the container-like
convention); all views follow the `attach`-validates-counts discipline.

## hashindex/ — the static hash module

The lecture's baseline scheme beside the extensible one:
`hashindex/StaticHashIndex` over `hashindex/StaticHashPages`, exercised
by its own check suite (`check_static.cpp`), not wired into the SQL
surface. One file of 4096-byte blocks, same conventions as the
extensible module; the block-0 record mirrors the extensible header
with the bucket count in place of the global depth:

```
block 0          header: magic "SHSH" (u32), key type (u32), key width
                 (u32), table name (len + bytes, 200 max), column name
                 (len + bytes), bucket count (u32), first directory
                 block (u32), entry count (u64), last block (u32)
directory page:  the extensible module's DirectoryPage reused as a
                 plain chained u32 array — exactly N slots, no depths
bucket page:     count (u32), first overflow (u32), count x (key + rid)
                 — 340 entries for int keys, no local-depth field
overflow page:   the extensible module's OverflowPage, reused verbatim
```

- **Routing is `h(key) mod N`.** The same `hash_key` as the extensible
  module (integer keys are their own bit pattern, double and string
  keys FNV-1a), taken modulo the fixed bucket count — the lecture's
  static scheme; for a power-of-two N it is the last-d-bits rule. No
  global depth, no local depth, no slot masking.
- **Create allocates the whole file**: the header, the directory chain
  holding exactly N slots, and N empty primary buckets — buckets exist
  from birth, empty ones included, so the bucket count is a fact of
  the file, not a growth state. `N` is a parameter of create/build and
  is refused when zero; open re-validates it and reloads the array.
- **Insert** routes through the array. A roomy bucket appends; a full
  bucket spills into its overflow chain — the first page with room,
  else a fresh page front-chained (the extensible spill rule, minus
  every split and doubling branch). Overflow is permanent: a bucket
  with a chain never takes an entry back into the bucket page. This is
  the lecture's cost story made structural: nothing redistributes, so
  a skewed insert pattern just grows chains, and the chains are
  visible to verify.
- **Lookup** hashes, mods, reads the slot, and scans the bucket and
  its whole overflow chain in arrival order — duplicates contiguous,
  exact-key filtered, so congruent keys sharing a bucket (1 and 9 mod
  8) still answer exactly their own rids.
- **Erase** removes the one entry matching (key, rid) exactly, tail
  entries shifting down. No merge, no deallocation, no relocation: an
  emptied bucket stays allocated, an emptied overflow page stays in
  its chain, and the vacated space refills on later appends. Erasing
  an absent pair is a named error.
- **Verify** walks the array slot by slot, every bucket, and every
  overflow chain, reporting named findings: dangling rids (caller
  callback), entries whose `h(key) mod N` disagrees with their owning
  slot (misplaced — overflow entries judged against the owning
  bucket), and a walked total that disagrees with the header. There is
  no depth rule to violate: the static file has none.
- **Build** is create + one insert per entry (hashing needs no sort),
  taking N as a parameter; an empty build is a valid empty index.

What the static module deliberately drops, relative to the extensible
one, is the entire growth machinery — splits, directory doubling,
local and global depth, the cannot-separate abandonment branch — and
what it keeps is the page discipline: the same `HashEntryPage` entry
array, the same `DirectoryPage` chain (as a plain array), the same
`OverflowPage` spill pages, the same header I/O shape, and the same
pin-window and error conventions. The distinct `"SHSH"` magic keeps
the two kinds from opening each other's files; each open names the
refusal.

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
- **Planner** (`Planner`): flattens top-level AND conjuncts (moving the
  bound nodes). An equality `column = literal` (or flipped) on a column
  with a **hash** index claims the hash scan — the first equality
  wins, later ones filter the claimed point's rows as residual.
  Otherwise `column op literal` on a column with an **ISAM** index
  yields bounds — equality a point, `< <= > >=` open/closed — and
  conjuncts on the same column intersect (an empty intersection is an
  index scan that answers nothing). A range never selects the hash
  path — hash answers equality only — so a range on a hash-only column
  is residual. With both kinds on the column, an equality prefers the
  hash and a range-only conjunct set takes the ISAM scan. Everything
  else — OR at the top level, `<>`, non-literal sides, unindexed
  columns — becomes residual, evaluated per fetched row. With no
  usable conjunct the plan is a heap scan. The planner is also the
  oracle's seam: the executor can hide all indexes (heap-only runs) or
  one kind at a time (the oracle's per-path runs).
- **Executor** (`Executor`): the dispatcher and the session state — the
  buffer manager reference, the catalog cache, the file table (a path
  maps to an open `HeapFile` plus its open indexes of both kinds).
  Handlers: CREATE/DROP TABLE (catalog writer + file lifecycle), 
  CREATE/DROP INDEX (catalog `indexes` array + build/erase, one index
  per column and kind, in-session index registration), INSERT (validate
  every row first — all-or-nothing per statement — then heap insert,
  then each index through one kind-dispatching seam), erase_row (read
  the row, erase the heap slot, then every index), SELECT (bind, plan,
  execute hash, index, or heap path, residual filter, project).
  Refusals for the wider language name their feature.

Physical naming: a table's data file is `data/<table>.tbl` (the
`data` directory is created on demand); indexes sit beside it as
`data/<table>.tbl.<column>.idx` (ISAM) or
`data/<table>.tbl.<column>.hdx` (hash). Opening a data file lazily
rebuilds any declared index file that is missing, with its catalog
kind. Files bind to the table they were first used with (ex11's rule).

## ex14_cli — the session CLI

One BufferManager (LRU, 8 frames) per session, shared by heap files and
index files. A line whose first word is an escape command (`scan`,
`read`, `seek`, `bulk`, `delrow`, `checkindex`, `quit`, `help`) runs
immediately; anything else accumulates until a line closes a `;`, then
executes (a pure-whitespace leftover does not count as an unfinished
statement). SELECTs print their `(via ...)` marker, then `name=value`
rows, then the count; DDL, INSERT, and delrow print one report line.
Errors print and the prompt returns; piped input suppresses the prompt;
`quit` closes every session file exactly once and the exit code is 1
only if a flush fails.

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

## parts_bench — the index-vs-heap benchmark

One executable, one run, one report. The argument surface is `--rows N`
(default 1,000,000, capped at 9,999,999 so the pname scheme always
fits) and `--reuse`; the two cannot combine. A default run: clean slate
(drop any leftover `parts` indexes and the table itself, tolerating
absence), CREATE TABLE through the executor, deterministic generation
(seed 42; sequential partid; seven colors; clamped-normal pprice on
[0.5, 3000] and pweight on [1, 50]; pname = "p" + a zero-padded row
number), bulk load through `tools::bulk_load`, then the measurement
protocol: Q1 (`partid = K`, K = 54645 when N >= 54645, otherwise N) and
Q2 (`pprice >= 1.0 AND pprice <= 10.0`) timed on the heap path — the
run fails if either announces anything else — then the index phase, one
kind at a time: a hash index on `partid` (build timed) and Q1 re-run on
the hash path (the run fails if it does not announce `hash scan`), an
ISAM index on `partid` (build timed) and Q1 re-run on the ISAM path
(the hash gate suppresses the preferred hash for this run), and an ISAM
index on `pprice` (build timed) with Q2 re-run on the ISAM path — a
range never takes the hash. All answer sets are compared (sorted, all
six columns) and must be identical; the report prints the query times
per path with speedups, all build times, the load time, and the
verdicts. The serialization round-trip self-check and the data
fingerprint (two runs with the same count must match) pin the
generator's contract.

A `--reuse` run re-measures on data that is already there: the indexes
are still dropped (a genuine heap-scan baseline needs that) but the
table is kept, N is the existing table's row count, and creation and
loading are skipped. The rows are still regenerated in memory with the
same seed, so the fingerprint, the Q2 expected count, and the spot
checks verify the reused file against the generator — a stale or
foreign file fails the run by name. Reuse without data (no catalog, no
file, or an empty table) fails before anything is measured, and the
report marks the provenance (`data: reused (N rows, load skipped)`) in
place of a load time.

## main.cpp — the check suite

Seven suites: the vendored ex11 storage suite (351, with the
catalog-kind checks), the vendored ex12 sql suite (291, including the
USING-clause checks), the ISAM-module suite (465 — codec, page views,
header identification, build, duplicates spanning leaves, ranges,
overflow chaining with byte-frozen separators, erase with byte-frozen
separators and persistence, verify with seeded corruption,
persistence), the hash-index-module suite (143 — the hash function
against the slides' worked examples, the three page views, lifecycle
and self-identification, the lecture's placements, split-at and
split-below the global depth, duplicate floods into overflow chains,
erase across bucket and overflow, verify with seeded corruption, build
and reopen), the static-hash-module suite (104 — lifecycle and
self-identification including the extensible-file refusal, modulo
routing with congruent-key collisions, duplicate floods into overflow
chains with the bucket count fixed, build against per-entry inserts,
erase across bucket and overflow, verify with seeded corruption),
the exec-layer suite (85 — bind-time refusals, evaluation
semantics, kind-aware access-path markers, planner routing rules, the
DDL seam, INSERT all-or-nothing, erase_row fan-out, index-file
rebuild, DROP TABLE completeness), and the bulk-loader suite (31 —
fill/link read-back, row-id parity, the tool-vs-insert differential
oracle, refusal rules, empty and zero-record loads, generator-failure
naming) — ending in the **differential oracle**: 500 random rows, four
indexes (both kinds on each of two columns), 300 random predicates,
each SELECT executed once per available path — both kinds, hash only,
ISAM only, heap — all four answers required to be the same set of rows.

## Design decisions worth knowing

- **Hash now, B+tree later, on purpose.** The ISAM module's frozen
  separators and overflow chains and the hash module's split/double
  directory are the two lecture 11 structures; the latched B+tree of
  the roadmap is student homework and stays out.
- **One mutation seam per layer.** Heap writes go through `HeapFile`,
  index writes through their index modules, DDL through the catalog
  writer. INSERT applies heap first, then every index; erase_row reads
  the row, erases the heap slot, then every index — one
  kind-dispatching seam serves both. The escape hatches share the same
  path through the Executor.
- **Overflow is permanent, on both structures.** ISAM leaves spill to
  chains forever; a hash bucket that owns an overflow chain never
  splits again. The alternative — redistributing chains during splits
  — is restructure the lecture does not define, and the duplicated
  keys it exists for would not redistribute anyway.
- **The catalog is still out-of-band.** Index files self-identify
  (magic + table + column + key type/width) like heap files do; the
  catalog says which indexes exist and what kind they are. A deleted
  index file is a rebuild, not an error.
- **Bind-time safety.** Names resolve and types check before any page
  is pinned; a refused statement costs no I/O.
- **Correctness never depends on the index.** The planner can suppress
  all indexes or one kind at a time; the oracle holds every path to
  the same answer set. Without ORDER BY, row order is unspecified and
  may differ between paths.
- **Known hole, documented.** No WAL: a crash between the heap write
  and the index write (insert or erase) can leave a dangling entry;
  `checkindex` exists to catch it, and the transactions exercise will
  close it.
