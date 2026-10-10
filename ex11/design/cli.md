# ex11_cli — the p1dbeng_demo-equivalent session CLI — Implementation Plan

## Prompt (original request)

> I want a similar cli for project ex11. Make a plan in file cli.md. Do
> not generate any code yet.
>
> adapt your plan to change tablesample to tablecatalog
>
> adapt your plan to only do the equivalent of p1dbeng_demo and call it
> ex11_cli
>
> In your plan, consider modules to bring csv data that follows the schema
> a json file. This was done in PersonCsv in ex10. Can it be done as a
> general module?
>
> Consider in your plan also modules to add randomly generated records
> that follow a schema in a json file. In ex10 this was done by
> PersonGenerator.
>
> in your plan, for the cli commands, drop the .json. You can add the
> extension. For example: scan person person.bin
>
> For the future I will add threads to have multiple users and sessions.
> But that will be later on when transactions and locking get added.

"Similar" reads as: the REPL of `~/CLionProjects/p1dbeng` (`p1dbeng_demo`,
`p1db.cpp`) reshaped for ex11's engine. The third directive pins the scope
to **exactly p1dbeng_demo's surface** — `append`, `read`, `seek`, `scan`,
`bulk`, `quit`, nothing more (no `create`, `update`, `erase`, or `help`
command) — and names the binary `ex11_cli`. The second directive renames
the catalog directory: `tablesample/` becomes `tablecatalog/`. The fourth
and fifth directives promote the two data-moving helpers from CLI glue to
**general modules**: `schema/TableCsv` (the generalization of
`PersonCsv`/`PartCsv`) and `schema/TableGenerator` (the generalization of
`PersonGenerator`/`PartGenerator`). The sixth makes the commands type
table **names**, not paths: the CLI resolves `person` to
`tablecatalog/person.json` itself, exactly as p1dbeng's commands typed
`person`/`part`. Both modules generalize for the same reason: everything
the per-type modules hardwired — field count, types, widths — is exactly
what `TableSchema` already declares. The seventh is a trajectory note:
threads, multiple users, and multiple sessions arrive later, together
with transactions and locking — this iteration builds the single-session
shape they will multiply.

## 1. Goals

- A new executable **`ex11_cli`** from a new `ex11_cli.cpp`: an interactive
  REPL over `HeapFile`, no command-line parameters, the six commands
  p1dbeng_demo has, mapped onto the tuple heap file:

  | p1dbeng_demo | ex11_cli | The mapping |
  |---|---|---|
  | `append <table> <csv> <bin>` | `append <table> <csv> <file>` | Loads rows through the new general module `schema/TableCsv` (the generalization of `PersonCsv`/`PartCsv`); creates the file when missing. |
  | `read <table> <bin> <block>` | `read <table> <file> <page> <slot>` | One record by row id (`HeapFile::find`) — read is row-level. |
  | `seek <table> <bin> <block>...` | `seek <table> <file> <page> <slot>...` | One or more records by row id, duplicates included, `(pool hit)` per record. p1dbeng's seek was block-level; the heap file's user-facing vocabulary has no blocks — pages exist only inside `RowId` — so seek reads rows too. |
  | `scan <table> <bin>` | `scan <table> <file>` | Full chain scan with row ids and a count. |
  | `bulk <table> <count> <bin>` | `bulk <table> <count> <file>` | Rows from the new general module `schema/TableGenerator` (the generalization of `PersonGenerator`/`PartGenerator`). |
  | `quit` | `quit` | One flush for the whole session. |

- **The catalog replaces the table adapter.** p1dbeng needed a
  `PersonOps`/`PartOps` struct pair per record type plus hand-written CSV
  loaders; ex11's `schema::TableSchema` already knows names, types, and
  widths, so the CSV parser, tuple printing, and usage messages are all
  written once, schema-driven. Adding a table to the CLI means writing a
  JSON file — zero code.
- The session model, identical in spirit: one buffer manager (and its
  pool) for the whole session, shared by every heap file, kept alive
  across commands (pages an earlier command brings in are hits for a
  later one), files opened on first use and kept open, `quit` (or end of
  input) flushes and closes everything once, exit code 1 only if that
  final flush fails.
