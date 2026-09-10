# demo.md — second executable: ex07-style CLI over the ex09 stack

Status: implemented and verified — stdout, stderr, exit codes and the
resulting binary files are byte-identical to `../ex07/build/ex07` across all
four commands and the error paths.

## Goal

A second executable (`ex09_demo.cpp`) with **exactly the CLI behavior of
ex07's `main.cpp`**: same four commands, same arguments, same stdout/stderr
strings, same exit codes — but running on the ex09 stack (block file +
buffer pool + buffer manager), with **all** data access going through the
pool, including appends.

## CLI (identical to ex07)

```
Usage:
  ex09_demo append <csv-file> <binary-file>
  ex09_demo read <binary-file> <block-number>
  ex09_demo seek <binary-file> <block-number>...
  ex09_demo scan <binary-file>
  ex09_demo bulk <count> <binary-file>
```

- `append` — load CSV rows (invalid rows skipped, `line N: reason` on
  stderr), append valid records, print
  `appended N record(s), skipped M row(s)`; empty-but-clean CSV exits 0,
  any skipped row exits 1 (ex07 semantics).
- `read` — print `block N: M record(s)` plus
  `  slot i: pid=…, name=…, age=…, city=…` lines; block beyond EOF → error,
  exit 1.
- `scan` — per-block output as `read`, then
  `scanned B block(s), R record(s)`; empty file scans as zero blocks.
- `bulk` — generate `count` synthetic records with pids continuing from the
  file's existing record count, print `appended N generated record(s)`.
- Wrong arguments → usage on stderr, exit 1. All I/O errors → error string
  on stderr, exit 1.

## Per-command implementation mapping

| Command | Path taken in ex09 |
|---|---|
| `append` | `bufman::load` (CSV) → `append_people`: `pin` the last block, fill its free slots with `put_record`, then `alloc_page` one zeroed page after another for the rest; every touched frame unpinned dirty; `close_all` flushes before returning |
| `read` | `open_file_read` + `pin(file, block)` → `deserialize_block(frame.buffer)` → print → `unpin` |
| `scan` | `open_file_read` → `bufman::block_count` → per block: `pin` → `deserialize_block(frame.buffer)` → print → `unpin` |
| `bulk` | `bufman::record_count` → `bufman::generate` → same `append_people` path as `append` |

Design notes (revised after the "do real DBs route appends through the
pool?" discussion — they do, via a new-page operation, so ex09 got one):

- **`BufferManager::alloc_page(file, block_number)`** is the new-page path:
  victim frame via the usual free-first/policy rules, buffer zeroed, page
  table entry installed, frame marked dirty and pinned. Refuses blocks that
  are already resident or already on disk (checked via `block_count`).
- **Appends go through the pool** like in a real DBMS: the last partial
  block is pinned and filled, further records land in freshly allocated
  pages. `pin` past EOF still fails — `alloc_page` is the only way a block
  comes into existence.
- **`open_file_read`** wraps `open_for_read` so `read`/`scan` do not create
  missing files (stdout/stderr parity with ex07's O_RDONLY behavior).
- `append_people` is shared by `append` and `bulk`; the eviction churn (one
  frame per new page in a pool-of-one, many frames in the demo's pool of 8)
  is what performs the actual disk writes.
- Pure-port alternative (BlockFile only, no pool) rejected: it would bypass
  everything ex09 adds and make the demo binary redundant with ex07's.

## Details

- **Serialization buffer note**: moot after the revision — records are
  written record-by-record into frame buffers via `put_record`; no staging
  buffer is needed anymore.
- **Namespaces**: everything is `bufman` already; `ex09_demo.cpp` needs no
  new module. Includes: BlockFile, PersonCsv, PersonGenerator,
  PersonSerializer, BufferManager, LRUPolicy.
- **Output parity**: `print_person` prints `person.name` / `person.city` as
  C strings, exactly like ex07's main.cpp.
- **`main.cpp` untouched** — it remains the check-suite binary.
- **CMakeLists.txt**: add a second target sharing the module sources:
  `add_executable(ex09_demo ex09_demo.cpp <same source list>)` — source file
  and executable share the name `ex09_demo`, matching the repo's `exNN`
  target convention. `main.cpp` is NOT in that target.
- **README.md**: build section gains the second artifact and its run line.

## Files touched

- `ex09_demo.cpp` (new — the only real code)
- `CMakeLists.txt` (one added `add_executable` line)
- `README.md` (document the second artifact)

## Verification (all passing)

1. Both targets build clean; `./build/ex09` check suite passes (72 checks,
   including the alloc_page cases).
2. Functional pass: `append` (valid + invalid rows, diagnostics + counts),
   `read 0`, partial-block `read`, `scan`, `bulk` twice (pid continuation),
   `read` beyond EOF, bad usage, missing file.
3. **Diff parity vs `../ex07/build/ex07`**: identical stdout, stderr and exit
   codes for `append`, `read 0`, `bulk 4000`+`bulk 50`, `scan`, and the
   error paths (missing file for read/scan, usage, bad block number).
4. **Byte parity**: `cmp` of the produced binary files is identical per
   command — the pool-based write path lands exactly the same bytes on disk
   as ex07's direct `append_records`.

## `seek` — read a list of blocks (revision 2, implemented)

New command, syntax:

```
ex09_demo seek <binary-file> <block-number>...
example: ex09_demo seek person1.bin 0 2 0 1
```

Implemented as planned, with the approved decisions:

- Valid blocks print exactly what `read` prints, **plus a `(pool hit)`
  marker** when the page table already held the block (made possible by a
  small const query, `BufferManager::resident(file, block)`; `pin` itself
  was unchanged). Duplicates in the list make the marker visible: in
  `seek p.bin 0 2 0 1` the second block 0 prints as a hit.
- Failures print `block <arg>: not a nonnegative integer` (unparsable token)
  or `block <n>: <manager error string>` (e.g. beyond EOF) on stderr, and
  the loop continues with the next block.
- Exit code is 1 if the file failed to open or **any** block failed; 0 only
  when every requested block was read.
- Requires at least one block number (`argc >= 4`), otherwise usage; the
  ≤20 bound is an assumption and is not enforced.
- Usage text gained the `seek` line — the usage output now intentionally
  differs from ex07's (the only parity exception, verified per-command
  parity is unaffected).

## Out of scope

New CLI commands, a `create`/`delete` block API beyond `alloc_page`, CSV
export.
