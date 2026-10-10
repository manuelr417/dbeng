# roadmap.md — where threads and latches land, and what waiting costs

The plan after ex12: (a) selection/insert/update/delete execution, (b)
indexing, (c) joins, (d) transactions. The worry: if threads and latches
wait too long, retrofitting them becomes a major rewrite. This document
answers "when", and separates the part of concurrency that is safe to
defer from the part that is not.

## The short answer

- **Latches arrive with the index (b)** — designed into the B+tree from
  its first line, even though the exercise still runs one command at a
  time. The first thread tests land in the same exercise.
- **Threads arrive with transactions (d)** — as sessions, running on a
  buffer manager that became thread-safe back in (b).
- **What starts now, in (a), is not threads — it is discipline.** Four
  rules, listed below. Discipline is what makes waiting cheap; naive
  structures are what make it expensive. Threads themselves are almost
  never the rewrite.

## The rewrite-risk audit

Which components actually hurt when concurrency lands last?

| Component | Retrofit cost if deferred to (d) | Why |
|---|---|---|
| BufferManager | **Small** | `pin`/`unpin` API unchanged; add a latch on the page table and one per frame. One file pair, no caller changes. |
| HeapFile / SlottedPage | **Small** | Slot allocation becomes "pin + page latch + mutate + unpin" around existing page code. The page stays a plain view. |
| Executor + joins | **~None** if written as scan operators | Pure functions over iterators parallelize for free later; nothing to rewrite. |
| B+tree | **Large if built naive** | Crabbing / optimistic latch coupling needs a latch/version word in every node and restart logic woven through traversal. This is the real rewrite risk of the whole roadmap. |
| WAL | **Medium if retrofitted** | Every record mutation needs a logging hook. Free if all mutations already flow through one seam. |
| Session / CLI | **Small** | The REPL loop becomes thread-per-session; ex11 already kept session state as one cohesive unit for exactly this day. |

Conclusion: **waiting on threads is safe; a latch-naive B+tree and a
log-naive write path are not.** Those two get their concurrency-shape
early. Everything else is a contained upgrade.

## Phase by phase

### ex13 — (a) execution: single-threaded, but obey the discipline

- Executor = binder + scan operators: pure functions over
  `HeapFileIterator`, no hidden global state. This is what makes (c) and
  any later parallelism free.
- UPDATE/DELETE stay **collect-then-apply** (gather matching `RowId`s,
  then mutate) — never mutate while an iterator is alive. This is not
  just iterator safety; it is the deadlock-free update pattern that (d)
  keeps.
- **One mutation seam**: record changes flow only through
  `HeapFile::insert/update/erase`; DDL flows only through the catalog
  writer. No code needed now — it is a rule. In (d), WAL hooks the seam
  and the executor is never touched again.
- No threads here. A parallel executor before an index has nothing
  interesting to protect and all of the debugging cost.

### ex14 — (b) indexing: first latches, still one command at a time

- The B+tree node carries a **latch/version word from day one**. Search
  descends with read latches (or optimistic version validation), writes
  use the crabbing discipline — latch child, release parent when the
  parent is safe (not full for insert, more than half full for delete) —
  with the optimistic fast path: most inserts/deletes touch only the
  leaf; if the leaf would split or merge, restart the descent
  pessimistically.
- Built and tested **single-threaded first** — the latch code paths are
  the same code, uncontended and easy to assert on. The exercise's last
  section adds the first real thread tests: N threads inserting distinct
  keys, concurrent readers during writes, all validating the tree.
- The **buffer manager becomes thread-safe here** (page-table latch +
  per-frame latch). Its API does not change, so ex08–ex13 code is
  untouched.
- Why here and not earlier: crabbing is an *index* algorithm — with no
  index there is nothing latch-shaped to learn on. Why not later:
  retrofitting version words and restart logic into a finished tree is
  precisely the major rewrite this roadmap avoids.

### ex15 — (c) joins: single-threaded, parallelism is a bonus

- Nested-loop and hash joins are just more scan operators. No new
  concurrency obligations.
- Optional bonus: parallel outer scan (threads over row-id ranges).
  Cheap only because the executor is pure — and never load-bearing for
  correctness.

### ex16 — (d) transactions: two halves, threads in the second

- **ex16a — WAL & recovery (single-threaded).** Log at the mutation seam
  from ex13; redo/undo (ARIES-lite), checkpointing. Crash recovery is
  testable by kill-and-replay with zero threads — and it is the bigger
  learning arc of transactions anyway.
- **ex16b — sessions & isolation (threads arrive).** Thread per
  session; **one shared buffer manager** (thread-safe since ex14) —
  two managers caching one file is corruption, so the session's
  `BufferManager` moves to shared; the open-file table is shared and
  latched; DDL serializes on a catalog latch. On top: a lock manager
  (table/page locks — logical *locks*, distinct from latches),
  isolation levels, deadlock detection (the one-directional ordering
  below is why cycles cannot arise in the latch layer). MVCC is the
  optional stretch.
- Why threads wait until here: before isolation exists, concurrency
  protects nothing the course can observe or grade. The session is also
  the natural thread unit, and its state has been one cohesive class
  since ex11.

## The four rules to obey from day one

1. **Short critical sections.** Pin → operate → unpin; never hold a pin
   (or page latch) across user-visible work or I/O you do not need.
2. **One mutation seam.** Record mutations only through
   `HeapFile::insert/update/erase`; DDL only through the catalog writer.
   Future WAL and locking hook exactly these.
3. **Collect-then-apply.** Never mutate while iterating a structure.
4. **One-directional acquisition.** Latches and locks are always taken
   top-down / left-to-right (crabbing, scan coupling) — deadlock-
   freedom by construction, and the invariant (d)'s lock manager keeps.

## What not to do

- Do not add threads in ex13 "to parallelize the executor" — nothing to
  protect yet, maximum debugging pain, and (c) does not exist to
  benefit.
- Do not build the B+tree naive planning to "add latches in (d)" — that
  is the one genuine rewrite on this roadmap.
- Do not push latches down into `SlottedPage` internals — latches live
  at the frame boundary (buffer manager / heap-file discipline); pages
  stay plain byte views.
- Do not conflate latches and locks when (d) arrives: latch = physical,
  short-lived, on pages/frames; lock = logical, transaction-lived, on
  tuples/tables. (d) introduces the second kind; the first has existed
  since ex14.

## Precedent

This ordering is the classic course arc, not a compromise: buffer-pool
latching → index concurrency (crabbing/OLC) → query execution →
concurrency control and recovery — the same sequence the standard
database courses teach, mapped onto this repo's exercises.
