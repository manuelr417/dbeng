# Repository and Roadmap Assessment

## Current state

The repository is a deliberately layered, course-style database engine. Each
exercise is self-contained, and the later exercises form this stack:

```text
Lexer + Parser + AST (ex12)
Schema/catalog + generic tuples (ex11)
Heap file + slotted pages (ex10)
Buffer manager + LRU replacement (ex09)
Buffer pool (ex08)
Block-file storage and record serialization (ex07)
```

The codebase has reached a strong boundary for the next stage:

- `ex10` supplies stable fixed-width heap files, page-chain iteration, and
  `insert`, `find`, `update`, and `erase` by `RowId`.
- `ex11` generalizes storage through JSON table schemas, fixed-width record
  codecs, generic tuples, and a single-manager session CLI.
- `ex12` supplies a standalone SQL lexer, recursive-descent parser, AST,
  printer, and tree-drawing demo. It deliberately has no storage dependency.

All exercises from ex07 through ex12 configure and build under CMake 4.3.3.
Their documented test suites pass when executed from their respective exercise
directories.

## Roadmap assessment

`roadmap.md` proposes this sequence after ex12:

1. ex13: single-threaded execution.
2. ex14: indexing and the first latches.
3. ex15: joins.
4. ex16: recovery, sessions, and transaction isolation.

This is a sound ordering. It correctly identifies the B+tree as the component
whose concurrency design cannot safely be postponed: latch/version state and
restart-aware traversal need to exist from the index's first implementation.
By contrast, the existing buffer-manager and heap-file APIs are reasonably
shaped for a later, localized thread-safety upgrade.

The four stated rules are particularly appropriate for the current APIs:

- Keep pin/latch lifetimes short.
- Route mutations through one seam: `HeapFile::insert`, `update`, and
  `erase`, with DDL going through one catalog writer.
- Collect matching `RowId`s before UPDATE or DELETE applies changes.
- Acquire physical latches and later logical locks in a single direction.

They preserve the current separation: `SlottedPage` remains a plain byte view,
while coordination belongs at the buffer-manager and heap-file boundary.

## Required decisions before ex13

### Resolve the execution-scope conflict

The roadmap places joins in ex15, but `sql.md` sketches joins, grouping,
aggregates, ordering, and limits as ex13 work. The project should choose one
scope before implementation.

Recommended scope: ex13 implements CREATE/DROP, INSERT, UPDATE, DELETE, and
single-table SELECT with scan, WHERE filtering, and projection. The parser may
continue accepting its larger SQL subset, but unsupported execution features
should produce a clear named error until their assigned exercise. That keeps
the ex13 executor small and gives ex15 a coherent query-processing arc.

### Define SQL table storage paths

The ex11 CLI receives an explicit heap-file path per command. SQL statements
only name a table. Ex13 therefore needs one deterministic table-name-to-data-
file convention, including how a table is created, reopened, and removed by
DROP TABLE.

### Add a focused catalog writer

ex11 includes a catalog JSON parser and loader, not a writer. CREATE TABLE
needs a small schema-to-JSON writer; DROP TABLE needs the matching deletion
path. This should become the sole DDL mutation seam identified by the roadmap.

### Plan tuple updates deliberately

`tuple::Tuple` currently supports constructing tuples and reading columns, but
not replacing an existing column. UPDATE can either rebuild a tuple from its
existing values plus evaluated SET expressions, or ex13 can add a narrowly
scoped replacement operation with tests. The choice should be explicit.

## Recommended ex13 shape

Create `ex13/` as another self-contained CMake project. Copy the ex11 storage
and schema stack plus the ex12 SQL front end; do not link later exercises to
earlier exercise directories. Use one session-owned `BufferManager`, cache
catalogs and opened heap files by table name, and let a binder resolve AST
names/types before execution begins.

For UPDATE and DELETE, scan first, save the matching `RowId`s, close the
iterator's traversal, and only then mutate the heap file. This is required by
the current iterator contract and directly preserves the later transaction
discipline.

The ex13 test binary should keep the established inline PASS/FAIL pattern and
exercise real catalogs and temporary heap files end to end: DDL lifecycle,
literal and type errors, inserts, updates, deletes, scan filtering/projection,
persistence, and explicit unsupported-feature errors.

## Observations

- `ex11` resolves `tablecatalog/...` relative to the process working
  directory. Its check suite passes from `ex11/`, as documented, but fails if
  invoked from the repository root. Either document that constraint more
  prominently or make future catalog resolution project/data-root based.
- The current working tree has several untracked planning and design files,
  including `roadmap.md` and `sql.md`. They should be added intentionally if
  they are meant to define the repository's durable course plan.
- No concurrency primitives should be added in ex13. The existing physical
  layer is intentionally single-threaded; ex14 is the appropriate place to
  introduce latches together with B+tree crabbing and a thread-safe buffer
  manager.
