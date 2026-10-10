# General-Purpose Tuple and Column — Implementation Plan

## Prompt (original request)

> Plan the implementation a general purpose tuples and column classes. The tuple
> is an array of columns. Each column has three fields: a) column_value - a c++
> variant type, b) column_type - an integer that represents the column data
> type, and c) column_length - the length in bytes of the column. The column
> value can be one of: integer, string, double. The column_type will drawn from
> an enum that mirrors the types that can be held in the column value, so
> perhaps is best to define the enum as a type and use it in the class to
> represent column types. The tuple is a vector of columns. Serialization and
> deserialization will not happen between a buffer and a Tuple, not hardwire for
> Part as it was done before. This is an academic exercise, so favor clarity
> over raw performance in your plan. Generate the plan into a file named
> tuple.md

Reading of the serialization requirement: bytes move **between a raw buffer and
a `Tuple`**, generically for any mix of int/string/double columns — the
hardwired `Person` layout of `part/PersonSerializer` is not reused or extended.

## 1. Goals

- A `Column` class with exactly the three specified fields: `column_value`
  (a `std::variant` over `int`, `std::string`, `double`), `column_type` (drawn
  from a new `ColumnType` enum), and `column_length` (payload size in bytes).
- A `Tuple` class that is a `std::vector<Column>` with a minimal, safe API.
- Free serialization functions that convert between a `Tuple` and a raw byte
  buffer (`char*`), self-describing for any column mix — no dependence on
  `part/Person` or any fixed schema.
- Checks in the repo's PASS/FAIL `check()` style proving the byte layout,
  round-trips, and every error path.

Non-goals (this iteration): a `Schema` class, NULL/marker values, more value
types, storing tuples inside `SlottedPage`/`HeapFile`, endianness portability,
and performance tuning of any kind.

## 2. Design principles — clarity over performance

- **The variant index is the single source of truth for the value's kind; the
  stored `column_type` mirrors it.** The spec asks for all three fields to be
  stored, so they are — and because redundant state can drift, every boundary
  function (constructor, serializer) validates the mirror:
  `static_cast<int>(column_type) == column_value.index()` and
  `column_length == payload size of the value`. A broken mirror is an error,
  never a silently mis-copied byte.
- **One uniform header for all types.** Every column serializes as
  `[type][length][payload]` even though int/double lengths are implied by their
  type. One code path, one layout table, and a format a generic reader can walk
  without knowing the value types.
- **Plain `memcpy` encoding**, native endianness, exactly like ex10's page
  header — no bit packing, no varints, no portability layer (single-host
  academic format, documented).
- **Errors follow the house convention**: `bool` return + `std::string& error`
  out-param, cleared on entry, empty = success — the same discipline as
  `BlockFile`, `BufferManager`, and `HeapFile`. Exceptions are reserved for
  programming errors (`Tuple::column` out-of-range throws, matching
  `BufferPool::frame`).
- **Everything recomputable is recomputed at the boundary.** `serialized_size`
  is derived from the tuple on every call; nothing about sizes is cached in
  the classes.

## 3. The `ColumnType` enum and the `ColumnValue` alias

Both defined in `tuple/Column.h`, before and used by the class:

```cpp
enum class ColumnType : int {
    Integer = 0,   // payload: the int, 4 bytes
    String  = 1,   // payload: the characters, column_length bytes
    Double  = 2,   // payload: the double, 8 bytes
};

// The value of one column: an int, a string, or a double. std::variant is a
// tagged union — exactly one alternative is active, and .index() reports
// which (0 = int, 1 = string, 2 = double).
using ColumnValue = std::variant<int, std::string, double>;
```

- Underlying type `int`, so `column_type` *is* "an integer representing the
  data type" while still being spelled `ColumnType` in the class — type-safe
  at the call site, trivially writable to the buffer with `memcpy`.
- The alias names the variant once, in one commented place; everywhere else
  (fields, accessors, serializer helpers) just says `ColumnValue`. Students
  meet `std::variant` once instead of deciphering the template spelling at
  every use site — and if the type list ever changes, it changes here only.
- The enum members mirror the three alternatives of `ColumnValue`, in the same
  order, so the wire tag equals `column_value.index()` by construction.
- A small helper in the same header gives the fixed payload size and is the
  one place that knows it:
  `std::optional<std::size_t> fixed_payload_size(ColumnType)` — 4, nullopt
  (length varies), 8.

## 4. The `Column` class

`tuple/Column.h`, header-only (like `DataFrame.h`):

```cpp
class Column {
public:
    Column();                        // Integer, value 0, length 4
    explicit Column(int v);
    explicit Column(double v);
    explicit Column(std::string v);  // length = v.size()

    const ColumnValue& column_value() const;
    ColumnType column_type() const;
    std::size_t column_length() const;

private:
    ColumnValue column_value_;
    ColumnType column_type_;
    std::size_t column_length_;      // payload bytes only, never the header
};
```

