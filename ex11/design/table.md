# TableSchema and ColumnSchema — Implementation Plan

> **Revision (restore-tuple-serializer):** the `column_order` permutation
> and `set_column_order` are **retired** — the wire order is fixed to
> column creation order, and `ColumnSchema::column_position` is gone
> (it always equaled the column's definition index). Sections below that
> specify `column_order` / `set_column_order` / per-column positions —
> the original request's item (c) here, the class members and invariants
> around lines 25-57 and 124-238, and the wire-order discussion — describe
> a removed feature and no longer match the code. Rationale: no shipping
> code path ever reordered columns (identity everywhere outside the test
> suite); the record serializer is now the schema-bound
> `schema::TupleSerializer` (see `openspec/changes/
> restore-tuple-serializer/design.md`). Kept for the record, per the
> repo's revision-note convention.

## Prompt (original request)

> Lets now plan the implementation of a class named TableSchema, which holds
> the metadata about a table and its tuples. This class can be used to map the
> logical structure of a table into a Tuple, and help drive the
> serialization/desrialization process in general way. The TableSchema class
> as: a) table_name : - name of the table, b) column_info  - a vector
> containing a list of ColumnSchema instances, c) column_order - the order in
> which columns are layout in the raw data buffer, and d) column_name_map - a
> map that has the column name as key and as value it has column index in the
> vector of ColumnSchema instances. The class ColumnSchema represents the
> metadata for a column. It holds the following fields: a) column_name - name
> of the column, b) column_type - the type of the column, using  ColumnType
> from tuple/Column.h. Favor simplicity over raw performance, as usual.
> Generate the plan on file table.md. do not write any code yet.

## 1. Goals

- A `ColumnSchema` class: the metadata of one column — `column_name`,
  `column_type` (reusing `ColumnType` from `tuple/Column.h`), and
  `column_position` (where the column sits inside a schema-shaped tuple).
- A `TableSchema` class with exactly the four specified fields: `table_name`,
  `column_info` (vector of `ColumnSchema`), `column_order` (how the columns
  are laid out in the raw buffer), and `column_name_map` (name → index into
  `column_info`).
- Schema services that drive tuple work in a general way:
  - **validate** a tuple against the schema (count and per-position type),
  - **make_tuple** — a schema-shaped, default-valued tuple,
  - **schema-driven serialize/deserialize** — thin wrappers over the existing
    `TupleSerializer` that also apply `column_order` to the wire layout,
  - **by-name lookup** through `column_name_map`.
- Checks in the PASS/FAIL `check()` style proving the mapping, the order
  permutation, and every error path.

Non-goals (this iteration): persisting the schema itself (no catalog on
disk), storing schema/column names inside the tuple bytes, NULL/marker values,
keys/constraints (primary, foreign), default values per column, and any
performance tuning.

## 2. Design principles — clarity over performance

- **The schema is out-of-band metadata.** The wire format of `tuple.md`
  section 6 does not change by one byte: tuples stay self-describing through
  their `[type][length]` tags, and the schema lives beside them, the way a
  real system keeps a catalog separate from the data pages. Schema-driven
  serialization must produce bytes identical to hand-permuting a tuple.
- **Definition order, tuple order, wire order — three views, kept apart.**
  The `column_info` vector lists columns in definition order;
  `column_position` (per column) says where each sits in a schema-shaped
  tuple; `column_order` says how they hit the raw buffer. By-name access
  resolves through `column_position` — equal to the vector index today, but
  never assumed. The serializer wrappers are the only code that touches
  `column_order`. No user of the schema ever handles the permutations by
  hand.
- **Derived state is recomputed, not stored.** The inverse of `column_order`
  (column index → wire position) is recomputed by a linear search each time
  it is needed. The two stored indices — `column_name_map` and
  `column_position` — are exactly the ones the prompt asks for, and both are
  maintained by the single writer `add_column`.
- **The house error split continues**: `bool` + `std::string& error` (cleared
  on entry) for data-level failures — duplicate column name, non-permutation
  order, tuple/schema mismatch; exceptions (`std::out_of_range`) only for
  programming errors (bad index into `column_info`).