- Errors keep the CLI alive: every failure prints a message on stderr and
  the prompt returns; the exit code is decided only at `quit`.

Non-goals (this iteration): everything p1dbeng_demo does not have — no
`create`/`update`/`erase`/`help` commands (the library APIs stay available
to `main.cpp`'s check suite), no SQL, no config file (fixed LRU pool for
the session), no inline-tuple syntax (values come from CSV rows, as in
p1dbeng), keys or updates by value, multiple sessions (planned: threads,
multiple users/sessions, transactions, and locking — one exercise at a
time), and any performance work.

## 2. Design principles — clarity over performance

- **The schema is the CLI.** p1dbeng's per-table code — two CSV loaders,
  two generators, hand-written validation, block serializers, per-type
  counters, print functions — collapses into `load_table_schema`, the two
  new modules `schema/TableCsv` and `schema/TableGenerator` (sections 4
  and 4a), `HeapFile`, and one generic printer. The CLI adds glue, not
  layers.
- **One `BufferManager` for the whole session** — the architecture the
  manager was built for (ex09): its page table is keyed by
  `(fd, block_number)`, so files never collide in the pool, and every
  `HeapFile` in the session shares the one pool. This needs the manager's
  missing piece: a per-file close. Today `HeapFile::close` runs
  `close_all`, which would slam shut every other open file — the reason
  an earlier draft gave each file its own manager. The plan instead adds
  `BufferManager::close_file` (flush that file's dirty pages, drop its
  pool entries, close its fd) and switches `HeapFile`'s lifecycle to it;
  the "only user of its manager" contract in `heapfile/HeapFile.h`
  becomes "shares the session's manager, owns only its own file's
  lifecycle". `quit` closes each session file once, exactly one flush
  per file.
- **The `<table>` argument is a catalog name, resolved by the CLI.**
  p1dbeng's commands carried `<table>` (`person`/`part`); ex11_cli's
  carry a table name too, and resolve it to
  `tablecatalog/<name>.json` — the `.json` extension and the directory
  are the CLI's job, never the user's. Catalogs load once per name and
  are cached; a file is bound to the table it was first used with, and a
  later command naming a different table is refused (`HeapFile::open`
  would refuse anyway; the CLI refuses earlier, with a clearer message).
  An unknown name fails with `unknown table 'x' (no
  tablecatalog/x.json)`; typing a path or an extension
  (`tablecatalog/person`, `person.json`) is refused with a hint to use
  the bare name.
- **Validation stays in the schema layer.** CSV tokens are parsed by
  column type; every other constraint (string width, NUL) is enforced by
  `validate`/`pack` inside `HeapFile::insert`, whose named errors the CLI
  prints verbatim. The CLI never re-implements a constraint the schema
  layer already owns.
- **House REPL mechanics carry over from p1dbeng** because they are
  proven: `isatty` prompt suppression for scriptable sessions, usage
  lines on wrong arity, help printed for unknown commands, invalid CSV
  rows reported and skipped (never fatal), one flush at exit.

## 3. Command reference

Every command: unknown → the command list; wrong arity → a one-line
usage hint; bad table/file/row → message on stderr, session continues.
There is no `help` command — help is what the CLI prints when it does not
understand you, exactly like p1dbeng_demo. `<table>` is a catalog name
(`person`, `car`) resolved to `tablecatalog/<table>.json`.

