# ex08 — Buffer Pool

A small database-engine exercise that implements the **buffer pool** layer:
a fixed set of frames that queries share, sitting between the on-disk block
storage of ex07 and the buffer manager of ex09. Each frame is one 4096-byte
page plus the bookkeeping the pool needs to manage it:

- `pin_count` — how many queries currently use the frame,
- `dirty` — true if the buffer was modified and not yet saved,
- `buffer[4096]` — the actual data page.

There is no CLI: the single binary runs a PASS/FAIL check suite that
exercises the pool API.

## Building

```bash
cmake -S . -B build
cmake --build build
```

This produces one executable, `build/ex08`.

(CMake ≥ 4.2 is required; CLion builds into `cmake-build-debug/` instead —
both directories are gitignored.)

## Running

```bash
./build/ex08
```

Runs 9 checks, prints one PASS/FAIL line per check, and exits 0 only if
every check passed:

```
PASS: init creates 4 entries
PASS: all entries start with pin_count = 0 and dirty = false
PASS: buffer of entry 2 is accessible and writable
PASS: set_dirty(2, true) turns the flag on
PASS: set_dirty(2, false) turns the flag off
PASS: two pins give pin_count = 2
PASS: one unpin gives pin_count = 1
PASS: second unpin gives pin_count = 0
PASS: accessing entry 4 of a 4-entry pool throws

All checks passed.
```

## Structure

All code is in `namespace buffer_pool` (one namespace per project, per repo
convention). Tests are PASS/FAIL `check()` calls inline in `main.cpp` — no
separate test target (per repo convention for this exercise).

- `DataFrame.h` — one frame: `pin_count`, `dirty`, `char buffer[4096]`
  (`kPageSize`).
- `BufferPool.{h,cpp}` — fixed set of frames with `init`, `frame`,
  `set_dirty`, `pin`, `unpin`, `size`. `frame()` throws `std::out_of_range`
  on an invalid index; `unpin()` throws `std::logic_error` if the pin count
  is already zero. `init(n)` creates n empty frames (pin_count = 0,
  dirty = false, zeroed buffer).
- `main.cpp` — the check suite.

## Where this fits

- ex07 stores `Person` records in fixed 4096-byte blocks on disk — the same
  page size as a frame buffer here.
- ex08 (this exercise) provides the pool of frames with pin counting and
  dirty flags, but no disk I/O and no replacement policy.
- ex09 builds the buffer manager on top: it owns a pool, moves blocks
  between disk and frames, and adds LRU replacement and dirty write-back.