- **Validation at the boundary.** Every schema service that accepts a tuple
  re-checks it against the schema; nothing trusts a previous check.

## 3. The `ColumnSchema` class

`schema/ColumnSchema.h`, header-only (the metadata twin of `tuple/Column.h`,
in the new `namespace schema`):

```cpp
namespace schema {

class ColumnSchema {
public:
    ColumnSchema();                          // empty name, Integer, position 0
    ColumnSchema(const std::string& column_name,
                 tuple::ColumnType column_type,
                 std::size_t column_position);

    const std::string& column_name() const;
    tuple::ColumnType column_type() const;
    std::size_t column_position() const;

    friend bool operator==(const ColumnSchema& a, const ColumnSchema& b);

private:
    std::string column_name_;
    tuple::ColumnType column_type_;
    std::size_t column_position_;   // slot of this column in a schema tuple
};

}
```

- Three fields: the prompt's two plus `column_position`, the position of
  this column within a schema-shaped tuple's vector of columns.
- `TableSchema::add_column` is the only writer of `column_position` and
  always assigns the next free slot, so today it equals the entry's index in
  `column_info` — but all tuple-position logic routes through the field, not
  the vector index, so the two can decouple later without touching call
  sites (see open questions).
- The default constructor exists only so `column_info` can be resized; real
  columns are built from name, type, and position.
- It deliberately holds *no* value and *no* length: those belong to `Column`
  (the value side). The schema says what must be there; the tuple says what
  is.

## 4. The `TableSchema` class

`schema/TableSchema.h` + `schema/TableSchema.cpp`, `namespace schema` — the
tuple-layer types it uses are spelled `tuple::Tuple` and `tuple::ColumnType`:

```cpp
class TableSchema {
public:
    explicit TableSchema(const std::string& table_name);

    /// Appends a column: one entry in column_info (column_position = the
    /// current column count), one entry in column_name_map, and one slot
    /// appended to column_order (identity order until set_column_order
    /// says otherwise).
    /// Fails on a duplicate or empty column name.
    bool add_column(const std::string& name, tuple::ColumnType type,
                    std::string& error);

    /// Installs a wire layout: `order[p]` is the column_info index stored at
    /// wire position p. Accepts only a real permutation of 0..n-1.
    bool set_column_order(const std::vector<std::size_t>& order,
                          std::string& error);

    const std::string& table_name() const;
    std::size_t column_count() const;
    const ColumnSchema& column_info(std::size_t index) const;  // throws
    const std::vector<std::size_t>& column_order() const;

    /// The column_name_map service: nullopt for an unknown name. The result
    /// indexes column_info; the column's tuple slot is
    /// column_info(index).column_position().
    std::optional<std::size_t> column_index(const std::string& name) const;

    /// True when the tuple matches the schema: same column count, and every
    /// tuple position p holds the type of the schema column whose
    /// column_position is p. Named error otherwise.
    bool validate(const tuple::Tuple& tuple, std::string& error) const;

    /// A fresh tuple in column_position order, every column at its default
    /// value (int 0, "", double 0.0) with the schema's types.
    tuple::Tuple make_tuple() const;

private:
    std::string table_name_;
    std::vector<ColumnSchema> column_info_;
    std::vector<std::size_t> column_order_;   // wire position -> column index
    std::unordered_map<std::string, std::size_t> column_name_map_;
};
```

- The four fields are exactly the prompt's `table_name`, `column_info`,
  `column_order`, `column_name_map` (private, accessors above).
- `column_order` direction: **`column_order[p]` = the column_info index that
  lives at wire position p** — it reads as "the order the columns are laid
  out in the buffer". It is kept a full-length vector at all times (identity
  while building), so every method can assume it is a valid permutation.
- `column_index` is the only lookup into `column_name_map`; everything else
  goes through it. Position of a column on the wire (needed by the wrappers
  and useful to tests) is derived by searching `column_order`.