| Command | Syntax | Effect |
|---|---|---|
| `append` | `append <table> <csv-file> <file>` | Loads the CSV (schema-driven; invalid rows are skipped with `line N: reason`), creating `<file>` when missing, and inserts every valid row through `HeapFile::insert`. Ends with `appended N record(s), skipped M row(s)` and a `flush_all` (p1dbeng parity) — the pages stay resident for later `read`/`seek`. |
| `read` | `read <table> <file> <page> <slot>` | Reads one record by row id: `HeapFile::find` on `RowId{page, slot}`; prints the tuple with its row id and a `(pool hit)` marker when the page was already resident. |
| `seek` | `seek <table> <file> <page> <slot>...` | Reads one or more records by row id, in argument order, duplicates included — the batch form of `read`. Each record prints with its row id and a `(pool hit)` marker when its page was already resident; a bad row id prints a message and the loop continues. |
| `scan` | `scan <table> <file>` | Iterates the chain; prints every record with its `RowId`; ends with `scanned N record(s)`. |
| `bulk` | `bulk <table> <count> <file>` | Rows from the generator module `schema/TableGenerator` (section 4a), numbering continuing after the existing record count; reports `appended N generated record(s)`. |
| `quit` | — | Flushes and closes every session file; exit 1 only if a flush fails. |

Example session (p1dbeng's README shape, ex11's engine):

```text
> append person people.csv people.bin
line 1: column 'pid': expected an integer
line 4: column 'age': expected an integer
appended 2 record(s), skipped 2 row(s)
> bulk person 420 people.bin
appended 420 generated record(s)
> scan person people.bin
(1,0)  pid=1, name=Alice, age=30, city=PR
(1,1)  pid=2, name=Bob, age=25, city=NY
...
scanned 422 record(s)
> read person people.bin 1 0
(1,0)  pid=1, name=Alice, age=30, city=PR
> seek person people.bin 1 1 1 0
(1,1)  (pool hit)  pid=2, name=Bob, age=25, city=NY
(1,0)  (pool hit)  pid=1, name=Alice, age=30, city=PR
> quit
```

(Files are opened into the session on first use; a file created by
`append` binds to the catalog that command named.)

## 4. The general CSV module (`schema/TableCsv.h` + `.cpp`)

