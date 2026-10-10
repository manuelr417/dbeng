# sql.md — plan: the SQL front-end (ex12) and its executor (ex13)

The engine so far stores and retrieves rows by row id; a human drives it
through positional commands (`read person people.bin 1 0`). The next step
is a **SQL front-end**: a hand-rolled parser that turns a statement string
into a clean AST, then an executor that walks those statements over the
ex11 stack. This document is the plan for both, with the parser (ex12)
specified in full and the executor (ex13) sketched.

Decisions already made:

- **Hand-rolled recursive descent** (sqlparser.txt option 4): own lexer +
  parser (~600–900 lines), zero external dependencies, an AST this repo
  fully owns. No vendored sources, no codegen step.
- **Two exercises, two arcs.** ex12 delivers the front-end (lexer, parser,
  AST, check suite); ex13 wires the AST to the engine. Neither the plan
  nor the code mixes them: ex12 knows nothing of `HeapFile`, ex13 knows
  nothing of tokens.
- **Namespace `sql`** — `sql::parse`, `sql::SelectStatement` — matching
  the layer style of `tuple::`, `schema::`, `heapfile::`.

## Repo conventions this exercise follows

- All files inside `ex12/` (later `ex13/`); never wired to other
  exercises; common headers are copied in when the executor phase needs
  them — ex13 copies what it borrows from ex11, exactly as ex10 copied
  from ex09.
- `cmake_minimum_required(VERSION 4.3)`, C++20, CLion builds into
  `cmake-build-debug/`, local builds into `build/` (both gitignored).
- Errors are values: `bool` return + `error` string out-param, cleared on
  entry. The parser reports `"line L: message"` like the ex11 JSON
  catalog loader; parse failures never throw.
- No `std::vector<std::byte>`; the AST holds no byte buffers at all.
- No test framework: PASS/FAIL `check()` calls in `main.cpp`, exit 0 iff
  all pass.

## The SQL subset

One table's worth of statement forms, in the grammar the parser accepts.
Keywords are case-insensitive; identifiers are case-sensitive and must
match the catalog (`pid`, not `PID`).

```text
statement :=
    CREATE TABLE ident '(' column_def (',' column_def)* ')'
  | DROP TABLE ident
  | INSERT INTO ident VALUES '(' literal (',' literal)* ')'
          (',' '(' literal (',' literal)* ')')*
  | UPDATE ident SET ident '=' expr (',' ident '=' expr)* [WHERE expr]
  | DELETE FROM ident [WHERE expr]
  | select_stmt

select_stmt :=
    SELECT select_items
    FROM ident [alias] (JOIN ident [alias] ON expr)*
    [WHERE expr]
    [GROUP BY column_ref (',' column_ref)*]
    [ORDER BY order_item (',' order_item)*]
    [LIMIT int]

select_items := '*' | select_item (',' select_item)*
select_item  := expr [[AS] alias]
order_item   := column_ref [ASC | DESC]
column_def   := ident type
type         := INTEGER | INT | DOUBLE | VARCHAR '(' int ')'
column_ref   := ident ['.' ident]
literal      := int | double | 'string'
```

