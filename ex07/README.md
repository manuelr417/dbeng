# ex07 — Binary Person Block Storage

A small command-line program that stores `Person` records in a binary file,
packed into fixed-size 4096-byte blocks. See [`docs/explain.md`](docs/explain.md)
for how the storage format and code are structured; this file just covers
building and running the `ex07` executable.

## Building

```bash
cmake -S . -B build
cmake --build build
```

This produces two executables in `build/`:

- `ex07` — the command-line program described below.
- `ex07_tests` — an assert-based test suite (`./build/ex07_tests`).

## Usage

```
ex07 append <csv-file> <binary-file>
ex07 read <binary-file> <block-number>
ex07 bulk <count> <binary-file>
```

### `append` — load CSV records into the binary file

```bash
./build/ex07 append sample.csv people.bin
```

Reads rows from `<csv-file>`, validates each one, and appends the valid
records to `<binary-file>` (creating it if it doesn't exist). Records are
packed into 4096-byte blocks; if the file already ends with a partially
filled block, new records fill it before a new block is started.

Invalid rows are skipped, not fatal — parsing continues to the end of the
file. Each skipped row is reported to stderr with its line number and reason,
and the final line of output reports how many records were appended and how
many rows were skipped:

```
line 3: pid must be a positive integer
appended 2 record(s), skipped 1 row(s)
```

#### CSV format

Each row must have exactly four comma-separated fields:

```
pid,name,age,city
```

```
1,Ana,20,PR
2,Bob,31,NY
```

A row is rejected if:

- it doesn't have exactly four fields,
- `pid` is not a positive integer,
- `age` is not a nonnegative integer,
- `name` is longer than 9 characters,
- `city` is longer than 2 characters.

### `read` — inspect one block of the binary file

```bash
./build/ex07 read people.bin 0
```

Reads block `<block-number>` (0-indexed) from `<binary-file>` and prints each
populated record in it:

```
block 0: 2 record(s)
  slot 0: pid=1, name=Ana, age=20, city=PR
  slot 1: pid=2, name=Bob, age=31, city=NY
```

Requesting a block number at or beyond the end of the file is an error.

### `bulk` — generate and append n synthetic records

```bash
./build/ex07 bulk 4000 people.bin
```

Generates `<count>` synthetic `Person` records and appends them to
`<binary-file>` (creating it if it doesn't exist), the same way `append`
does — no CSV file is needed. This is useful for quickly populating a large
file to exercise block-boundary behavior.

Generated records have sequential pids, a name derived from the pid (e.g.
`P4001`), a deterministic age, and a city code cycled from a small fixed
list. Pids continue from the file's existing record count, so running `bulk`
multiple times against the same file does not reuse pids:

```
appended 4000 generated record(s)
```

## Example session

```bash
cmake -S . -B build
cmake --build build
./build/ex07 append sample.csv people.bin
./build/ex07 read people.bin 0
```