- The three fields are exactly the ones the prompt names; they are private so
  the type/length mirror cannot drift silently — the constructors are the only
  writers, and each derives type and length from the value in one place.
- `column_length` semantics: **payload bytes only** — 4 for int, 8 for double,
  `string.size()` for string. It excludes the 8-byte per-column wire header.
  On-disk record size is `8 + column_length`.
- `operator==` compares all three fields (cheap, and defends the mirror
  invariant in tests).
- No setters this iteration — a column is built from its value. Direct mutation
  would break the mirror for no pedagogical gain.

## 5. The `Tuple` class

`tuple/Tuple.h` + `tuple/Tuple.cpp`:

```cpp
class Tuple {
public:
    void add_column(const Column& column);
    void add(int v);             // convenience: build the Column here
    void add(double v);
    void add(std::string v);

    std::size_t column_count() const;
    const Column& column(std::size_t index) const;  // throws std::out_of_range
    void clear();

private:
    std::vector<Column> columns_;    // the tuple IS a vector of columns
};
```

- No capacity, no schema, no cached sizes — the vector is the entire state.
- Bounds errors throw (`std::out_of_range` via an explicit check, matching the
  `BufferPool` misuse convention); data errors use `bool` + `error`.
- `operator==` delegates to the columns, so round-trip tests are one-line.
- Iteration, if a consumer needs it, goes through `column(i)` /
  `column_count()` for now; `begin()/end()` forwarding is a non-goal until a
  consumer exists.

## 6. Serialized byte layout

Wire format, written/read with `memcpy` in native endianness
(`static_assert(sizeof(int) == 4 && sizeof(double) == 8)` in the .cpp):

**Tuple record:**

| Region   | Offset range (bytes)      | Contents                                  |
|----------|---------------------------|-------------------------------------------|
| Count    | `[0, 4)`                  | 4-byte int: number of columns N           |
| Column 0 | `[4, 4 + S0)`             | first column record, `S0 = 8 + len0`      |
| …        |                           |                                           |
| Column N-1 | `[…, 4 + ΣSi)`          | last column record                        |

**Column record** (size `S = 8 + column_length`):

| Field   | Offset within record | Size          | Contents                       |
|---------|----------------------|---------------|--------------------------------|
| type    | `[0, 4)`             | 4             | `ColumnType` as int (0/1/2)    |
| length  | `[4, 8)`             | 4             | `column_length` (payload only) |
| payload | `[8, 8 + length)`    | `length`      | value bytes (see below)        |

**Payload encodings** — the only per-type knowledge in the whole format:

- `Integer`: `memcpy` of the `int`; length is always 4.
- `Double`: `memcpy` of the `double` (IEEE-754); length is always 8.
- `String`: exactly `length` bytes of the characters, **no NUL terminator** —
  the length prefix already says where it ends.

**Worked example** — `Tuple` with 42, "Ruth", 3.14:

| Region     | Wire range (bytes) | Contents                    |
|------------|--------------------|-----------------------------|
| Header     | `[0, 4)`           | count = 3                   |
| Column 0   | `[4, 16)`          | type 0, length 4, int 42    |
| Column 1   | `[16, 28)`         | type 1, length 4, "Ruth"    |
| Column 2   | `[28, 44)`         | type 2, length 8, double    |

Total: `4 + (8+4) + (8+4) + (8+8)` = **44 bytes**, and in general
`serialized_size = 4 + Σ (8 + column_lengthᵢ)`.

An empty tuple is legal: 4 bytes, count 0.

## 7. Serializer API and validation

`tuple/TupleSerializer.h` + `.cpp`, free functions over any `Tuple` (the
`PersonSerializer` split of data vs. encoding, generalized):

```cpp
std::size_t serialized_size(const Tuple& tuple);
bool serialize(const Tuple& tuple, char* buffer, std::size_t max_len,
               std::string& error);
bool deserialize(const char* buffer, std::size_t max_len, Tuple& tuple,
                 std::string& error);
```

- `serialize`: clears `error`, validates every column's mirror
  (type matches variant index, length matches payload), then writes the count
  and columns. Fails with a message when `max_len < serialized_size` or a
  mirror is broken. `max_len` is a bound, not a target — trailing buffer bytes
  are left untouched.
- `deserialize`: clears `error`, clears `tuple` first (overwrite semantics),
  then reads the count and walks columns. Per-column validation, in order:
  type tag must be 0/1/2; for `Integer`/`Double` the stored length must equal
  the fixed payload size; the payload must fit the remaining buffer. Any
  violation fails with a named error ("unknown column type 7",
  "column 2 truncated", …) and leaves `tuple` empty. Trailing bytes after the
  last column are ignored (a buffer may be a 4096-byte block holding one
  tuple).