Expressions (precedence low → high; `x` binds tighter than `y` means
`y`'s operands are `x`):

```text
expr        := or
or          := and (OR and)*
and         := not (AND not)*
not         := NOT not | comparison
comparison  := additive [('=' | '<>' | '!=' | '<' | '<=' | '>' | '>=') additive]
additive    := multiplicative (('+' | '-') multiplicative)*
multiplicative := unary (('*' | '/' | '%') unary)*
unary       := '-' unary | primary
primary     := literal | column_ref | function | '(' expr ')'
function    := ident '(' ('*' | expr) ')'
```

Exactly one comparison per level — `a < b < c` is a parse error, not a
silent reinterpretation.

Aggregate functions: `COUNT(* | expr)`, `SUM`, `AVG`, `MIN`, `MAX` over
an expression. Unknown function names are parse errors
(`unknown function 'FOO'`). A bare aggregate (`SELECT COUNT(*) FROM t`)
is an implicit single group.

**Non-goals** (deliberate, each with a clean upgrade path): subqueries,
`HAVING`, `DISTINCT`, `OFFSET`, outer joins (`LEFT/RIGHT/FULL`), `t.*`
after a plain `*`, `INSERT INTO … SELECT`, arithmetic in `VALUES`, `NULL`
and three-valued logic — ex11 tuples have no nulls and the plan keeps it
that way. `AS` in FROM (table alias) is supported — self-joins need it.

## ex12 — the front-end

### Layout

```text
ex12/
  CMakeLists.txt     ex12core static lib + ex12 (checks) + ex12_demo
  Token.h            Token, TokenType, keyword table
  Lexer.h / .cpp     scanner: query text -> token stream
  Ast.h              expression + statement node structs
  Parser.h / .cpp    recursive descent + sql::parse entry point
  AstPrinter.h/.cpp  AST -> normalized SQL text (debug + round-trip tests)
  main.cpp           the check suite (the ex12 binary)
  ex12_demo.cpp      read lines from stdin, print the AST (ex12_demo)
  README.md / EXPLAIN.md
```

One layer, so the sources are flat (ex01–ex09 style); the `sql/` sub-
directory split is unnecessary for four modules.

### Token and Lexer

`Token` is `{ TokenType type; std::string text; std::size_t line; }` —
text is the lexeme (identifiers keep their case, string literals are
unescaped to their value, numbers keep their spelling). Types: keyword
per word (`And`, `Or`, `Select`, … as one enum or one `Keyword` type with
a `KeywordKind` — pick the single-enum form for switch-ability),
`Identifier`, `Integer`, `Double`, `String`, `Comma`, `LParen`,
`RParen`, `Dot`, `Star`, `Semicolon`, operators (`Eq`, `Ne`, `Lt`, `Le`,
`Gt`, `Ge`, `Plus`, `Minus`, `Slash`, `Percent`), `End`.

Scanner rules:

- Whitespace skips; `--` starts a line comment (skipped to end of line).
- Identifiers/keywords: `[A-Za-z_][A-Za-z0-9_]*`; the keyword table is
  consulted case-insensitively (`select` ≡ `SELECT`).
- Numbers: digits with optional fraction (`123`, `3.14`); a second dot is
  an error (`line 1: malformed number '1.2.3'`).
- Strings: single quotes, `''` escapes to one quote; unterminated is an
  error with its line.
- Any other character: `line L: unexpected character 'x'`.

The lexer is checked independently in the suite: feeding it a string and
asserting the token sequence — no parser involved.

### AST

Node structs, one per statement kind plus one expression node, all under
`sql::`. Statements share a tagged base; expressions are one tagged
struct with a payload per kind (they are small and there are only five
kinds — a class hierarchy per expression kind would be ceremony).

**Why tagged structs, not `std::variant` or a class hierarchy.** The
audience is the course arc itself: ex01–ex05 already teach struct layout,
pointers, and buffers, so `Kind` + fields + `switch` is familiar
territory. The alternatives both cost more than they pay:

- A class hierarchy with proper subtypes forces the visitor pattern or
  double dispatch just to evaluate or print an expression — the most
  ceremony per node of the three shapes.
- `std::variant` still requires one struct per kind (so it is more types,
  not fewer concepts), adds the `std::visit`/overloaded-lambda idiom that
  students reliably stumble on, and — because a variant cannot contain
  itself — needs a wrapper node for the recursion anyway
  (`struct ExprNode { std::variant<...> as; }`), which is the same tagged
  idea with extra syntax.

The known downside of the tagged shape is silent misuse: nothing in the
type system stops a reader from touching `left` on a `Literal` node. The
plan contains that the cheap way — every tree walk (printer, equality,
later the binder and evaluator) is one `switch` with a `default` that
refuses, and those walks are the only code that touches payloads.

```cpp
namespace sql {

struct Expr; using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
    enum class Kind { Literal, ColumnRef, Binary, Unary, Function };
    Kind kind;
    // Literal:   LiteralValue value { int64 i; double d; std::string s; Type tag; }
    // ColumnRef: std::string table;  // "" when unqualified
    //            std::string column;
    // Binary:    BinaryOp op; ExprPtr left, right;
    // Unary:     UnaryOp op; ExprPtr operand;
    // Function:  FunctionCode fn; ExprPtr arg;  // nullptr == COUNT(*)
};

struct SelectItem { ExprPtr expr; std::string alias; bool star; };
struct OrderItem  { ExprPtr expr; bool descending; };
struct TableRef   { std::string name; std::string alias; }; // alias "" if none
struct JoinClause { TableRef table; ExprPtr on; };

struct Statement {
    enum class Kind { CreateTable, DropTable, Insert, Update, Delete, Select };
    Kind kind;
    virtual ~Statement() = default;
};

struct CreateTableStatement : Statement { std::string table; std::vector<ColumnDef> columns; };
struct ColumnDef             { std::string name; DataType type; std::size_t size; };
struct DropTableStatement    : Statement { std::string table; };
struct InsertStatement       : Statement { std::string table; std::vector<std::vector<ExprPtr>> rows; };
struct UpdateStatement       : Statement { std::string table; std::vector<SetClause> sets; ExprPtr where; };
struct SetClause             { std::string column; ExprPtr value; };
struct DeleteStatement       : Statement { std::string table; ExprPtr where; };
struct SelectStatement       : Statement {
    std::vector<SelectItem> items;
    TableRef from;
    std::vector<JoinClause> joins;
    ExprPtr where;
    std::vector<ExprPtr> group_by;
    std::vector<OrderItem> order_by;
    std::uint64_t limit = 0; bool has_limit = false;
};

}
```

`DataType` mirrors the catalog one-for-one — `Integer`, `String`,
`Double`, with `size` carried for `VARCHAR(n)` — so ex13's CREATE TABLE
can write `tablecatalog/<name>.json` without translation (`INT` and
`INTEGER` both parse to `Integer`). The AST does not include
`tuple::ColumnType` or any ex11 type: the front-end is self-contained,
and the binder in ex13 does the mapping.

The same shape, drawn — six tagged statements, one tagged expression
node, six plain data carriers:

```text
Statement  (tagged base: Kind + virtual dtor)
├── CreateTableStatement   table, columns[]                CREATE TABLE
├── DropTableStatement     table                           DROP TABLE
├── InsertStatement        table, rows[][]                 INSERT … VALUES
├── UpdateStatement        table, sets[], where?           UPDATE … SET
├── DeleteStatement        table, where?                   DELETE FROM
└── SelectStatement        items[], from, joins[],         SELECT
                           where?, group_by[],
                           order_by[], limit?

Expr  (one tagged struct, five kinds)
├── Literal     value {int64 | double | string}     1   3.14   'PR'
├── ColumnRef   table? + column                     pid   c.owner_id
├── Binary      op + left + right                   age > 30
├── Unary       op + operand                        -3   NOT x
└── Function    fn + arg (nullptr = *)              COUNT(*)   SUM(age)

Data carriers (plain aggregates, no tags)
SelectItem{expr, alias, star}    OrderItem{expr, descending}
TableRef{name, alias}            JoinClause{table, on}
ColumnDef{name, type, size}      SetClause{column, value}
```

#### A worked example

One statement and the tree `parse()` builds for it — the arrows are the
`ExprPtr`/`unique_ptr` edges, so the tree owns its expressions top-down:

```sql
SELECT city, COUNT(*), AVG(age)
FROM person JOIN car ON pid = owner_id
WHERE age >= 18
GROUP BY city
ORDER BY city
LIMIT 5
```

```text
SelectStatement
├─ items
│   ├─ SelectItem  expr → ColumnRef{ table: "", column: "city" }
│   ├─ SelectItem  expr → Function{ fn: COUNT, arg: nullptr }     COUNT(*)
│   └─ SelectItem  expr → Function{ fn: AVG,
│                                    arg → ColumnRef{ "", "age" } }
├─ from       TableRef{ name: "person", alias: "" }
├─ joins
│   └─ JoinClause
│       ├─ table  TableRef{ name: "car", alias: "" }
│       └─ on     Binary{ op: Eq,
│                         left  → ColumnRef{ "", "pid" },
│                         right → ColumnRef{ "", "owner_id" } }
├─ where      Binary{ op: Ge,
│                      left  → ColumnRef{ "", "age" },
│                      right → Literal{ int 18 } }
├─ group_by   [ ColumnRef{ "", "city" } ]
├─ order_by   [ OrderItem{ expr → ColumnRef{ "", "city" },
│                           descending: false } ]
└─ limit      5   (has_limit: true)
```

Precedence is visible in the nesting. `NOT` binds tighter than `AND`,
and a comparison binds tighter than both, so one clause line parses to
one shape with no parentheses needed:

```text
NOT city = 'PR' AND age >= 18

            Binary{ And }
           ┌─────┴────────┐
      Unary{ Not }     Binary{ Ge }
           │           ┌─────┴─────┐
      Binary{ Eq }  ColumnRef   Literal
      ┌─────┴────┐    "age"       18
 ColumnRef   Literal
   "city"     'PR'
```

### Parser and entry point

One function per grammar rule, precedence climbing exactly as the
expression grammar above lays out; each rule consumes tokens through a
small cursor (`peek`, `accept`, `expect` — `expect` produces the named
error, e.g. `line 1: expected ')'`). Keywords terminate rules: an
expression stops at `FROM`, `WHERE`, `GROUP`, … naturally because those
are keywords, not identifiers.

Entry point — the trivial API, shaped to the repo's error convention:

```cpp
struct ParseResult {
    std::vector<std::unique_ptr<Statement>> statements;
};

bool parse(const std::string& query, ParseResult& result, std::string& error);
```

- `result.statements` is cleared first; a failed parse leaves it empty.
- Multiple statements separated by `;` are all parsed; empty statements
  between semicolons are ignored; a trailing `;` is optional.
- The first error wins: parsing stops, `error` carries `line L: …`.
- Structural (catalog-free) semantic checks run after a successful parse
  and belong to ex12: unknown function names, `a < b < c`, and the
  aggregate/grouping consistency rule — in a SELECT, every non-aggregate
  select item and every ORDER BY item must appear verbatim in GROUP BY
  when GROUP BY is present. *Column-existence* checks need the catalog
  and wait for ex13's binder; the front-end accepts `SELECT nope FROM t`
  by design.

### AstPrinter

Renders an AST back to normalized SQL (keywords upper, single spaces,
`AS` always explicit). Two jobs: the demo binary prints ASTs for manual
inspection, and the check suite round-trips — parse, print, re-parse,
assert the two ASTs equal (a hand-written `operator==` per node).
Round-trip equality is the cheapest high-yield AST check there is.

### The check suite (ex12 binary)

Same shape as ex09–ex11: `check(name, condition)` lines, exit 0 iff all
pass. Target ~200 checks:

| Area | ~count | Covers |
|---|---|---|
| Lexer | 35 | token streams for every token kind, keyword case, `''` escape, comments, number/string/char errors, line numbers |
| Expressions | 40 | precedence and associativity tables, parens, unary minus, NOT, one-comparison rule, function calls, `COUNT(*)` |
| Statements | 70 | every statement's happy path asserted field-by-field, plus its usage errors (missing VALUES, bad type, trailing garbage) |
| Round-trips | 25 | parse → print → parse → equal, across all six statement kinds |
| Semantics | 20 | aggregate/group-by consistency, unknown function, structural errors |
| API contract | 10 | multi-statement strings, empty input, error-is-value semantics (cleared result, first-error-wins) |

A few checks parse `CREATE TABLE` texts mirroring `tablecatalog/
person.json` and `car.json` and assert the column names/types/sizes —
the first tie-back to the real catalogs.

### Milestones

1. **Lexer** — `Token`/`Lexer` + lexer checks. Done when the token
   stream of a full SELECT is asserted end to end.
2. **Expressions** — `Ast.h` expression half + expression parsing +
   printer. Done when precedence/associativity checks pass.
3. **Simple statements** — CREATE/DROP/INSERT + their errors.
4. **UPDATE/DELETE** — SET clauses, WHERE plumbing.
5. **SELECT** — items, FROM/alias, then JOIN, GROUP BY, ORDER BY, LIMIT,
   and the semantic checks.
6. **Polish** — round-trip suite, `ex12_demo`, README + EXPLAIN.

## ex13 — the executor (sketch)

A binder + a row-at-a-time execution engine over the ex11 stack, which
ex13 copies in (`file/`, `bufpool/`, `policy/`, `heapfile/`, `schema/`,
`tuple/`). The session shape is ex11_cli's: one `BufferManager` (LRU, 8
frames), catalogs cached by name, files opened on first use and flushed
once at quit — this time driven by SQL instead of positional commands.

