# EXPLAIN.md — how ex12 works, module by module

ex12 is the SQL front-end: text in, AST out. The AST — abstract syntax
tree — is the parse of a statement as a tree of plain structs: one node
per statement kind at the root, its clauses and sub-clauses beneath,
and one node per operator, operand, and column/literal reference in
every expression. `sql::parse()` builds it; the printer and the
drawing render it; nothing here executes it. Its subject is the step
from positional commands (`read person people.bin 1 0`) to a language a
human can write. Where the engine stands: ex07 serializes Person
records into blocks, ex10 adds a slotted-page heap file addressable by
row id, ex11 makes rows schema-driven (a JSON catalog per table, a
buffer manager, a session CLI over positional commands) — and ex12
puts a language on top. The module is deliberately self-contained — it
imports nothing from ex11: tokens and trees here, tables and rows
there.

Two binaries come out of the exercise: `ex12`, the check suite (exit 0
iff every check passes), and `ex12_demo`, a REPL that parses a
statement and prints its AST as a terminal drawing. Build and run
commands live in README.md.

The pipeline is the classic three-stage shape:

```text
query text ──tokenize──> token stream ──parse──> AST ──print──> normalized SQL
   sql/Lexer.cpp            sql/Token.h        sql/Parser.cpp   sql/AstPrinter.cpp
                                                 (sql::parse)          │
                                                                       └──draw──> tree
                                                                              sql/AstDraw.cpp
```

Every stage reports failures the same way the ex11 catalog loader does:
a `bool` return plus an `error` out-parameter cleared on entry, carrying
`"line L: message"`. Nothing throws.

## The SQL it accepts

One catalog's worth of statements — keywords case-insensitive,
identifiers case-sensitive:

```sql
CREATE TABLE person (pid INTEGER, name VARCHAR(10), age INT, city VARCHAR(3));
DROP TABLE person;
INSERT INTO person VALUES (1, 'Ruth', 42, 'PR'), (2, 'Amy', 30, 'MA');
UPDATE person SET age = 31, city = 'MA' WHERE pid = 2;
DELETE FROM person WHERE age > 40;
SELECT city, COUNT(*), AVG(age)                  -- joins, aliases,
FROM person JOIN car ON pid = owner_id           -- GROUP BY, ORDER BY,
WHERE age >= 18 GROUP BY city ORDER BY city LIMIT 5;   -- LIMIT
```

Expressions: `AND`/`OR`/`NOT`, one comparison per level (`a < b < c` is
a parse error, not a silent reinterpretation), `+ - * / %`, unary
minus, parentheses, and the aggregates `COUNT/SUM/AVG/MIN/MAX`
(`COUNT(*)` included; unknown function names are parse errors).
Precedence, low to high: `OR` → `AND` → `NOT` → comparison → `+ -` →
`* / %` → unary minus → primaries.

Deliberately out (each with a clean upgrade path): subqueries, `HAVING`,
`DISTINCT`, `OFFSET`, outer joins, `t.*`, `INSERT … SELECT`, arithmetic
in `VALUES`, and `NULL` — ex11 tuples have no nulls and the plan keeps
it that way. `AS` in FROM is supported (self-joins need it).

## One statement, end to end

The input

```sql
SELECT city, COUNT(*) FROM person WHERE age >= 18 GROUP BY city LIMIT 5
```

parses to a `SelectStatement` (items, from, where, group_by, limit) and
the demo draws it:

```text
SelectStatement
├─ items
│   ├─ SelectItem[0]  ColumnRef{ city }
│   └─ SelectItem[1]  Function{ COUNT(*) }
├─ from  TableRef{ person }
├─ where
│                   Binary{ Ge }
│                ┌───────┴────────┐
│         ColumnRef{ age }  Literal{ 18 }
├─ group_by  city
└─ limit  5
```

The same tree also renders as normalized SQL through `AstPrinter` —
`select 1+2*3 from t` comes back as `SELECT 1 + 2 * 3 FROM t`, `int`
as `INTEGER`, `<>` for `!=` — and the suite round-trips every statement
kind through that text to prove print and parse are inverses.

## Token.h — the vocabulary