- Both are pure byte movers: no `part/` include, no record-size constants from
  any other layer.

## 8. Placement, namespace, and integration

- New subdirectory `tuple/` beside `part/`: `Column.h` (header-only),
  `Tuple.h`/`Tuple.cpp`, `TupleSerializer.h`/`TupleSerializer.cpp`.
- Dependency direction: `tuple/` includes **nothing from this project** — only
  the standard library. Nothing includes `tuple/` yet; it is a standalone new
  layer, exactly how `heapfile/` started. `part/PersonSerializer` is left
  untouched for the heap-file layer.
- Namespace: the repo gives each new layer its own namespace (ex10: `bufman`
  and `heapfile`). Proposal: `namespace tuple` matching the directory —
  confirm before coding (see open questions).
- `CMakeLists.txt`: add
  `tuple/Column.h tuple/Tuple.cpp tuple/Tuple.h tuple/TupleSerializer.cpp tuple/TupleSerializer.h`
  to the `SOURCES` list of the `ex11` target; nothing else changes.
- `main.cpp`: the CLion template is replaced by the `check()` PASS/FAIL
  harness, copied in style from `ex10/main.cpp` (bool + message, failure
  counter, "All checks passed." at the end, exit 0 iff all pass).

## 9. Test plan (added to `main.cpp` in `check()` style)

1. **Column construction**: each constructor sets type and length correctly
   (int → Integer/4, double → Double/8, string → String/size; "Ruth" → 4,
   empty string → 0).
2. **Default column**: Integer, value 0, length 4.
3. **Tuple API**: `add` overloads build the right columns; `column_count`
   tracks; `column(i)` returns the right column; out-of-range index throws.
4. **serialized_size**: hand-computed values for empty tuple (4), single int
   (16 = 4 + 8 + 4), the 44-byte example, and a long-string tuple.
5. **Byte-for-byte layout**: serialize {42, "Ruth", 3.14}; memcmp each region
   against an expected buffer built with `memcpy` (count, then type/length/
   payload per column) — the layout table in section 6 has exactly one
   counterpart in this check.
6. **Round-trips**: each single-type tuple; a mixed 4-column tuple; a tuple
   with an empty string and a 300-character string; `operator==` after
   deserialize.
7. **Overwrite semantics**: deserialize into a tuple that already holds
   columns; the old columns are gone.
8. **Trailing bytes ignored**: deserialize a tuple from a 4096-byte buffer
   whose remaining bytes hold a marker pattern; the marker survives.
9. **Error paths** (each with the error message non-empty):
   serialize into a too-small buffer; truncated buffer mid-column; unknown
   type tag; Integer column with length != 4; Double column with length != 8.
10. **Mirror violation on the wire**: an in-memory mirror violation is
    unrepresentable (Column's fields are private, constructors are the only
    writers), so the check corrupts the wire instead — serialize a valid
    tuple, patch one column's type tag in the bytes, and `deserialize` must
    refuse it. `serialize`'s own mirror re-check stays in as defense in depth.
11. **Through the buffer pool**: serialize a tuple into a `DataFrame::buffer`
    of a pinned frame, `unpin(dirty)`, `flush_all`, re-pin, deserialize —
    the tuple survives the pool/disk path unchanged.

## 10. Implementation steps

1. `tuple/Column.h`: enum, `fixed_payload_size`, the `Column` class.
2. `tuple/Tuple.h` + `.cpp`: the vector-of-columns API.
3. `tuple/TupleSerializer.h` + `.cpp`: `serialized_size`, `serialize`,
   `deserialize` with the section-6 layout and section-7 validation.
4. Register the five files in `CMakeLists.txt`; build clean.
5. Replace `main.cpp` with the check harness; implement tests 1–11 in order
   (unit checks first, buffer-pool integration last).
6. Build and run `./ex11/build/ex11`; all checks pass, exit 0.

## 11. Open questions

- Namespace `tuple` (matching the directory) — confirm, or prefer keeping
  everything in `bufman` / a different name?
- `column_length` = payload bytes only (int 4, double 8, string N), wire
  record `8 + column_length` — confirm this reading of "length in bytes of
  the column".
- Native-endian `memcpy` format with a 4-byte type tag and 4-byte length
  (uniform header, slightly redundant for int/double) — confirm.
- Default-constructed `Column` is Integer/0 — confirm (alternative: a fourth
  `Invalid` type; rejected for now as unused complexity).
- Trailing bytes after a tuple are ignored by `deserialize` — confirm
  (alternative: require exact size match, which would pin the format to
  tuple-only buffers).