The direct answer to the PersonCsv question: **yes, it generalizes**,
because everything `PersonCsv`/`PartCsv` hardwired per type — the field
count, the per-field types, the name of each field for error messages —
is exactly what `TableSchema` declares. One module replaces both loaders,
the way one `RecordCodec` replaced both serializers. It lives in the
`schema/` layer (its only dependencies — `TableSchema`, `tuple::Tuple` —
are already that layer's), `namespace schema`, named after its concern
like `TupleSerializer` and `TableSchemaJson`: the JSON dialect of a table
is its catalog, the CSV dialect is its data.

```cpp
/// One parsed CSV line as a schema-shaped tuple; false with a named
/// reason (no line prefix — the caller adds "line N: "). The out tuple
/// is cleared first and stays empty on failure.
bool parse_csv_row(const TableSchema& schema, const std::string& line,
                   tuple::Tuple& tuple, std::string& reason);

/// Everything one CSV load reports: the valid rows in file order and how
/// many lines were skipped.
struct CsvLoadResult {
    std::vector<tuple::Tuple> tuples;
    std::size_t skipped = 0;
};

/// Loads CSV rows shaped by `schema`: one record per line. Invalid rows
/// print "line N: reason" to `diagnostics` and are skipped, never fatal.
/// False with a named `error` only when the file itself cannot be read.
bool load_csv(const TableSchema& schema, const std::string& path,
              CsvLoadResult& result, std::ostream& diagnostics,
              std::string& error);
```

- **The split mirrors `TableSchemaJson`**: a row-level core
  (`parse_csv_row`) plus a file-level wrapper (`load_csv`) — the core is
  what the check suite can test without temp files, the wrapper is what
  the CLI calls.
- **Row rules** (in `parse_csv_row`, all with named reasons): split on
  `,`, no quoting or escaping (a comma or NUL cannot appear inside a
  value — the same limitation as p1dbeng's CSV rows and the fixed-width
  format itself); the token count must equal `column_count`; empty tokens
  are legal and mean the empty string for String columns, a parse error
  for Integer/Double; Integer via `std::from_chars` fully consumed
  (`4.5`, ` 4`, `+4`, and `pid` all fail with "column 'pid': expected an
  integer"); Double via `std::from_chars` for `double`, fully consumed;
  String tokens verbatim, with width and NUL rules left to
  `validate`/`pack` — the module refuses a row only for syntax, and a
  too-wide string surfaces at insert time as the row's reason.
- **Layering stays one-way**: the module produces `tuple::Tuple`s and
  knows nothing of `HeapFile` — the caller (the CLI's `append`) owns
  insertion. `schema/` never includes `heapfile/`.
- **What deliberately does not generalize**: p1dbeng's per-type *value*
  rules (`pid > 0`, `age >= 0`, color in [0, 5]) were business rules, not
  type rules; the module enforces syntax only. Range constraints would be
  a schema extension (a `min`/`max` in the catalog), out of scope here.
- **Diagnostics contract** identical to p1dbeng's loaders: invalid rows
  print `line N: reason` on the caller's stream and are skipped, never
  fatal; blank lines are skipped silently; a trailing `\r` is stripped
  (files written by Windows editors still load); the summary counts come
  back in `CsvLoadResult`.
- The CLI's `append` shrinks to: `load_csv` → create-if-missing →
  `HeapFile::insert` per tuple → print the summary. The loader is also
  usable by future consumers (an `import` command, other tools) — that is
  what makes it a module rather than a helper.

## 4a. The general generator module (`schema/TableGenerator.h` + `.cpp`)

The same answer for `PersonGenerator`/`PartGenerator`: **it generalizes**,
because the only thing those modules knew about their record type was
which fields exist and how wide the strings are — all of it declared by
`TableSchema`. One module replaces both, the way `TableCsv` replaces both
loaders. Same layer, same namespace, same naming family:

```cpp
/// Generates `count` schema-shaped tuples with pseudo-random values that
/// always pass `validate`: Strings within their declared widths and free
/// of NUL, numerics in a small readable range. Deterministic for a given
/// seed. The first Integer column (position order) is a sequence
/// starting at `first_id` — the record's visible identity, the way
/// PersonGenerator numbered pids; every other column is random.
std::vector<tuple::Tuple> generate_tuples(const TableSchema& schema,
                                          std::size_t count,
                                          std::uint32_t seed,
                                          int first_id = 1);
```

- **Random but always valid**: a seeded `std::mt19937` drives every
  value — Integers in `[0, 1000)`, Doubles in `[0, 100)` with one
  decimal, Strings of random letters with length in
  `[1, column_size]` (width 8 for undeclared columns, which record
  packing refuses anyway). Nothing the module emits can fail
  `validate` — the generator, like the CSV module, produces tuples and
  knows nothing of `HeapFile`.
- **The sequence convention** (open question 1 in section 10): the leading Integer
  column counts `first_id, first_id+1, …` so scans stay readable
  (`pid=186` after a bulk of 420) and `bulk` can continue p1dbeng's
  numbering by passing `first_id = existing + 1`. All-random is the
  recorded alternative.
- **Determinism**: same schema + seed + first_id → identical tuples.
  That is what makes the generator checkable (exact-value checks, not
  just property checks) and demos reproducible. PersonGenerator was
  fully deterministic too — pattern arithmetic instead of a PRNG; the
  module keeps the determinism, drops the hardwired patterns.
- **Checks in `main.cpp`** (section 8): two loads with the same seed
  produce equal vectors; a different seed does not; every generated
  tuple passes `validate`; the sequence lands in the first Integer
  column; strings never exceed their declared width; `count` is exact.

The CLI's `bulk` shrinks to: count existing records (one scan tally) →
`generate_tuples(schema, count, seed, existing + 1)` → insert per tuple →
print the summary.

## 5. Session model — one manager, many heap files

```cpp
struct SessionFile {
    std::unique_ptr<heapfile::HeapFile> file;  // bound to its table's schema
    const schema::TableSchema* schema;         // owned by the catalog cache
};
```

- **One `bufman::BufferManager` for the session**, created before the
  prompt loop with the fixed LRU pool — the same shape as p1dbeng's
  session, and the shape the manager's `(fd, block)` page table was
  designed for. Every `HeapFile` is constructed against it; pages one
  command brings in are hits for the next command, including across
  different files sharing the pool.
- **Catalog cache**: `unordered_map<string name, TableSchema>` keyed by
  the table name — `load_table_schema("tablecatalog/" + name + ".json")`
  runs once per name; an unknown or malformed catalog is a command error
  ("unknown table 'x' (no tablecatalog/x.json)"), not a session error.
- **File table**: `unordered_map<string path, SessionFile>` — the
  HeapFile is created and opened on first use. `append` on a missing
  path runs `HeapFile::create` (with the command's catalog) before
  inserting — p1dbeng's append-to-new-file behavior; an existing path is
  opened.
- **`HeapFile::close` becomes per-file** (section 6): it flushes and
  closes only its own file through the new
  `BufferManager::close_file`, leaving every other session file's pages
  and descriptor untouched. The old `close_all`-based close would have
  ended every other file's session — the single reason an earlier draft
  gave each file its own manager.
- **Binding check**: if a command names a file already open under a
  different table (compared by `table_name()`), the command is refused:
  `"people.bin is bound to table 'person'"`.
- **`quit`/EOF**: each session file's `HeapFile::close` runs exactly
  once (one flush per file, through the shared manager); any failure is
  printed, and the exit code is 1 iff any flush failed — p1dbeng's
  contract exactly. (A final `close_all` is unnecessary and skipped: the
  per-file closes already drained the manager.)
- No per-command close, no open/close commands: files live for the
  session, like p1dbeng's.
- **Shaped for what's next** (threads, multiple users and sessions,
  transactions, locking — a later exercise): this iteration keeps the
  session state in one cohesive place — the manager, the catalog cache,
  and the file table are the session; nothing global hides outside it.
  When sessions multiply, each session instantiates that state (its own
  manager and pool, its own bindings) and the per-file `close_file` and
  shared-pool design carry over unchanged. Until transactions and
  locking exist, the engine stays single-threaded by contract: one
  command at a time, no internal locks anywhere — stated here so nobody
  mistakes the absence of mutexes for an oversight.

## 6. Printing, and the two small heap-file accessors

- **Tuple printer**: one function over any schema — for each column, its
  `column_name()` and its value from the variant, formatted
  `pid=1, name=P1, age=19, city=PR`. No per-table print functions exist.
- **Row ids in scan output**: `read`/`seek` are addressed by `RowId`, so
  `scan` prints row ids too — but `HeapFileIterator` today yields only
  tuples and advances its internal cursor past the record it just
  returned. The plan adds one accessor:

  ```cpp
  /// The row id of the record most recently reported by next().
  const RowId& row_id() const;
  ```

  implemented by storing the yielded `RowId` before `next` returns true.
  No other change to `next`'s signature or semantics.
- **Pool-hit reporting** (p1dbeng's seek signature move): before a
  `read`/`seek` fetch, the CLI asks the manager whether the record's page
  is already resident and prints `(pool hit)` before that record. That
  needs the file handle, which `HeapFile` owns privately — hence the
  second, equally small accessor:

  ```cpp
  /// True when `page_id` is currently resident in the file's pool.
  bool resident(std::uint32_t page_id) const;
  ```

  a two-line read-through to `BufferManager::resident`. The heap file's
  own pin paths stay untouched. (A block-level `HeapFile::read_page` was
  considered for a block-flavored seek and dropped: with seek reading
  rows, the layer needs no block-shaped API. `scan` cannot report hits —
  the iterator hides its frames by design, exactly like p1dbeng's scan.)
- **The manager's per-file close** — the enabler of the one-manager
  session. `BufferManager` gains:

  ```cpp
  /// Flushes the dirty pages of this one file, drops its entries from
  /// the page table and frame map, closes its descriptor, and removes it
  /// from the registry. Other files' pages and descriptors are untouched.
  /// Fails when a frame of the file is still pinned.
  bool close_file(File& file, std::string& error);
  ```

  ~35 lines: walk `frame_page_` for entries with the file's fd, flush
  dirty ones, release the frames, erase the `page_table_` range, remove
  the registry entry in `files_`, close the fd. `HeapFile::close`
  switches from `close_all` to `close_file(file_, ...)`, and the
  `create`/`open` failure unwinds do the same — no more closing the
  whole manager to get rid of one file. The "expects to be the only user
  of its manager" contract in `heapfile/HeapFile.h` is rewritten to
  "shares the session's manager; owns its file's lifecycle". Checks in
  `main.cpp`: two files open through one manager, closing one leaves the
  other readable, its dirty pages flushed, and its pages evicted from
  the pool; `close_file` refuses while a frame of the file is pinned.
- The additions get checks appended to `main.cpp`.

## 7. Placement, namespace, and integration

- New file `ex11_cli.cpp` at the ex11 root (beside `main.cpp`), anonymous
  namespace for its helpers, no new library namespace — it is a program,
  not a layer.
- `CMakeLists.txt`: the current single `SOURCES` list becomes the core
  source list; p1dbeng's shape is the model —

  ```cmake
  add_library(ex11core STATIC <core sources>)
  add_executable(ex11 main.cpp)        # links ex11core
  add_executable(ex11_cli ex11_cli.cpp) # links ex11core
  ```

  — so the core compiles once and both binaries link it. The check-suite
  target keeps its name (`ex11`) and its behavior; the CLI adds a second
  target only.
- **Buffer pool / heap file layer additions**: `BufferManager::close_file`
  (bufpool/), `HeapFileIterator::row_id()`, `HeapFile::resident(page_id)`,
  and the `HeapFile` lifecycle switch to per-file close (heapfile/) —
  section 6; the only changes outside the CLI and the new modules.
- **New modules**: `schema/TableCsv.h/.cpp` and `schema/TableGenerator.h/
  .cpp` (both `namespace schema`), registered in `CMakeLists.txt`
  (`SOURCES` gains `schema/TableCsv.cpp schema/TableCsv.h
  schema/TableGenerator.cpp schema/TableGenerator.h`) and covered by new
  check sections in `main.cpp` (section 8) — the modules are testable
  precisely because they are modules; the CLI alone would not be.
- **Catalog directory renamed: `tablesample/` → `tablecatalog/`** (the
  carried-over directive). The folder keeps exactly its contents
  (`person.json`, `car.json`) and becomes the fixed directory the CLI
  resolves table names against — `scan person …` means
  `tablecatalog/person.json`. No new catalogs are required. The rename
  touches only `main.cpp`'s `load_table_schema` paths and one doc comment
  in `schema/TableSchemaJson.h`.
- Documentation: the command reference lives in this file; a short
  "Running the CLI" note can join a future ex11 README (out of scope
  here).

## 8. Verification plan (scripted sessions, no new test framework)

The REPL reads stdin and suppresses the prompt when piped, so every
scenario below is a `printf … | ./build/ex11_cli` run with asserted
output/exit code — the same manual-verification tier p1dbeng used. Two
verification tiers this time: the new module gets **check sections in
`main.cpp`** (it is a library module now — row parser and file loader,
no CLI required), and the CLI gets **scripted sessions** for the glue.
The layers beneath (codec, heap file, manager) are already fully covered
by the 270-check suite.

Check sections for `schema/TableCsv` (appended to `main.cpp`):

1. **Row parsing**: a valid person row parses to the equal tuple; empty
   strings for String columns; a full-width string parses; arity
   mismatch, bad integer, bad double, and empty numeric token each fail
   with a named reason.
2. **Width rules surface at insert, not parse**: a too-wide string parses
   fine (syntax is legal) and is refused by `HeapFile::insert` with the
   schema's named error.
3. **File loading**: a temp CSV with valid rows, a header row, and bad
   rows → correct tuple order, `skipped` count, `line N: reason`
   diagnostics on the caller's stream; missing file → named error, empty
   result; blank lines and `\r` line endings are tolerated.

Check sections for `schema/TableGenerator` (appended to `main.cpp`):

4. **Generation**: same seed → equal vectors, different seed → different
   values; every generated tuple passes `validate`; the count is exact.
5. **Conventions**: the first Integer column counts `first_id, first_id+1,
   …`; String values never exceed their declared width and contain no
   NUL; the generator's output round-trips through `pack`/`unpack`.

Scripted CLI sessions:

1. **Append/scan/read/seek happy path** — the section-3 transcript, end
   to end: CSV with skipped rows and `line N` diagnostics, the bulk
   append, the scan with row ids, a row-level `read`, and seek's
   duplicate-with-`(pool hit)` sequence.
2. **Session pooling** — a `read` of a page a previous command brought in
   prints `(pool hit)`; in a fresh session (after `quit` and reopen) the
   first `read` of a page shows no marker and the second does — the
   miss/hit pair.
3. **Persistence** — append, `quit`, a second session (`scan`) sees the
   records; exit code 0.
4. **Page split** — `bulk` 400 on a fresh person file (capacity 185 per
   page): the scan reports records on pages 1, 2, and 3, and `seek` of a
   page-2 row id prints its record.
5. **Error paths** — unknown command (command list printed), wrong arity
   (usage line), a CSV with bad rows (skipped with `line N: reason`, the
   session continues), an unknown row id in `read` and in `seek` (message,
   loop continues), an unknown table name (and a path-with-extension used
   as a name), a file used with two different tables, `bulk 0` — each
   prints a message and the session continues; exit code 0 after `quit`.
6. **Exit code** — pipe everything, check `$?` is 0; the flush-failure
   path stays exercised by the check suite (the CLI's only exit-1 path).

## 9. Implementation steps

1. **Rename the catalog directory**: `mv tablesample tablecatalog`; update
   every `load_table_schema("tablesample/…")` path in `main.cpp` (the
   catalog tests 1–9 and the heap tests) and the one doc comment in
   `schema/TableSchemaJson.h` that names the directory; build and run
   `./ex11/build/ex11` — all 270 checks pass, exit 0. (No CMake change:
   the JSON catalogs were never in `SOURCES`.)
2. **Session plumbing**: `BufferManager::close_file` in
   `bufpool/BufferManager.{h,cpp}`; `HeapFile::close`/`create`/`open`
   switch to it and the single-manager contract doc lands in
   `heapfile/HeapFile.h`; checks (two files through one manager, close
   one, the other survives; pinned-frame refusal) plus one check each
   for `row_id()`/`resident()` in `main.cpp`; build — all checks pass.
3. **The data modules**: `schema/TableCsv.{h,cpp}` (`parse_csv_row`,
   `load_csv`) and `schema/TableGenerator.{h,cpp}` (`generate_tuples`),
   registered in `CMakeLists.txt`, their check sections (1–5 of section
   8) appended to `main.cpp`; build — all checks pass.
4. `CMakeLists.txt`: core sources into `ex11core`, add the `ex11_cli`
   target; build both.
5. `ex11_cli.cpp`: REPL plumbing (config-free init, prompt loop, token
   splitting, dispatch), catalog cache, session file table, the generic
   printer, and the six commands (`append` is `load_csv` +
   create-if-missing + insert per tuple; `bulk` is a scan tally +
   `generate_tuples` + insert per tuple).
6. Run the section-8 scripted sessions; fix what they surface.
7. Build and run `./ex11/build/ex11`; all checks pass, exit 0.

## 10. Open questions

- **The generator's sequence convention** — proposed: the first Integer
  column (position order) counts `first_id, first_id+1, …` so `bulk`
  continues p1dbeng's numbering and scans stay readable; every other
  column is seeded-random. Alternative: all columns random (drop
  `first_id`). Seed default proposed: 42, overridable per call.
- **No `conf.json`** — proposed: one fixed LRU pool of 8 frames for the
  session (enough for the demos; the manager, not the CLI, is the
  instrumentation point). Porting p1dbeng's `config/` layer is easy
  later but drags a new directory/namespace into ex11 for no course
  value this iteration.
- **CSV header rows** — proposed: no special case; a header is simply a
  row that fails type parsing and is skipped with `line 1: …` (the
  observable behavior matches p1dbeng's "header counts as one skipped
  row", with a better message). Alternative: detect-and-skip a first row
  that equals the column names.
- **`append` to an empty (zero-byte) file** — proposed: allowed;
  `HeapFile::create` formats it (the same rule the heap file already
  applies). Alternative: refuse and demand a fresh path.