One enum, `TokenType`, holds everything: the 28 keywords of the subset,
the four literal shapes (`Identifier`, `IntLiteral`, `DoubleLiteral`,
`StringLiteral`), punctuation, the ten operators, and `End`. A single
enum keeps the parser's switch flat — there is no keyword sub-tag to
consult, and "is this token `FROM`" is a one-comparison test. The
keyword table is a static array scanned linearly (28 entries) with
case-insensitive matching, so `select ≡ SELECT`; the token's `text`
keeps whatever spelling the source used. `Token` itself is three fields:
type, lexeme, and the 1-based line it starts on — the line is the only
position information the whole front-end carries, which is exactly what
the error messages need.

## Lexer — the scanner

`tokenize(source, tokens, error)` runs a cursor over the text and
appends to the token vector, finishing with an `End` token (so the
parser never has to bounds-check). Rules, in scan order:

- Whitespace is skipped; `\n` bumps the line counter.
- `--` starts a line comment, skipped to end of line.
- `[A-Za-z_][A-Za-z0-9_]*` is a word: keyword table first
  (case-insensitive), otherwise `Identifier`. Function names
  (`COUNT`, `SUM`, …) are deliberately **not** keywords — a bare
  `count` lexes as an identifier, and the parser only treats it as a
  function when `(` follows.
- Digits start a number: digits, optional fraction. `3.` (no fraction
  digits) and `1.2.3` (second dot) are errors reporting the full
  malformed run: `line 1: malformed number '1.2.3'`.
- `'...'` is a string; `''` inside escapes to one quote. The token text
  is the unescaped value. An unterminated string is reported on the
  line where the opening quote sits.
- Anything else: `line L: unexpected character 'x'`.

Numbers keep their spelling in `text` (`007` stays `007`); conversion
to `int64`/`double` happens in the parser, where out-of-range literals
become named errors instead of silent clamping.

## Ast.h — the tree

Six tagged statements share a `Statement` base (`Kind` + virtual
destructor; each derived type pins its `Kind` in its constructor, so a
forgotten tag is impossible). Expressions are one tagged struct, `Expr`,
with a payload per kind — Literal, ColumnRef, Binary, Unary, Function —
five kinds in total, few enough that a class hierarchy per kind would
be ceremony. The payload fields are only meaningful for
their kind; every tree walk switches on `kind` and never guesses.

`DataType` mirrors the catalog one-for-one (`Integer`, `String`,
`Double`), and `ColumnDef` carries `size` for `VARCHAR(n)`. The parser
gives `INTEGER` width 4 and `DOUBLE` width 8, so a parsed
`CREATE TABLE person (...)` is field-for-field the content of
`tablecatalog/person.json` — a CREATE TABLE executor could write that
JSON straight from the AST, no translation needed.

`VARCHAR(n)` is a **maximum** width, not variable-length storage —
the fixed-size record assumption holds. ex11's `ColumnSchema` already
documents a String's size as its maximum character count, the record
codec reserves exactly n bytes per record (zero-padding shorter
values, refusing longer ones with a named error), so `VARCHAR(10)`
occupies the same slot in every row. Storage-wise it behaves like
`CHAR(n)` with padding; nothing downstream changes.

Hand-written `operator==` per node makes whole trees comparable. It
exists for the round-trip checks, and any later consumer of the AST
(an executor, more tests) can reuse it.

## Parser — recursive descent

`sql::parse(query, result, error)` tokenizes, then drives a cursor
(`peek` / `accept` / `expect`) through one function per grammar rule:
`parse_statement` dispatches on the leading keyword; expression
precedence climbs exactly as the grammar lays out — `or` → `and` →
`not` → `comparison` → `additive` → `multiplicative` → `unary` →
`primary`. Keywords terminate rules naturally: an expression stops at
`FROM`, `WHERE`, `GROUP`, … because those are keyword tokens, not
identifiers.

Details worth writing down:

- **One comparison per level.** `parse_comparison` parses exactly one
  operator; if a second follows, it fails with
  `comparison operators cannot be chained` rather than reassociating.
- **Unknown functions fail at the call site**
  (`line 1: unknown function 'FOO'`), and `*` is only accepted inside
  `COUNT(*)` (`'*' is only allowed in COUNT(*)`).