### Schema-driven serialization (declared in the same header)

```cpp
/// validate -> permute into wire order -> tuple::serialize
bool serialize(const TableSchema& schema, const Tuple& tuple,
               char* buffer, std::size_t max_len, std::string& error);

/// tuple::deserialize -> validate -> un-permute into column_info order
bool deserialize(const TableSchema& schema, const char* buffer,
                 std::size_t max_len, Tuple& tuple, std::string& error);
```

- Both fail *before touching bytes* when `validate` rejects the tuple (on
  serialize) and leave the out-tuple empty on any deserialize failure — the
  same discipline as `TupleSerializer`.
- The permutation is applied by building one intermediate `Tuple` whose
  columns are in wire order (or reading one back on deserialize): a plain
  copy loop around the existing `TupleSerializer`. One serialization code
  path for the whole project; the schema layer adds mapping, not encoding.
- `column_position` closes the mapping: the value of `column_info[i]` lives
  at tuple slot `column_info[i].column_position()`, and its wire slot is the
  q with `column_order[q] == i`. The wrappers do no searching — serialize
  walks wire positions through `column_order` and pulls each value from its
  `column_position`; deserialize walks the wire and drops each value into
  its `column_position`.

## 5. Semantics of the two orders

| Order | Lives in | Meaning | Who sees it |
|---|---|---|---|
| Definition | the `column_info` vector | the catalog listing: index i is the i-th column added | internal |
| Tuple | `column_position`, per column | a schema-shaped tuple's slot p holds the column whose `column_position` is p; by-name access resolves through it | everyone |
| Wire | `column_order` | wire position p holds `column_info[column_order[p]]` | serializer wrappers only |

By default `column_order` is the identity permutation: the wire order equals
the declaration order (name is column 0 and comes first on the wire, then
age, then height). Columns are never reordered unless `set_column_order` is
called. The permutation capability exists for the cases where logical and
physical order legitimately differ — legacy file layouts, and
fixed-width-first packing (real engines store int/double columns together
ahead of variable-length strings so pages pack cleanly).

Example: `people(name String, age Integer, height Double)` built in that
order, then `set_column_order({1, 0, 2})` — age first, a deliberately
unnatural layout chosen to prove the mapping is general. Logical tuple
`("Ruth", 42, 3.14)` hits the wire as:

| Region   | Wire range (bytes) | Contents                              |
|----------|--------------------|---------------------------------------|
| Header   | `[0, 4)`           | count = 3                             |
| Wire pos 0 | `[4, 16)`        | age: type 0, length 4, int 42         |
| Wire pos 1 | `[16, 28)`       | name: type 1, length 4, "Ruth"        |
| Wire pos 2 | `[28, 44)`       | height: type 2, length 8, double      |

Total is still 44 bytes — the permutation reorders columns, it does not
resize anything. `deserialize` walks the wire, then hands back the tuple in
`column_position` order, so the round-trip `operator==` holds. In this
example the positions are sequential (name 0, age 1, height 2), so tuple
order and definition order coincide; the wrappers never rely on that.

## 6. Validation rules (all with named errors)

- `add_column`: duplicate name → "column 'age' already exists"; empty name →
  "column name is empty". It also assigns `column_position` (always the next
  free slot), so the positions over `column_info` always form exactly
  0..n-1 — maintained by construction, not by checking.
- `set_column_order`: size must equal `column_count()` and the values must be
  exactly 0..n-1 — rejects wrong length, out-of-range indices, duplicates
  ("column order is not a permutation of 0..2").
