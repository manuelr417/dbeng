# bufman.md — BufferManager implementation plan (ex09)

Status: implemented. `BufferManager`, `ReplacementPolicy`, `LRUPolicy` and the
BlockFile extensions are in place; `main.cpp` runs the check suite below (34
checks, all passing).

## Goal

`BufferManager` sits between queries and disk. It owns a `bufman::BufferPool`
and uses `bufman::BlockFile` to move 4096-byte blocks between files and pool
frames. Data crosses the disk boundary in one direction per path, always
through BlockFile:

- Read: disk → `BlockFile::read_block` → frame buffer
- Write: dirty frame buffer → `BlockFile::write_block` → disk

## Responsibilities

- **Page table**: hash table mapping (file, block number) → frame index.
- **Replacement**: delegated to a **pluggable replacement policy** (abstract
  class; LRU first, MRU/LFU swappable later). Candidates must have
  `pin_count == 0`; frames with no metadata (never used) are treated as free
  and taken before consulting the policy.
- **Write-back**: a dirty victim frame's buffer is written to its block on
  disk before the frame is reused.
- **Pin accounting**: pin on access, unpin on release; pinned frames are
  never evicted.

## New module: `BufferManager.{h,cpp}` (namespace `bufman`)

### State

- `BufferPool pool_` — frame storage (unchanged ex08 module).
- `std::unique_ptr<ReplacementPolicy> policy_` — injected at `init`.
- Page table: `std::unordered_map<Key, std::size_t>` where
  `Key = {int fd; std::uint64_t block_number}` (fd comes from
  `BlockFile::File`).
- Per-frame reverse entry: `std::vector<std::optional<Key>>` — an empty entry
  is the "never used / free frame" signal.
- `std::vector<BlockFile::File>` — files opened through the manager; the
  write-back path needs the victim's file handle, looked up by fd.

### API

- `void init(std::size_t pool_size, std::unique_ptr<ReplacementPolicy> policy)`
  — pool init, bookkeeping resize, `policy->init(pool_size)`.
- `bool open_file(const std::string& path, std::string& error)` — wraps
  `BlockFile::open_for_append` (O_RDWR | O_CREAT: serves both reads and
  write-backs).
- `std::size_t pin(BlockFile::File& file, std::uint64_t block_number, std::string& error)`
  — returns the frame index holding the block; increments pin count.
- `void unpin(std::size_t frame, bool was_dirty, std::string& error)` —
  decrements pin count, sets the dirty flag if `was_dirty`; error if pin
  count is already 0.
- `bool flush(std::size_t frame, std::string& error)` — if the frame is
  dirty, write it back and clear the flag.
- `bool flush_all(std::string& error)`.
- `void close_all(std::string& error)` — flush_all, close every file, clear
  page table and reverse entries, reset the policy
  (`policy_->init(pool_size)`); fds become invalid, metadata must not
  survive.

### `pin` algorithm (the core)

1. **Hit**: `Key k = {file.fd, block_number}` found in the page table →
   frame `f`: `pool_.pin(f)`, `policy_->on_access(f)`, return `f`.
2. **Miss** — choose a victim frame:
   1. first frame whose reverse entry is empty (never used), in index order;
      no policy consultation needed (a free frame never costs a write-back);
   2. else build `candidates` = all frames that hold a page and have
      `pin_count == 0`, ask `policy_->pick_victim(candidates)`;
   3. still nothing → `error = "all frames pinned"`, fail.
3. If the victim holds a page and is dirty: `write_block` its buffer back to
   the victim page's (file, block).
4. `policy_->on_remove(victim)`; erase the victim's old key from the page
   table; install `k → victim` and the reverse entry.
5. `read_block(file, block_number, victim.buffer)` — fresh data, clear the
   dirty flag.
6. `pool_.pin(victim)`, `policy_->on_load(victim)`, return the frame index.

## Pluggable replacement policy

### Interface — `ReplacementPolicy.h` (abstract class, header only)

Fixes the API every policy implements:

- `virtual void init(std::size_t pool_size) = 0` — reset for a pool of n
  frames.
- `virtual void on_access(std::size_t frame) = 0` — a resident page was
  accessed again (pin hit).
- `virtual void on_load(std::size_t frame) = 0` — a page was placed into the
  frame.
- `virtual void on_remove(std::size_t frame) = 0` — the frame's page left
  the pool (evicted or `close_all` reset).
- `virtual std::optional<std::size_t> pick_victim(
     const std::vector<std::size_t>& candidates) const = 0` — `candidates`
  are the frames allowed to be evicted: holding a page and unpinned (the
  manager filters pin counts, so policies never touch `BufferPool` and are
  trivially unit-testable standalone). Returns the chosen frame, or
  `nullopt` if `candidates` is empty.

