# Loading a TableSchema from a JSON Catalog — Implementation Plan

## Prompt (original request)

> Lets now plan the loading of a table schema from a JSON file. The files
> `tablesample/person.json` and `tablesample/car.json` describe a table:
> its name and its columns, each with a name, a type, and a size. Plan how
> to turn one of those files into a `TableSchema`. Favor simplicity over
> raw performance, as usual. Generate the plan on file catalog.md. do not
> write any code yet.

(Reconstructed wording — this plan was written after a session resume. The
JSON files and the deferred catalog non-goal in `table.md` fix the intent;
flag anything that drifted from the original ask.)

## 1. Goals

- A loader that reads a JSON catalog file — the format of
  `tablesample/person.json` / `tablesample/car.json` — and produces a
  `schema::TableSchema`, going through `add_column` so the name map, the
  positions, and the identity `column_order` are maintained by the one
  existing writer.
- A fourth `ColumnSchema` field, `column_size`: the declared width the JSON
  carries for every column (`pid` 4, `name` 10, `city` 3). The schema layer
  so far has nowhere to put it; the JSON is the first source that has it.
- A minimal JSON parser, hand-rolled in the loader's `.cpp` — the repo has
  no dependencies and needs only the subset the samples use.
- Checks in the PASS/FAIL `check()` style: both sample files load into the
  expected schemas, a loaded schema drives the existing
  serialize/deserialize round-trip, and every malformed-input path fails
  with a named error.

Non-goals (this iteration): writing a schema back to JSON (save), the
reverse mapping into the hardwired `Person` bytes, `column_order` in the
file (always loads as identity), NULL values, a general-purpose JSON
library, and any performance work.

## 2. Design principles — clarity over performance

- **The JSON file is the catalog; the `TableSchema` is the in-memory copy.**
  Loading is a one-way mapping, field by field, with no reinterpretation:
  `table_name` → the constructor argument, each entry of `columns` → one
  `add_column`. Nothing is inferred, reordered, or normalized.
