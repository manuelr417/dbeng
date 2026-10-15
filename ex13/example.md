# example.md — using ex13_cli on a parts table

A verified walkthrough of the session CLI against the classic
parts-supplier table: create, load, index, query both access paths,
and the escape hatches. Every output below is real captured output.

Start the session (from `ex13/`):

```sh
build/ex13_cli
```

## 1. Create the table

Types: `INTEGER`, `VARCHAR(n)`, `DOUBLE`. The catalog lands in
`tablecatalog/parts.json`; the data file is created on demand later.

```sql
CREATE TABLE parts (partid INTEGER, pname VARCHAR(10), pcolor VARCHAR(10),
                    pweight DOUBLE, pprice DOUBLE, pmaterial VARCHAR(20));
```

```text
table 'parts' created
```

## 2. Seed two rows, then bulk-generate the rest

Generated numbering continues after the existing records.

```sql
INSERT INTO parts VALUES (1, 'nut', 'red', 12.5, 2.0, 'steel'),
                         (2, 'bolt', 'blue', 30.0, 7.25, 'brass');
```

```text
inserted 2 row(s)
```

```
bulk parts 498 parts
```

```text
appended 498 generated record(s)
```

## 3. Same query, before and after an index

Watch the access path flip.

```sql
SELECT pname, pcolor, pprice FROM parts WHERE partid = 5;
```

```text
(via heap scan)
pname=nqd, pcolor=hd, pprice=20.8
1 row(s)
```

```sql
CREATE INDEX parts_partid ON parts (partid);
```

```text
index 'parts_partid' created on 'parts(partid)'
```

```sql
SELECT pname, pcolor, pprice FROM parts WHERE partid = 5;
```

```text
(via index scan)
pname=nqd, pcolor=hd, pprice=20.8
1 row(s)
```

## 4. An AND-combined range also takes the index

```sql
SELECT partid, pname FROM parts WHERE partid > 493 AND partid < 498;
```

```text
(via index scan)
partid=494, pname=wzpj
partid=495, pname=pjsqsew
partid=496, pname=cglhtlcqij
partid=497, pname=galepydq
4 row(s)
```

## 5. INSERT after CREATE INDEX keeps the index current

```sql
INSERT INTO parts VALUES (600, 'washer', 'red', 1.5, 0.4, 'copper');
SELECT pname FROM parts WHERE partid = 600;
```

```text
inserted 1 row(s)
(via index scan)
pname=washer
1 row(s)
```

## 6. Path selection

No index on `pcolor`, and a top-level `OR` always falls back to the
heap scan — same answers, different route.

```sql
SELECT pname FROM parts WHERE pcolor = 'red';
```

```text
(via heap scan)
pname=nut
pname=washer
2 row(s)
```

```sql
SELECT pname FROM parts WHERE pcolor = 'red' OR partid = 7;
```

```text
(via heap scan)
pname=nut
pname=qhc
pname=washer
3 row(s)
```

## 7. An alias may rename one projected column

```sql
SELECT pname AS label FROM parts WHERE partid = 5;
```

```text
(via index scan)
label=nqd
1 row(s)
```

## 8. Escape hatches

Bare commands — **no trailing `;`** (a trailing semicolon becomes part
of the last argument). Data pages start at block 1; block 0 is the
header page and never stores records.

```
scan demo demo
```

```text
(1,0)  id=1, tag=alpha
(1,1)  id=2, tag=beta
(1,2)  id=3, tag=gamma
(1,3)  id=4, tag=delta
scanned 4 record(s)
```

```
read demo demo 1 0
```

```text
(1,0)  (pool hit)  id=1, tag=alpha
```

```
seek demo demo 1 2 1 3
```

```text
(1,2)  id=3, tag=gamma
(1,3)  id=4, tag=delta
```

```
read parts parts 1 0
```

```text
(1,0)  partid=1, pname=nut, pcolor=red, pweight=12.5, pprice=2, pmaterial=steel
```

```
checkindex parts parts
```

```text
parts_partid: 501 entries, OK
```

(The `demo` snippets use a tiny throwaway table:
`CREATE TABLE demo (id INTEGER, tag VARCHAR(6));` with four rows.)

## 9. Refusals are named, not silent

```sql
SELECT pname FROM parts ORDER BY pname;
```

```text
ORDER BY is not supported in ex13
```

## 10. Leave

```
quit
```

## Gotchas

- Escape-hatch lines take no semicolon.
- Page 0 is the header page; data starts at `(1, 0)`.
- Generated rows are deterministic (seed 42) but their strings are
  random letters, so your `pname`/`pcolor` values will differ from the
  ones above in fresh tables.
