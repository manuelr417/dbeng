# EXPLAIN.md — how ex15 works, module by module

ex15 extends the ex14 stack — the ex11 storage engine (a schema-driven
heap over a buffer pool), the ex12 SQL front-end, the ISAM and hash
index modules, and the exec layer that binds, evaluates, plans, and
dispatches — with **grouped aggregation**: `GROUP BY`, `HAVING`, and
aggregate select items execute through a **static hash operator**
(lecture 11's fixed-N hashing, carried by an aggregation operator
instead of an index) with a **restart-based spill** for group counts
beyond the entry budget. The grammar stays permissive; the binder and
executor give the grouped surface its named rules. Plain C++20, zero
dependencies, single-threaded by contract.

Every layer reports failures the ex11 way: a `bool` return plus an
`error` string out-parameter, cleared on entry. Errors are named; the
CLI prints and continues; the check suite asserts. Pin windows stay
short, and the spill's temp I/O never touches the buffer pool.

## What was vendored

`tuple/`, `schema/`, `heapfile/`, `bufpool/`, `policy/`, and `file/`
are ex11's modules verbatim. `sql/`, `index/`, `hashindex/`, and
`tools/` are ex14's modules (the exercise-named artifacts renamed); the
grouping work touches `sql/` only by adding `HAVING` to the SELECT
grammar. `exec/` is ex14's module extended with the two new binding and
evaluation contexts and the grouped SELECT branch — the ungrouped
paths, the planner, and the access-path machinery are byte-for-byte
ex14's, with the scan spine factored into one shared helper both SELECT
paths call. The regression base is the vendored check suites
(`check_storage.cpp`, `check_sql.cpp`, `check_index.cpp`,
`check_hash.cpp`, `check_static.cpp`, `check_exec.cpp`,
`check_loader.cpp`), run by the `ex15` binary before the new group
suite (`check_group.cpp`).

## sql/ — the HAVING seam

The keyword table gains `HAVING` (sql/Token.h; the "34 keywords"
comment moves with it). `SelectStatement` gains a `having` expression
parsed between GROUP BY and ORDER BY — the grammar stays permissive
about clause order, as it always has been — and `expr_equal` compares
HAVING trees so statement equality stays structural. `AstPrinter`
prints the clause; `AstDraw` attaches it like the other expression
children. `DISTINCT` inside an aggregate call is refused at parse time
(`DISTINCT aggregates are not supported`): the surface has no NULLs and
no duplicate semantics to define, so the form is refused rather than
half-defined. The rest of the grammar — joins, ORDER BY, LIMIT,
UPDATE, DELETE — still parses: the *executor* refuses those by name,
so the subset contract lives in the dispatcher, not the grammar.

## exec/ — two binding contexts, two walkers

The binder (exec/Binder.h) now distinguishes **where an expression
sits**:

- **The row context** (`bind_expr`) — WHERE clauses, GROUP BY keys,
  index bounds, aggregate arguments. Columns are free; aggregate calls
  are refused by name (WHERE: `aggregates are not allowed in WHERE`).
- **The group context** (`bind_group_expr`) — grouped select items and
  HAVING. Bare columns must be GROUP BY columns
  (`column '...' must appear in GROUP BY or be inside an aggregate`);
  aggregate calls land in a **group inventory**: each distinct
  aggregate call is bound once into a `BoundAggregate{fn, arg, type}`
  (deduplicated by structural equality of function and argument tree)
  and the expression tree stores only the slot index. Nested aggregates
  are refused (`aggregate calls cannot nest`).

The context flag is one parameter on the recursive bind, so placement
rules are checked where they are true — a HAVING item's aggregate
subtrees are exempt from the column rule by construction, while its
column leaves are checked verbatim against GROUP BY. GROUP BY itself
binds in the row context and must name plain columns
(`GROUP BY must name plain columns`); the result is a
`GroupBinding{key_columns, aggregates}` — the shared inventory every
group-context tree points into.