Contract: hooks arrive only for frames that currently hold a page;
`pick_victim` is const and side-effect free.

### Concrete policies

- **`LRUPolicy`** (implemented now, per the original requirement):
  `std::list<std::size_t>` of frames, MRU at front, plus
  `std::vector<list::iterator>` for O(1) move/erase. `on_access`/`on_load` →
  move to front; `on_remove` → erase; `pick_victim` → walk the list from the
  back and return the first frame present in `candidates`.
- **`MRUPolicy`** (drop-in later): identical bookkeeping; `pick_victim`
  walks from the front.
- **`LFUPolicy`** (drop-in later): per-frame access counter (reset on
  `on_load`, incremented on `on_access`); `pick_victim` scans `candidates`
  for the lowest count, ties broken by least-recent touch (secondary
  sequence number).

All policies live in `namespace bufman`; one `.h/.cpp` pair per implemented
policy (`LRUPolicy.{h,cpp}` first). Worst-case `pick_victim` scans are O(n)
per miss — fine at course scale.

### BlockFile extensions required (modifies the copied module — approval needed)

1. `bool write_block(File&, std::uint64_t block_number, const char* buffer,
   std::size_t length, std::string& error)` — `seek_block` + `write_all`
   (both already private helpers in BlockFile.cpp). Block-granular only
   (`length == kBlockSize`) to preserve the whole-blocks file invariant.
2. Raw-pointer read: `bool read_block(File&, std::uint64_t block_number,
   char* buffer, std::size_t length, std::string& error)` — needed because
   `DataFrame::buffer` is a plain `char[]` while the current `read_block`
   takes `std::array<char, kBlockSize>&`. The array overload stays as a thin
   wrapper. (Alternative: change `DataFrame::buffer` to
   `std::array<char, kPageSize>` — rejected: modifies another copied module;
   the raw-pointer variant also matches the byte-buffer `append_records`.)

## Decisions made (adjust if wrong)

- The replacement algorithm is injected as
  `std::unique_ptr<ReplacementPolicy>` at `init` — runtime polymorphism via
  an abstract class, no templates, per the "algorithm as parameter"
  requirement.
- Metadata (page table, reverse entries) lives in **BufferManager**, not in
  BufferPool/DataFrame — the ex08 modules stay untouched, matching "this
  module manages the buffer pool".
- `pick_victim` receives a pre-filtered candidate list so policies stay
  decoupled from `BufferPool` (easy standalone unit tests).
- Free (never-used) frames are consumed before any policy victim —
  policy-independent and never costs a write-back.
- **`LRUPolicy` ships now**; `MRUPolicy`/`LFUPolicy` are interface-ready
  drop-ins (say the word and all three get implemented + tested).
- Error style: BlockFile-style `error` strings (cleared on entry) for all
  BufferManager I/O paths; BufferPool exceptions (`out_of_range`,
  `logic_error`) propagate unchanged.
- Files are assumed open for the manager's lifetime; fd reuse after
  close/reopen would alias metadata keys (mitigated by `close_all` clearing
  all state). Documented limitation, fine for course scope.
- `kPageSize` (DataFrame) and `kBlockSize` (PersonSerializer) stay as two
  names for 4096; they document different roles. Can alias later.

## Testing plan (ex08-style `check()` calls, replacing the CLion scaffold main)

1. `init` + `open_file` on a file seeded via `write_block`.
2. Miss loads from disk: `pin` a block, verify buffer bytes.
3. Hit: pin the same block again → same frame index, pin count 2 (page-table
   hit, no second load).
4. Policy contract, via a recording fake policy: hooks fire correctly
   (`on_access` on hit, `on_load` on miss, `on_remove` on eviction) and
   `pick_victim` receives only unpinned page-holding frames.
5. LRU ordering (through `LRUPolicy`): fill the pool with blocks 0..2, unpin
   all, re-pin block 0 (promote), pin block 3 → block 1's frame is evicted,
   not block 0's.
6. Pinned refusal: pin every frame, pin one more block → "all frames pinned".
7. Dirty write-back: pin, modify the buffer, `unpin(frame, true)`, force
   eviction, re-pin → modified bytes come back; verify on-disk bytes through
   a separate BlockFile handle.
8. Never-used-first: with free frames remaining, a new block lands in a
   never-used frame, not behind eviction.
9. `flush` clears the dirty flag; write-back happens exactly once.
10. `unpin` at pin count 0 → `logic_error` (expected-throw check, ex08 style).
11. `close_all` clears state and resets the policy; pinning afterwards fails
    cleanly.
12. (If MRU/LFU are implemented) same eviction scenario as case 5 run with
    each policy → different victim, proving swappability.

## Out of scope

Concurrency/pinning across threads, partial-page or record-level writes,
prefetching, file deletion from the page table while open, policies beyond
the three named (clock, random, …).