- `validate(tuple)`: count mismatch ("tuple has 3 columns, schema has 4");
  type mismatch at a position ("column 1: tuple has Double, schema says
  Integer"). Length is not re-checked: `Column`'s constructors make it
  mirror the value, exactly as in the tuple layer.
- Schema-driven `deserialize`: first the byte-level checks of
  `TupleSerializer` (truncation, unknown tag), then `validate` against the
  schema — a wire tuple whose type disagrees with the schema is refused even
  though the bytes alone would deserialize fine. This is the general
  "drive by schema" guarantee the prompt asks for.

## 7. Placement, namespace, and integration

- New subdirectory `schema/` with its own namespace `schema` (decided):
  `schema/ColumnSchema.h` (header-only), `schema/TableSchema.h`,
  `schema/TableSchema.cpp` — the same one-namespace-per-layer pattern as
  ex10's `bufman` and `heapfile`.
- Dependency direction stays one-way: `schema/` includes `tuple/Column.h`,
  `tuple/Tuple.h`, `tuple/TupleSerializer.h` and spells those types
  `tuple::...`; nothing in `tuple/` includes the schema files.
- `CMakeLists.txt`: add `schema/ColumnSchema.h schema/TableSchema.cpp
  schema/TableSchema.h` to `SOURCES` of the `ex11` target; nothing else
  changes.
- `main.cpp`: new check sections appended after the tuple sections, same
  harness.

## 8. Test plan (appended to `main.cpp` in `check()` style)

1. **ColumnSchema**: constructor sets name, type, and position; equality of
   two schemas.
2. **Building a schema**: `add_column` grows count, `column_info(i)` reports
   name/type, and positions come back 0, 1, 2, … in insertion order;
   duplicate name fails with an error; empty name fails.
3. **column_name_map**: `column_index("age")` finds the right index; unknown
   name returns nullopt.
4. **column_order**: defaults to identity; `set_column_order` accepts a
   permutation; rejects wrong length, out-of-range value, and duplicates,
   each with an error.
5. **validate**: a `make_tuple()` product passes; wrong column count fails;
   wrong type at a position fails with the position named in the error.
6. **make_tuple**: correct count, per-position types, default values
   (0, "", 0.0), tuple order == `column_position` order.
7. **Identity-order serialize is byte-identical**: schema with default order
   produces exactly the bytes of plain `TupleSerializer::serialize`.
8. **Permutation on the wire**: with order {2, 0, 1} — deliberately
   non-self-inverse, so a direction bug cannot hide — the wire bytes match
   the Option-A reading [height, name, age], and `deserialize(schema, ...)`
   hands back the logical-order tuple equal to the input.
9. **Schema-guarded deserialize**: patch a wire tag so a position's type
   disagrees with the schema; plain `TupleSerializer::deserialize` accepts
   the bytes but the schema wrapper refuses with a named error and an empty
   out-tuple.
10. **Failure discipline**: every schema failure path leaves `error`
    non-empty and (where applicable) the out-tuple empty.
11. **By-name workflow**: `make_tuple`, set the "age" column by name via
    `column_index` + `column_position`, schema-serialize,
    schema-deserialize, read back by name.

## 9. Implementation steps

1. `schema/ColumnSchema.h`: the three-field metadata class.
2. `schema/TableSchema.h` + `.cpp`: fields, `add_column` with map/order
   maintenance, `set_column_order` permutation check, lookups, `validate`,
   `make_tuple`, and the two serializer wrappers over `TupleSerializer`.
3. Register the three files in `CMakeLists.txt`; build clean.
4. Append check sections 1–11 to `main.cpp` (unit checks first, wire
   permutation and schema-guard tests after).
5. Build and run `./ex11/build/ex11`; all checks pass, exit 0.

## 10. Resolved questions

- `column_order` direction — **decided: Option A**. `column_order[p]` is the
  column_info index living at wire position p: the vector lists the columns
  in buffer order, matching the prompt's wording, and both wrappers walk the
  wire search-free. Test 8 uses a non-self-inverse permutation to pin this.
- `column_position` writer — **decided: `add_column` only**, assigning
  sequential positions. A `set_column_positions` mutator can arrive later
  without call-site changes.
- Empty column names — **decided: rejected** in `add_column`.
- `make_tuple` defaults — **decided: int 0, "", double 0.0** (NULL handling
  stays out of scope).
- Schema-driven wrappers — **decided: free functions declared in
  `TableSchema.h`**, no separate `SchemaSerializer` file pair.
