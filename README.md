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
- Storage-layer errors all use the same shape: a `bool` return plus an
  `error` string out-param that is cleared on entry — an empty string after
  the call means success.
- There is no test framework: ex07 ships an assert-based test target, the
  later exercises run PASS/FAIL `check()` calls inline in `main.cpp`.

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

Each exercise reuses the previous ones' code and on-disk formats, so the
later exercises form a working stack:

```text
Lexer + Parser + AST (sql::parse)     (ex12)
ex11_cli + schema/catalog + Tuple     (ex11)
HeapFile / SlottedPage                (ex10)
BufferManager + ReplacementPolicy/LRU (ex09)
BufferPool / DataFrame                (ex08)
BlockFile + Person/serializer         (ex07)
```

## Where to look

Every exercise directory has its own `README.md` with build/run instructions,
usage, and structure; the later ones also carry design documents
(`ex07/docs/explain.md`, `EXPLAIN.md`, and design notes such as ex11's
`design/`) that explain formats and decisions in depth. Start with the
README of whichever exercise you are working on.