Typing is static and lives at bind time: `COUNT` is Int, `AVG` is
Double, `SUM` takes its argument's type (numeric required), `MIN`/`MAX`
take their argument's type (numeric or string; a boolean argument is
refused). `SELECT *` under GROUP BY is refused with a named error
before any page is pinned.

The evaluator (exec/Evaluator.cpp) mirrors the two contexts with two
walkers over one shared operator application (`apply_binary`,
`apply_unary`): `evaluate` walks a row-context tree over a `Tuple`;
`evaluate_group` walks a group-context tree over one group's **results**
— a Column node reads the group's key value by the column's table
index, an Aggregate node reads the group's final value by inventory
slot. Both walkers agree on every type promotion, division, and
comparison rule because the operators are the same functions.

## group/ — the static hash operator

`GroupTable` (group/GroupBy.h) is the lecture's static hash scheme
applied to aggregation. Fixed N buckets (the executor injects 8 by
default); routing `h(key) mod N`; each bucket owns a chain (a vector)
of `GroupEntry{key values, states}`; lookup scans the chain comparing
key values exactly. **No splits, no resize, ever** — a skewed group
distribution just grows chains, the static scheme's cost story now
lived by an operator. The table sizes with the **group count**, not the
row count: GROUP BY on a near-unique column is the documented pressure
case, and an **entry budget** bounds it — the first fold that would
exceed the budget fails with `group table exceeded its entry budget`,
which is the executor's spill signal, not a user-facing error.

**The hash** follows the ex14 convention: a lone integer key hashes to
its own bit pattern (the slides' h(4) = 100 — routing for integer keys
is then just the low bits), everything else through FNV-1a 32-bit over
a typed byte image — a tag byte per value (`\0` int, `\1` double, `\2`
string), the value's bytes, and a `\3` terminator after strings so no
key is a prefix of another. Multi-column keys concatenate the per-value
images before hashing; `1` and "1" never collide.

**Fold and drain.** `fold(key, args)` finds or appends the entry and
updates one `AggregateState` per aggregate slot: `count` (COUNT, and
AVG's denominator), `int_sum`/`double_sum` (SUM/AVG over the argument's
type), and `extremum` with a `has_extremum` flag (MIN/MAX, set from the
first value). The `args` vector is aligned with the slots; a COUNT(*)
slot takes no argument. `take_all()` drains in bucket order, entries in
arrival order within a bucket — a deterministic order that is bucket
order, not sorted order. `final_value` produces each slot's answer:
COUNT the count, AVG the quotient (0.0 for an empty group), SUM the
accumulated sum, MIN/MAX the extremum or, for a group that never set
one, **the type's zero value** — this engine has no NULLs, so an empty
group answers an identity, not a null.

The module is pure structure: no heap access, no buffer pool, no
executor. That is what makes it unit-checkable at check-suite scale
(routing against hand-computed placements, chain growth under
collisions, typed final values) and reusable by both execution phases.

## group/ — the partition run format, page by page

A spill run (group/PartitionRun.h) stores **plain input tuples** — the
group-key values plus one value per aggregate slot that takes an
argument, pre-evaluated — under a synthesized **mini-schema**: columns
`c0..cN` typed from binding (key types first, then argument types in
slot order). String width is the widest string column of the source
table: the only way a spill record carries a string is a whole column
value (the surface has no string-producing expressions), so the bound
is exact.

The format is the BulkLoader's discipline verbatim, with one difference
in the header:

- **Block 0 — the identification page.** A `SlottedPage`-formatted
  4096-byte block whose record area carries the identification: magic
  `GRUN` at offset 0 (a heap file spells `TUP1`; a leaked run must not
  be mistakable for a table file), then the heap header's name-length
  and name shape at offsets 4 and 8. The page header is formatted with
  an **optimistic** `next = 1` link, patched back to `kNoPage` at close
  if no row ever arrives.