- **Binder.** Walks each statement's expressions against the target
  tables' `TableSchema`: resolves column refs by name
  (`TableSchema::column_index`), checks literal types against column
  types, resolves `t.c` qualifications across the FROM/JOIN tables, and
  produces a plan the executor can run without touching the AST again.
- **CREATE TABLE** — builds the catalog JSON from the AST's
  `ColumnDef`s and writes `tablecatalog/<name>.json`. Needs a small JSON
  *writer* — ex11 only parses JSON, so ex13 adds one (~100 lines,
  writer-only, no general JSON). Refuses a table that already exists.
- **DROP TABLE** — refuses while the table's file is open, deletes the
  catalog entry and the data file (`HeapFile::erase_file`).
- **INSERT** — literal rows → `make_tuple()` + fill → `insert`; reports
  rows the schema refuses (too-wide string) and continues.
- **UPDATE / DELETE** — scan the target table, evaluate WHERE per row
  against *that table's columns only* (the stated scope — no join terms
  in these predicates), collect matching `RowId`s first, then apply
  `update`/`erase` — collect-then-apply because the scan iterator must
  not see mutation.
- **SELECT (single table)** — scan + filter (WHERE) + project (select
  items). Star maps to all columns in position order.
- **JOIN** — inner nested-loop join over the two sides of each JOIN
  clause in FROM order, ON evaluated over the combined row; three-table
  queries chain left-deep. No indexes exist, so this is the honest plan —
  the simplicity-over-performance stance is explicit.
- **GROUP BY / aggregates** — hash aggregation: key = the group-by
  column values, accumulators per aggregate select item; a bare
  aggregate is one global group. Output rows hold the computed values.
- **ORDER BY / LIMIT** — materialize result rows, `std::sort` with a
  comparator per order item (ASC default), then LIMIT.
- **CLI** — `ex13_cli`, a REPL where each line is one SQL statement
  (`quit` still ends the session); the ex11 commands disappear. Output
  formatting reuses ex11's `print_tuple` pattern for result rows.

ex13's check suite runs statements against real catalog tables and heap
files and asserts on returned rows — the parser's ~200 AST checks become
the executor's end-to-end checks.

## Open items

- Confirm `HAVING` stays out (upgrade path: a per-group filter between
  aggregation and projection).
- Decide whether `t.*` and `OFFSET` are worth their small cost; both are
  additive and can land later without format changes.
- Type affinity in comparisons (`age > '30'`): binder error (strict) is
  the default stance — matches the repo's named-error style.
