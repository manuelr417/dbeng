# demo.md — plan: `ex12_demo`, the AST visualizer

ex12 already ships a demo binary that echoes each parsed statement as
normalized SQL. This plan upgrades it into the second artifact of the
exercise: a small REPL that takes a SQL statement, validates it through
the real front-end (`sql::parse`), and on success prints a **drawing of
the AST** — the tree a student sees in sql.md's worked examples, now
produced from the live parse. The goal is visual: precedence, clause
order, and ownership (which expression hangs off which clause) should
be readable at a glance.

Nothing else changes: `AstPrinter` keeps its job (normalized SQL for
round-trips), the check suite keeps its job, ex13 is untouched. This
document is the plan only — no code yet.

## Session contract

Same shape as the current demo, one behavior changed:

- One line of stdin = one input string; a line may hold several
  `;`-separated statements (each is drawn in turn). `quit` or end of
  input ends the session. Blank lines draw nothing.
- On success, each parsed statement is drawn and followed by one blank
  line. With several statements on a line, the drawings appear in
  order, separated by blank lines.
- On failure, print `error: line L: message` (the parser's own
  error-is-value message) and continue the session — a bad statement
  never kills the REPL.
- **Behavior change:** the normalized-SQL echo goes away. The drawing
  is the output. (AstPrinter stays in the library and the suite; it is
  just no longer what the demo prints.)