- **Errors are positional.** `fail()` stamps the current token's line;
  the grouping check stamps the SELECT token's line (expressions carry
  no line — the statement position is enough to find the clause).
- **Structural semantics, catalog-free.** After a SELECT parses, the
  aggregate/grouping rule runs: with GROUP BY present, every select
  item must be verbatim in GROUP BY or be an aggregate;
  every ORDER BY item must be verbatim in GROUP BY (an order item is a
  column ref with an optional direction by the grammar, so
  `ORDER BY COUNT(*)` cannot be written). Column existence
  is *not* checked: `SELECT nope FROM t` parses, by design — column
  resolution needs the catalog, which the front-end does not have.
- **Statement lists.** `;`-separated statements all parse; empty
  statements between semicolons are ignored; the trailing `;` is
  optional; the first error wins and leaves `result.statements` empty.

## AstPrinter — the AST as text

`print(statement)` renders normalized SQL: keywords upper, single
spaces, `AS` always explicit, `ASC` implicit, `<>` for not-equal, `INT`
normalized to `INTEGER`. Parenthesization is minimal but exact — a
child is parenthesized precisely when flat text would re-parse to a
different tree: lower precedence always, and equal precedence on the
right (the parser folds left, so `(a + b) + c` prints bare but
`a + (b + c)` keeps its parens). Two special cases guard re-lexing:
doubles print via shortest-round-trip `to_chars` with a forced `.0`
when needed (so `12.0` does not come back as an integer), and `- -3`
keeps its space (a bare `--3` would lex as a comment).

The printer serves the round-trip checks: parse → print → re-parse →
`operator==` — the cheapest high-yield check the suite runs, across all
six statement kinds. (The demo binary no longer prints this text; it
draws, below.)

## sql/AstDraw — the tree renderer

`draw(statement)` produces the tree picture shown in "One statement,
end to end": the
statement as a `├─`/`└─` outline — one branch per populated clause, in
grammar order, absent clauses omitted — with every composite
expression drawn top-down under the clause that owns it (`Binary{ Ge }`
labels, `┌─┴─┐` branch rows, `│` stubs) and leaves joining their clause
line as compact one-liners (`Literal{ 18 }`, `ColumnRef{ p.age }`).
Expressions compose bottom-up into blocks of single-column cells, so
the multi-byte box glyphs never skew the column math; the classic
width/center algorithm positions parents over branch rows and the
outline embeds each block under its clause with the branch gutter
prefixed. The demo binary drives it; a substring-level suite section
checks labels, glyph structure, clause order, and gutter integrity.

## The check suite (the ex12 binary)

Same shape as ex09–ex11: `check(condition, message)` lines, exit 0 iff
all pass. 263 checks:

| Area | count | Covers |
|---|---|---|
| Lexer | 35 | token streams for every kind, keyword case, `''` escape, comments, number/string/char errors, line numbers |
| Expressions | 52 | precedence and associativity tables, parens, unary minus, NOT, comparisons, aggregates, `COUNT(*)`, chained-comparison and unknown-function errors |
| Statements | 99 | every statement's happy path field-by-field (CREATE mirrors `person.json`/`car.json`), plus its usage errors |
| Round-trips | 29 | parse → print → parse → equal, all six statement kinds |
| Semantics | 18 | aggregate/group-by consistency, unknown function, chained comparison, first-error-wins |
| API contract | 13 | multi-statement strings, empty input, cleared results, error-is-value semantics |
| AstDraw | 17 | node labels, box-glyph branch rows, clause order, gutter integrity, all six statement kinds' headline fields |

Test scaffolding note: `ok_parse` keeps the parsed statement alive in a
suite-level holder, because typed views and expression pointers point
into that tree; the suite consumes one statement at a time, so each
new parse safely replaces the previous one.

## Milestones and how this was built

The build landed in six milestones, in order — lexer, expressions,
simple statements, UPDATE/DELETE, SELECT + semantics, polish — with the
suite growing one section per milestone. Three bugs surfaced along the
way, all instructive: a missing `#include <string>` (headers should
include what they use), dangling test pointers when a helper returned
a pointer into a destroyed `unique_ptr` (the reason for the holder
above), and `ORDER BY COUNT(*)` — allowed by an early draft of the
grouping rule but not writable under the grammar; the grammar won.
