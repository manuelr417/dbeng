# dbeng — building a small database engine

A course-style repository: a small database engine is built up exercise by
exercise, each exercise adding one layer on top of the previous ones. Plain
C++20, no external dependencies.

## How the repo is organized

There is no root build. Every exercise lives in its own directory and is a
self-contained CLion CMake project that configures and builds independently:

```bash
cmake -S exNN -B exNN/build
cmake --build exNN/build
```

Some exercises produce more than one binary (e.g. a check-suite plus a demo
CLI); each exercise's README lists its targets and usage.

## Conventions

The exercises follow a few repo-wide rules:

- Every exercise is fully self-contained: all its files live inside its own
  directory, exercises are never wired together, and shared code is never
  hoisted to the repo root. Common headers (`Person.h`, `DataFrame.h`) are
  deliberately copied into each exercise that needs them instead of shared.
- Error shape evolved deliberately, in three steps: through ex16 a
  fallible call returns a `bool` plus an `error` string out-param that is
  cleared on entry (empty after the call means success), with ex16 adding
  the `common/Fail.h` `fail`/`wrap` idiom for the plumbing; from ex17 on,
  failures throw `common::Error` (a `std::runtime_error`) caught once at
  the CLI/check-harness boundary, and a check that guards a module's own
  contract is an `assert`. End-of-data and the group table's entry budget
  stay plain return values in every exercise.
- There is no test framework: ex07 ships an assert-based test target, the
  later exercises run PASS/FAIL `check()` calls in `check_*.cpp`
  translation units driven by `main.cpp`.

## The exercises

| Exercise | Adds | Resulting layer |
|---|---|---|
| ex01–ex05 | C/C++ foundations: struct layout, pointers, `memcpy` buffers, smart pointers | — |
| ex06 | Serializing whole arrays of `Person` records through one buffer | — |
| ex07 | `Person` records packed into fixed 4096-byte blocks on disk, first CLI | record + block storage |
| ex08 | A fixed set of shared frames with pin counts and dirty flags | buffer pool |
| ex09 | Pin/unpin over many files, dirty write-back, pluggable replacement (LRU), `alloc_page` | buffer manager |
| ex10 | Fixed-length records with per-slot occupancy in a doubly linked page chain; records addressable by `RowId{page_id, slot}` | slotted pages + heap file |
| ex11 | Schema as data: JSON table catalogs, generic self-describing tuples, fixed-width record layout derived from the catalog, session CLI over named tables | schema-driven storage engine |
| ex12 | A hand-rolled lexer, recursive-descent parser, and AST behind `sql::parse(query, result, error)` for the SQL subset (CREATE/DROP/INSERT/UPDATE/DELETE/SELECT, JOIN/GROUP BY/ORDER BY/LIMIT) | SQL front-end |
| ex13 | SQL front door over the ex11 stack: exec layer (binder, planner, executor), an ISAM index, a SQL session CLI, a bulk loader, and a parts benchmark | indexed SQL engine |
| ex14 | A second access path: the extensible hash index, the `USING HASH \| ISAM` clause, index erase on both kinds, the `delrow` escape hatch | two access paths (ISAM + hash) |
| ex15 | Grouped aggregation: `GROUP BY`, `HAVING`, and aggregate select items through a static hash operator with a restart-based spill | grouped SQL engine |
| ex16 | Error discipline restated: named errors at the boundary, asserts for internal contracts, the `common/Fail.h` idiom (behavior unchanged) | same stack, cleaner errors |
| ex17 | Failures as exceptions: `common/Error.h` thrown at the failure site, caught once at the boundary (behavior unchanged) | same stack, exceptions |
| ex18 | Readability restated: the `common/PinnedPage` RAII pin guard, named algorithm steps in `HeapFile::insert`, `std::format` messages (behavior unchanged) | same stack, reads like the algorithm |

Each exercise reuses the previous ones' code and on-disk formats, so the
later exercises form a working stack:

```text
GROUP BY/HAVING via the static hash operator (group/)   (ex15–ex18)
sql/ Lexer + Parser + AST + the index statements        (ex12–ex14)
exec/ Binder + Planner + Executor                       (ex13–ex18)
index/ IsamIndex, hashindex/ extensible + static hash   (ex13–ex14)
ex11_cli + schema/catalog + Tuple                       (ex11)
HeapFile / SlottedPage                                  (ex10)
BufferManager + ReplacementPolicy/LRU                   (ex09)
BufferPool / DataFrame                                  (ex08)
BlockFile + Person/serializer                           (ex07)
```

## Where to look

Every exercise directory has its own `README.md` with build/run instructions,
usage, and structure; the later ones also carry design documents
(`ex07/docs/explain.md`, `EXPLAIN.md`, and design notes such as ex11's
`design/`) that explain formats and decisions in depth. Start with the
README of whichever exercise you are working on.