The demo stays line-oriented: multi-line statements, file input, and
history remain out of scope (ex13's REPL has the same shape).

## Two rendering layers

The output combines the two styles sql.md already establishes:

1. **Statements render as an outline** (worked example 1 style): the
   root node on the first line, then one `├─`/`└─` branch per populated
   clause, in grammar order — items, from, joins, where, group_by,
   order_by, limit. Clauses that are absent from the statement are
   omitted entirely: the drawing shows what was written, nothing more.
2. **Expressions render top-down** (worked example 2 style): root
   label, branch row, children beneath — recursively. Precedence is
   visible as nesting depth, exactly as in sql.md's `NOT city = 'PR'
   AND age >= 18` drawing.

## Expression drawings

Node labels, one line each:

- `Binary{ Ge }`, `Binary{ And }`, `Binary{ Plus }`, … — the operator
  named as in the AST (`Eq, Ne, Lt, Le, Gt, Ge, And, Or, Plus, Minus,
  Mul, Div, Mod`), the tree's vocabulary rather than SQL punctuation.
- `Unary{ Not }`, `Unary{ Neg }`.
- `Function{ COUNT(*) }` when the argument is the star; otherwise
  `Function{ SUM }` with the argument drawn as a child.
- **Leaves are compact one-liners**: `Literal{ 18 }`,
  `Literal{ 3.14 }`, `Literal{ 'PR' }`, `ColumnRef{ age }`,
  `ColumnRef{ p.age }`. A leaf has nothing to branch to, so a drawn
  stub would be noise; sql.md's field-complete dump style
  (`ColumnRef{ table: "", column: "city" }`) stays where it belongs —
  in the docs.

Assembly rules:

- **Binary:** children drawn side by side with a two-space gap; a
  branch row underneath the parent label uses `┌` at the left child's
  center, `┴` at the parent's center, `┐` at the right child's center,
  dashes between. The parent label is centered over the branch row; if
  the label is wider than the children span, the gap grows until it
  is not.
- **Unary / Function-with-arg:** one child, centered; a single `│`
  stub row joins parent to child (parent centered over the child
  block).
- **Leaf:** just the one-line label.

Box-drawing characters (`├ ─ │ └ ┌ ┴ ┐`) match sql.md's examples; the
repo's terminals are UTF-8, no ASCII fallback is planned.

## Statement outlines

Formats per kind, fixed now so the implementer and the checks agree.
Concretely, per clause:

- `items` — one line per item: `SelectItem[k]`, then the item's
  expression (inline if it is a leaf, drawn as a block otherwise);
  ` AS alias` appended to the item line when aliased; a lone `*` item
  renders as `SelectItem[0]  *`.
- `from` / join `table` — inline: `TableRef{ person }`, alias included
  when present: `TableRef{ person AS p }`.
- `joins` — per join: a `JoinClause  TableRef{ car }` line, then the
  ON expression drawn as a block beneath.
- `where`, join `on`, DELETE `where`, UPDATE `where` — expression
  block beneath the clause line (inline right on the line when the
  expression is a leaf).
- `group_by` / `order_by` — these hold column refs only (per the
  grammar), so they render inline and comma-separated:
  `group_by  city, age` and `order_by  age DESC, name`.
- `limit` — inline: `limit  5`.
- `sets` (UPDATE) — per clause: `SetClause{ age }` (or inline
  `SetClause{ city } = Literal{ 'PR' }` when the value is a leaf),
  value expression drawn beneath when composite.
- `columns` (CREATE) — one line per column: `pid  INTEGER(4)`,
  `name  VARCHAR(10)`, `x  DOUBLE(8)` — name, normalized type, size.
- `rows` (INSERT) — one line per row: `row[0]  Literal{ 1 },
  Literal{ 'Ruth' }, Literal{ 42 }, Literal{ 'PR' }` — VALUES holds
  literals only, so rows are always inline leaf lists.
- `table` (all of CREATE/DROP/INSERT/UPDATE/DELETE) — inline.

**Block nesting and the gutter:** an outline line's continuation gutter
(`│   ` under a `├─`, spaces under a `└─`) prefixes every line of an
expression block drawn beneath it, so sibling branches stay aligned no
matter how wide the expression grows. Expression blocks are indented a
fixed few columns past their clause label.

## Worked examples (target output)

The sql.md worked example:

```sql
SELECT city, COUNT(*), AVG(age)
FROM person JOIN car ON pid = owner_id
WHERE age >= 18
GROUP BY city
ORDER BY city
LIMIT 5
```

should draw (shape — exact columns come from the algorithm):

```text
SelectStatement
├─ items
│   ├─ SelectItem[0]  ColumnRef{ city }
│   ├─ SelectItem[1]  Function{ COUNT(*) }
│   └─ SelectItem[2]
│         Function{ AVG }
│             │
│         ColumnRef{ age }
├─ from  TableRef{ person }
├─ joins
│   └─ JoinClause  TableRef{ car }
│       └─ on
│              Binary{ Eq }
│              ┌─────┴──────┐
│        ColumnRef{ pid }  ColumnRef{ owner_id }
├─ where
│         Binary{ Ge }
│         ┌─────┴─────┐
│   ColumnRef{ age }  Literal{ 18 }
├─ group_by  city
├─ order_by  city
└─ limit  5
```

The precedence example, as a WHERE clause
(`SELECT name FROM person WHERE NOT city = 'PR' AND age >= 18`):

```text
SelectStatement
├─ items
│   └─ SelectItem[0]  ColumnRef{ name }
├─ from  TableRef{ person }
└─ where
                    Binary{ And }
                   ┌─────┴────────┐
              Unary{ Not }     Binary{ Ge }
                    │           ┌─────┴─────┐
              Binary{ Eq }  ColumnRef{ age }  Literal{ 18 }
              ┌─────┴──────┐
        ColumnRef{ city }  Literal{ 'PR' }
```

NOT hangs off `And`'s left branch, the comparison nests under it —
the same picture as sql.md, generated from the parse.

## Layout algorithm (described, not coded)

The classic width/center tree layout, expressed over the two layers:

1. An expression renders to a **block**: a list of text lines plus the
   column of its root's center. Leaves are a single line whose center
   is the label's middle.
2. A binary node lays its two child blocks side by side (gap ≥ 2),
   builds the branch row from the two child centers, centers the
   parent label over that row (widening the gap when the label is
   wider), then stacks: parent line, branch row, child lines padded to
   the wider child.
3. A unary/function node centers its single child block under the
   parent label and inserts one `│` row.
4. Blocks compose bottom-up; the statement outline then embeds each
   block under its clause line, prefixing every block line with the
   outline gutter.

All string building, no measuring of a real terminal; width limits are
a non-goal (below).

## Code layout

```text
sql/AstDraw.h / .cpp   sql::draw(const Statement&) -> std::string
                       (and the expression-block machinery, file-local)
ex12_demo.cpp          driver: getline -> sql::parse -> sql::draw -> stdout
CMakeLists.txt         AstDraw.h/.cpp join ex12core's CORE_SOURCES
```

`draw` cannot fail (the tree is already valid), so it returns a string
rather than using the error convention. Namespace stays `sql`; the
module sits beside AstPrinter as the second renderer.

## Check-suite additions

A small new section in `main.cpp` (~10 checks), substring-level — the
suite asserts labels and structure, not pixel columns:

- the worked example's drawing contains `Binary{ And }`-style labels,
  the `┌`/`┴`/`┐` branch row, `│` stubs, and the clause lines in order;
- a leaf-only statement (`SELECT age FROM t`) draws with no branch
  rows at all;
- absent clauses are absent (`SELECT * FROM t` has no `where` line);
- every statement kind draws its headline fields (`table  person`,
  `limit  5`, `row[0]`, `INTEGER(4)`);
- deep nesting stays inside the gutter (no line loses its `│` prefix).

## Non-goals

- No Graphviz/DOT, image, or HTML export — terminal text only.
- No line wrapping or horizontal scrolling for very wide trees;
  course-sized queries fit. (An explicit overflow marker can come
  later without format changes.)
- No multi-line statement input, no file input, no history.
- No color/styling.

## Open items

- Should the demo echo the normalized SQL as a context line above each
  drawing? Cheap and useful for tying input to canonical form, but it
  clutters; default is no.
- `t.*` items and `HAVING` (sql.md open items) would slot into the
  items/group outlines additively if they ever land.

## Milestones

1. **Expression blocks** — `AstDraw` bottom-up block composition, done
   when the two worked-expression drawings match sql.md's shape.
2. **Statement outlines** — all six kinds wired, clause omission and
   gutter embedding included.
3. **Demo + suite + docs** — driver swap (drawing replaces the
   normalized-SQL echo), CMake, the ~10 new checks, README/EXPLAIN
   updates.