- **Blocks 1..M — the record pages.** Each is composed in a local
  buffer through the engine's own `SlottedPage` view — records
  serialized by the engine's own `TupleSerializer` for the mini-schema
  (fixed-width records: 4-byte integers, 8-byte doubles, strings padded
  to the declared width with NULs, padding verified on read) — and
  written **exactly once**, linked in generation order with optimistic
  `next` links (`this page + 1`), one **trailer patch** on the final
  page setting `next = kNoPage`. K = `slot_capacity(record_size)`
  records per page.
- **The page geometry** is the heap's: records grow from offset 0, the
  occupancy array grows down from 4084, slot count at 4084, prev/next
  page ids at 4088/4092.

Writes and reads are **pool-free** raw block I/O (`bufman::File`
through `file/BlockFile.h`): temp I/O must not evict session pages
from an 8-frame pool. The reader validates the magic and the
identification name, then walks the page chain sequentially with one
block buffer, deserializing through the same mini-schema.

**Engine equivalence** is the format's correctness claim: with the four
magic bytes patched to `TUP1`, a written run *is* a heap file for its
mini-schema — `HeapFile::open` accepts it and its iterator scans the
records back. The check suite does exactly that (the loader's
tool-vs-insert twin test, reapplied), which pins the layout against
drift far better than a byte-dump comparison would.

Run files live beside the data file:
`data/<table>.tbl.gspill<r>.tmp`, where `r = level · K + partition`
encodes both the recursion level and the partition. Each run is deleted
as soon as it is drained; the named error paths delete best-effort too.

## exec/ — the grouped SELECT path end-to-end

`handle_select` detects grouped-ness (a GROUP BY, a HAVING, or any
aggregate select item — the implicit single group) and routes to
`handle_grouped_select`, which runs five phases:

1. **Bind.** GROUP BY keys, then the select items and HAVING in group
   context (one shared inventory), then WHERE in row context. All
   binding errors happen before any page is pinned.
2. **Scan.** The shared scan spine (`collect_candidates`) — the same
   planner, the same oracle kind gates, the same access-path tolerance
   fallbacks as the ungrouped path — yields candidate row ids and the
   announced scan path.
3. **The record stream.** Each surviving row (fetched one at a time,
   residual-filtered) becomes one record: the group-key values, then
   one value per aggregate slot that takes an argument — evaluated
   **once**, here. This is phase one; nothing downstream evaluates an
   expression or touches a heap page.
4. **Aggregate** (`aggregate_records`). The common case: fold the
   records into one in-memory `GroupTable` and drain. On the budget
   error, **restart** (`spill_records`): the partial table dies with
   the frame; the record stream re-partitions into K run files, each
   record routed by the **level-shifted** hash — level L partitions by
   bits `[L·log₂K, (L+1)·log₂K)` of `h(key)`, because re-partitioning
   by the same low bits would send every key back where it came from.
   Each partition is then aggregated by **the same function,
   recursively** — memory first, spill deeper on overflow. Termination:
   distinct keys separate within `ceil(32 / log₂K)` levels (after that
   every pair differs in some bit some level slices on), so one
   duplicate-flooded group is the only way a partition stays over
   budget — and that one group cannot overflow. The depth cap is that
   bound itself, derived from K: a level whose slice would start at or
   past bit 32 (`L·log₂K ≥ 32`) is a named error,
   `grouped execution exceeded the spill depth cap` — so K = 2 gets its
   full 32 levels and no shift ever exceeds the hash width. Each run is
   consumed (read, deleted) before its recursion, so the file set stays
   small.
