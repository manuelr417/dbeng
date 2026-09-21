# ex12 — the SQL front-end

A hand-rolled SQL front-end for the ex11 engine: a lexer, a recursive
descent parser, and the AST they build. `parse()` turns a statement
string into a tree of plain structs under `namespace sql`; nothing here
knows about `HeapFile` or the catalog — the front-end is a
self-contained layer over the engine.

Plain C++20, zero external dependencies. Errors are values: a `bool`
return plus an `error` out-parameter carrying `"line L: message"`;
parse failures never throw.

## The SQL subset

One catalog's worth of statements, keywords case-insensitive,
identifiers case-sensitive:

```sql
CREATE TABLE person (pid INTEGER, name VARCHAR(10), age INT, city VARCHAR(3));
DROP TABLE person;
INSERT INTO person VALUES (1, 'Ruth', 42, 'PR'), (2, 'Amy', 30, 'MA');
UPDATE person SET age = 31, city = 'MA' WHERE pid = 2;
DELETE FROM person WHERE age > 40;
SELECT city, COUNT(*) FROM person GROUP BY city ORDER BY city DESC LIMIT 5;
SELECT p.pid, c.brand FROM person AS p JOIN car AS c ON p.pid = c.owner_id;
```

Expressions: `AND`/`OR`/`NOT`, one comparison per level (`a < b < c` is
a parse error), `+ - * / %`, unary minus, parentheses, and the
aggregates `COUNT/SUM/AVG/MIN/MAX` (`COUNT(*)` included). Unknown
function names are parse errors. Non-goals: subqueries, `HAVING`,
`DISTINCT`, `OFFSET`, outer joins, `INSERT … SELECT`, arithmetic in
`VALUES`, `NULL`.

## Layout

```text
ex12/
  CMakeLists.txt       ex12core static lib + ex12 (checks) + ex12_demo
  main.cpp             the check suite (263 PASS/FAIL checks)
  ex12_demo.cpp        ex12_demo: type a statement, see its AST drawn
  sql/                 the front-end layer
    Token.h            TokenType, Token, the keyword table
    Lexer.h / .cpp     scanner: query text -> token stream
    Ast.h              expression + statement nodes, operator== per node
    Parser.h / .cpp    one function per grammar rule + sql::parse
    AstPrinter.h/.cpp  AST -> normalized SQL (debug printing + round-trips)
    AstDraw.h / .cpp   AST -> terminal drawing (the demo's tree view)
  EXPLAIN.md           how it works, module by module
```

## Build & run

```bash
cmake -S ex12 -B ex12/build
cmake --build ex12/build
./ex12/build/ex12        # ALL CHECKS PASSED, exit 0 iff every check passes
./ex12/build/ex12_demo   # REPL: draws each parsed AST top-down
```

`quit` (or end of input) ends the demo session. Parse errors print
`error: line L: ...` and the session continues. A successful statement
draws as a statement outline with its expressions rendered top-down
under the clauses that own them — the tree a textbook draws by hand,
generated from the live parse:

```bash
echo "SELECT city, COUNT(*) FROM person GROUP BY city LIMIT 5" | ./ex12/build/ex12_demo
```

```text
SelectStatement
├─ items
│   ├─ SelectItem[0]  ColumnRef{ city }
│   └─ SelectItem[1]  Function{ COUNT(*) }
├─ from  TableRef{ person }
├─ group_by  city
└─ limit  5
```