- **`column_size` is declared width, not wire length.** The tuple wire
  format of `tuple.md` section 6 is untouched: strings still carry their
  actual length. `column_size` lives only on the schema side — it says how
  wide the column is declared (SQL's `CHAR(10)`), while `tuple::Column`'s
  `column_length` keeps saying what a concrete value is. The split already
  established — *the schema says what must be there; the tuple says what
  is* — gains its third clause: the schema also says how wide.
- **Errors follow the house split**: `bool` + `std::string& error` (cleared
  on entry) for everything data-level — bad path, bad bytes, bad schema
  content. A parse error names the line; a content error names the offending
  key or column.
- **Overwrite semantics, all-or-nothing result.** Like `tuple::deserialize`,
  the loader clears its out-parameter first and builds into a fresh
  `TableSchema`; the caller's variable is either replaced by a complete,
  valid schema or left holding an empty one (table name set, zero columns).
  A failure mid-file never leaves a half-loaded schema behind.
- **Everything recomputable is recomputed.** No caching of file contents or
  parse trees; the loader parses once per call and discards.

## 3. The JSON catalog format (as the samples define it)

```json
{
  "table_name": "person",
  "columns": [
    { "column_name": "pid",  "column_type": "Integer", "size": 4 },
    ...
  ]
}
```

The mapping is one-for-one:

| JSON                     | Schema side                            |
|--------------------------|----------------------------------------|
| `"table_name"` (string)  | `TableSchema` constructor argument     |
| `"columns"` (array)      | one `add_column` per element, in order |
| `"column_name"` (string) | `ColumnSchema::column_name`            |
| `"column_type"` (string) | `ColumnSchema::column_type`            |
| `"size"` (integer)       | new `ColumnSchema::column_size`        |

- `"column_type"` accepts exactly `Integer`, `String`, `Double` — the
  spellings of `tuple::ColumnType`. Anything else is a named error.
- `"columns"` must be present; an empty array is refused (a table with no
  columns cannot make or validate tuples, and no sample uses one).
- No `column_order` in the file: the loaded schema keeps the identity order
  `add_column` installs. The permutation machinery of `table.md` stays a
  code-level feature.

## 4. `column_size` on `ColumnSchema`

`schema/ColumnSchema.h` grows one field — header-only, same file:

```cpp
ColumnSchema(const std::string& column_name,
             tuple::ColumnType column_type,
             std::size_t column_position,
             std::size_t column_size = 0);   // declared width; 0 = undeclared

std::size_t column_size() const;
```

- Defaulted fourth parameter, so every existing call site (the schema tests
  in `main.cpp`, `TableSchema::add_column`) keeps compiling unchanged — code
  built in memory simply has no declared width yet.
- `operator==` extends to all four fields, defending the invariant in tests.
- `add_column` keeps its two-argument signature; it creates columns with the
  undeclared size 0. The loader is the only caller that knows a width, so it
  sets the size *after* `add_column` through a small setter — see the open
  questions; the alternative (a three-argument `add_column` overload) is
  rejected as a second writer path for no gain.

## 5. The loader API

`schema/TableSchemaJson.h` + `schema/TableSchemaJson.cpp`, `namespace
schema` — the JSON dialect of `TableSchema`, named after the class it
produces the way `TupleSerializer` is named after its concern:

```cpp
/// Parse catalog text (UTF-8, the sample-file shape) into a fresh schema.
/// On failure: false, named error, `schema` left empty (name set, no
/// columns). On success the caller's schema is replaced wholesale.
bool parse_table_schema(const std::string& text, TableSchema& schema,
                        std::string& error);

/// Read the file at `path`, then parse_table_schema. File errors (missing,
/// unreadable) are named errors like any other.
bool load_table_schema(const std::string& path, TableSchema& schema,
                       std::string& error);
```

- The string-level core exists so tests can exercise malformed JSON without
  temp files; `load_table_schema` is `read file → parse`, nothing more.
- Reading uses `std::ifstream` + `std::ostringstream` — no C files, no mmap.
- Inside the `.cpp`, parsing builds the schema through the public API only:
  construct with `table_name`, `add_column` per entry, then set each
  column's size. Name-map, position, and order maintenance stay in
  `TableSchema`'s hands; the loader adds mapping, not state.

## 6. The minimal JSON parser

Anonymous-namespace recursive descent in `TableSchemaJson.cpp`, ~100 lines,
covering exactly the grammar the catalog needs:

- **Values**: object, array, string, integer. `true`/`false`/`null` and
  floating-point numbers are *recognized but rejected* with a named error
  ("line 5: catalog uses only objects, arrays, strings, and integers") —
  clearer than silently misparsing, cheaper than a full parser.
- **Objects**: `{ "key" : value, ... }`; duplicate keys inside one object
  are an error (a duplicated `"size"` is almost certainly a typo).
- **Strings**: double-quoted, with the standard escapes `\" \\ \/ \b \f \n
  \r \t` and `\uXXXX` for BMP code points. The samples use none of this;
  the escapes exist so a future name like `"first\nname"` parses
  honestly rather than by luck.
- **Integers**: optional `-`, digits, no fraction/exponent — the only number
  the format needs. Stored as `long long`; the loader range-checks before
  narrowing to `std::size_t`.
- **Whitespace**: spaces, tabs, newlines, carriage returns between tokens.
- A cursor (position + line counter) threads through the parser; every
  failure reports `"line N: ..."` — the single most useful thing a catalog
  error can say. Malformed input can never loop or overflow: every
  recursive call consumes at least one character, checked before recursing.

The parser produces values, not a DOM — the loader walks it directly, so
there is no intermediate tree to design or hold.

## 7. Validation rules (all with named errors)

In `parse_table_schema`, in order:

1. **Parse-level**: trailing garbage after the top-level object; unexpected
   token; unterminated string; bad escape — each `"line N: ..."`.
2. **Top level**: the document must be an object; `"table_name"` required,
   non-empty string; `"columns"` required, an array. Missing keys are named
   ("catalog is missing 'columns'"), not guessed.
3. **Per column entry** (each must be an object):
   - `"column_name"`: required, non-empty, not a duplicate of an earlier
     column — the duplicate check comes free through `add_column`'s own
     error ("column 'age' already exists"), which the loader surfaces with
     the line number prepended.
   - `"column_type"`: required, one of the three exact spellings —
     otherwise `"line N: unknown column type 'Float'"`.
   - `"size"`: required, positive integer. Type/natural-width consistency
     (`Integer` with size 4, `Double` with size 8) is *not* enforced this
     iteration — the declared width is metadata, and refusing to load a
     hand-written catalog over it would be the tail wagging the dog (see
     open questions).
4. **Result discipline**: any failure leaves the out-schema empty; the
   error names the first problem encountered (parse and validation stop at
   the first error — no error accumulation).

## 8. Placement, namespace, and integration

- `schema/TableSchemaJson.h` / `.cpp`, `namespace schema` — same layer as
  the classes it produces; no new directory, no new namespace. The catalog
  *files* stay where they are (`tablesample/`); only the loader is new.
- Dependency direction: `schema/` already includes `tuple/...`; the loader
  adds nothing beyond `<fstream>`, `<sstream>`, `<string>`. Nothing includes
  `TableSchemaJson.h` except `main.cpp`.
- `CMakeLists.txt`: add `schema/TableSchemaJson.cpp schema/TableSchemaJson.h`
  to the `SOURCES` of the `ex11` target; nothing else changes.
- `main.cpp`: new check sections appended after Schema test 11, same
  harness. The sample paths are relative (`tablesample/person.json`) — the
  check runner is invoked from the ex11 root like always.

## 9. Test plan (appended to `main.cpp` in `check()` style)

1. **Load `person.json`**: table name "person"; four columns in file order
   with the right names, types, and sizes (pid/Integer/4, name/String/10,
   age/Integer/4, city/String/3); `column_order` is identity;
   `column_index` finds every name; `validate(make_tuple(), ...)` passes.
2. **The catalog matches the hardwired record**: the sum of the loaded
   column sizes (4+10+4+3) equals `bufman::kRecordSize` (21) — the JSON
   describes exactly the layout `PersonSerializer` hardwires, checked
   rather than asserted.
3. **Load `car.json`**: five columns, sizes 4/15/10/4/4 — proves the loader
   is general, not person-shaped.
4. **Loaded schema drives the wire**: `make_tuple` from the loaded person
   schema, fill pid/name/age/city, `schema::serialize` →
   `schema::deserialize` → equal tuple; read "age" back by name through
   `column_index` + `column_position`.
5. **Overwrite semantics**: parse into a schema that already holds columns;
   after a successful load the old columns are gone; after a failed load
   the out-schema is empty.
6. **Parse errors** (string-level, each with a non-empty "line N" error):
   empty text; truncated JSON; trailing garbage; unterminated string;
   bad escape; a floating-point size; `true` as a table name.
7. **Content errors**: missing `table_name`; empty `table_name`; missing
   `columns`; `columns` not an array; empty `columns` array; column entry
   not an object; missing `column_name` / `column_type` / `size`; unknown
   type spelling; size 0; duplicate column name (error carries
   `add_column`'s message); empty column name.
8. **Failure discipline**: every failed load leaves the out-schema with
   zero columns and `error` non-empty.
9. **Missing file**: `load_table_schema` on a nonexistent path fails with
   the path in the error.

## 10. Implementation steps

1. `schema/ColumnSchema.h`: add `column_size` (constructor parameter with
   default, accessor, `operator==`); rebuild — all existing checks must
   still pass untouched.
2. `schema/TableSchemaJson.h` + `.cpp`: the two free functions, the
   recursive-descent parser, the section-7 validation.
3. Register the two files in `CMakeLists.txt`; build clean.
4. Append check sections 1–9 to `main.cpp`.
5. Build and run `./ex11/build/ex11`; all checks pass, exit 0.

## 11. Open questions

- **Setter for `column_size`** — the loader needs one line of mutability
  after `add_column` (proposed: `void set_column_size(std::size_t)` on
  `ColumnSchema`, or the loader rebuilds the `column_info` entry; confirm
  which). The alternative — `add_column(name, type, size, error)` — makes
  the JSON's shape leak into `TableSchema`'s signature.
- **Type/size consistency** — enforce `Integer`→4 / `Double`→8 at load
  time (proposed: no, metadata stays descriptive), or leave it entirely to
  the user?
- **`columns: []`** — refuse (proposed) or accept an empty table?
- **Escapes/`true`/floats in the parser** — reject-with-message (proposed)
  versus silently accepting broader JSON than the format needs.
- **Save direction** — writing a `TableSchema` back to JSON is explicitly
  out of scope this iteration; confirm it is not wanted alongside the
  loader.