5. **Drain and project.** Every group — in-memory buckets in order, or
   partitions 0..K−1 each in bucket order — is filtered by HAVING
   (evaluated in group context over the group's final values) and
   projected into the output row. Empty input with GROUP BY answers
   zero rows; without GROUP BY, one identity row (which HAVING may
   filter away). The announcement composes the phases:
   `(via <scan> + hash group)`, or
   `(via <scan> + hash group (spilled, K partitions))`.

**Parameter injectability** (design D8): bucket count, partition
count, and entry budget are `Executor` setters with production
defaults (8, 8, 100000) — the oracle's invariance runs and the spill
tests inject small values; SQL never sees them. The partition count
must be a power of two (the bit-slicing requires it; anything else is a
named error).

## ex15_cli — the session CLI

Vendored from ex14 with one authored change: `help` prints the ex15
surface — the SQL summary now names GROUP BY, HAVING, and the
aggregates, and the refusal list is the ex15 list (joins, ORDER BY,
LIMIT, UPDATE, DELETE). Grouped SELECTs print the composed
announcement, then the rows, then the count; a refused statement prints
its named error and the prompt keeps working.

## main.cpp — the check suite

Eight suites in order: the vendored storage, sql, index, extensible
hash, static hash, exec, and loader suites, then the group suite
(`check_group.cpp`): the hash convention unit-checked (integer identity,
tag-byte separation, multi-column images); routing against
hand-computed placements with N = 8 (drain order proves the buckets);
chain growth under collisions; repeated-key merging; the budget as a
named spill signal; fold/drain typing across COUNT/SUM/AVG/MIN/MAX
including MIN/MAX on strings and the empty-group identities; parameter
invariance over bucket counts 1, 3, 8, 17 and budgets at the group
count; the run file round-trip and the HeapFile equivalence (the
four-byte patch); forced spills at 8 and 2 partitions including
multi-level recursion, with identical answers and no leftover files;
and the **differential oracle** — 300 seeded-random rows over 12 group
keys, three queries (plain, WHERE-filtered, HAVING-filtered) through
ten parameter combinations (N ∈ {1, 3, 5, 8, 23}, K ∈ {2, 4, 8},
budgets from large down to 1), each answer set required to equal the
brute-force no-hashing reference computed in the suite.

## Design decisions worth knowing

- **The spill is the same operator, not different code.** Restart-on-
  overflow re-enters `aggregate_records` per partition; the in-memory
  table and every run are aggregated by one function. There is no
  second aggregation path to drift.
- **Evaluate once, spill values.** Phase one (the record stream) is the
  only place an expression is evaluated or a heap page is touched;
  phase two (memory or runs) moves pre-evaluated values. The run format
  stores exactly those records — which is why it can be a heap file for
  a synthesized mini-schema.
- **Engine-equivalent runs, proven.** The GRUN magic keeps a leaked run
  from being mistakable for a table file, and the check suite's
  patch-and-open test proves everything else is the engine's own
  layout — `TupleSerializer` records in `SlottedPage` pages on the
  BulkLoader's write-once discipline.
- **Fixed hashing all the way down.** The group table inherits the
  static index's contract — no splits, no resize, chains grow — and the
  spill inherits the discipline's shape: a fixed fan-out of level-sliced
  hash bits instead of a growing directory. The teaching point is the
  scheme's cost story (skew grows chains), lived by an operator.
- **Placement rules live in the binder, the grammar stays permissive.**
  One context parameter on the recursive bind checks each rule where it
  is true: aggregates refused in the row context, non-key columns
  refused in the group context. Refusals name the rule
  (`must appear in GROUP BY or be inside an aggregate`), and all of
  them fire before any I/O.
- **Parameters are injectable, never surface.** Bucket count, fan-out,
  and budget are executor settings with production defaults, mirroring
  ex14's `set_allow_*` oracle gates: the suite proves invariance by
  varying them, and no SQL syntax can reach them.
- **Known hole, documented.** No WAL, as in every exercise so far: a
  crash mid-statement can leak `gspill` run files. They are
  self-identifying (`GRUN`, and the `.tmp` suffix), never opened as
  table files, and safe to delete; the transactions exercise will close
  the hole the same way it closes the index-dangling-entry hole.
- **The benchmark is vendored unchanged on purpose.** `parts_bench`
  measures access paths, which grouping does not touch; keeping it
  byte-identical is the regression proof that the grouped branch left
  the scan machinery alone.
