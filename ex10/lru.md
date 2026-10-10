# LRU Policy — Implementation Alternatives and Trade-offs

## Prompt (original request)

> what is this business of the iterator in the implementation of LRUPolicy
>
> any simpler way using maps?
>
> generate a document with the alternatives and tradeoffs. call it lru.md

## 1. Context

`LRUPolicy` (`policy/LRUPolicy.h/.cpp`) must answer one question: *among the
frames the manager offers as candidates, which one was used least recently?*
The interface (`policy/ReplacementPolicy.h`) fixes the contract:

- `init(pool_size)` — reset state for a pool of `pool_size` frames.
- `on_load(frame)` / `on_access(frame)` — the frame just became (or was) used;
  it is now the most recently used frame.
- `on_remove(frame)` — the frame no longer holds a page; forget it.
- `pick_victim(candidates)` — return the candidate frame with the oldest last
  use, or `nullopt` if none of the candidates is tracked.

Two facts shape every alternative below:

1. **Frame ids are dense.** They are indices `0 .. pool_size-1` into the pool,
   so anything a plain array can index works — no hashing needed.
2. **Candidates are already materialized.** `BufferManager::find_victim_frame`
   scans all frames and passes a vector of page-holding, unpinned frames
   (`bufpool/BufferManager.cpp:51-64`). The policy never needs to know about
   pin counts; it only ranks the frames it is handed.

The shipped implementation stores a recency *list* (`std::list<std::size_t>`,
MRU at front, LRU at back) plus one cached iterator per frame
(`positions_[frame]`) so `touch()` can unlink and re-insert in O(1) without
searching the list. The iterator business exists solely to make that unlink
cheap: `std::list::erase(iterator)` is O(1) *only if you already hold the
iterator*.

## 2. Alternatives

### A. Doubly linked list + cached iterators (current)

```cpp
std::list<std::size_t> order_;                                  // MRU front, LRU back
std::vector<std::list<std::size_t>::iterator> positions_;       // one per frame

void touch(std::size_t frame) {
    if (positions_[frame] != order_.end()) order_.erase(positions_[frame]);
    order_.push_front(frame);
    positions_[frame] = order_.begin();
}
```

- **Costs:** `touch` / `on_load` / `on_access` O(1); `on_remove` O(1);
  `pick_victim` walks the list from the back and returns the first candidate
  hit — O(position of the victim in the recency order), worst case O(F) with
  F = pool size.
- **Complexity in the code:** the only alternative that must reason about
  *iterator validity* (list iterators survive unrelated erases; a frame's own
  iterator must be refreshed on every move) and a *sentinel convention*
  (`positions_[frame] == order_.end()` means "not tracked", and `end()`
  changes as the list mutates, so it is only ever compared, never stored
  long-term).
- **Memory:** one 8-byte pointer-sized iterator per frame plus two pointers
  per list node.
- **Best trait:** `pick_victim` stops at the first candidate — it reads in
  true recency order and does no per-candidate lookups.
- **Worst trait:** the machinery (cached iterators, sentinel, erase-and-relink)
  exists to protect an O(1) `touch`, yet `touch` is *not* the bottleneck in a
  teaching pool of a few frames.

### B. Map of frame → last-use tick

Replace ordering with timestamps. One monotonically increasing counter assigns
a "tick" on every use; the frame with the smallest tick is the victim.

```cpp
std::map<std::size_t, std::uint64_t> last_use_;   // frame -> tick
std::uint64_t tick_ = 0;

void on_load(std::size_t f)   { last_use_[f] = tick_++; }
void on_access(std::size_t f) { last_use_[f] = tick_++; }
void on_remove(std::size_t f) { last_use_.erase(f); }

std::optional<std::size_t> pick_victim(const std::vector<std::size_t>& cand) const {
    std::optional<std::size_t> victim;
    std::uint64_t best = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t f : cand)
        if (auto it = last_use_.find(f); it != last_use_.end() && it->second < best) {
            best = it->second;
            victim = f;
        }
    return victim;
}
```

- **Costs:** `touch`-family O(log F); `on_remove` O(log F); `pick_victim`
  O(C · log F) with C = number of candidates.
- **Complexity in the code:** dramatically lower. No iterators to cache, no
  sentinel, no relink; each hook is a one-liner. The tick counter is ordinary
  integer state.
- **Subtleties:** none that bite at teaching scale. `tick_` overflow needs
  2^64 accesses; ties are impossible while the counter is fresh (and even a
  tie would only matter if two frames shared a tick, which `tick_++` prevents).
- **Memory:** one map node (≈ 40+ bytes) per tracked frame.

### C. Vector of ticks indexed by frame

The same timestamp idea, exploiting that frame ids are dense:

```cpp
std::vector<std::uint64_t> last_use_;   // [frame] -> tick, 0 = not tracked
std::uint64_t tick_ = 0;                // starts at 1 so 0 is a free sentinel

void init(std::size_t pool_size) { last_use_.assign(pool_size, 0); }
void on_load(std::size_t f)      { last_use_[f] = ++tick_; }
void on_access(std::size_t f)    { last_use_[f] = ++tick_; }
void on_remove(std::size_t f)    { last_use_[f] = 0; }

std::optional<std::size_t> pick_victim(const std::vector<std::size_t>& cand) const {
    std::optional<std::size_t> victim;
    std::uint64_t best = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t f : cand)
        if (last_use_[f] != 0 && last_use_[f] < best) {
            best = last_use_[f];
            victim = f;
        }
    return victim;
}
```

- **Costs:** every hook O(1); `pick_victim` O(C).
- **Complexity in the code:** the lowest of all alternatives — a plain array,
  an integer counter, and one `if`. Same "bytes are the only state, recompute
  by scanning" spirit as the SlottedPage design principles (page.md §1a).
- **Subtleties:** tick 0 is the "not tracked" sentinel, so the counter starts
  at 1; overflow needs 2^64 accesses. Candidates are always valid frame ids,
  so indexing is safe by contract.
- **Memory:** exactly 8 bytes per frame, contiguous, no allocations after
  `init`.

### D. Two maps keyed by tick (ordered structure, O(log F) victim)

If `pick_victim` should not scan candidates at all, keep an ordered map from
tick → frame so the LRU end is simply `begin()`, plus the frame → tick map for
removal:

```cpp
std::map<std::uint64_t, std::size_t> by_tick_;   // tick -> frame, LRU first
std::map<std::size_t, std::uint64_t> tick_of_;   // frame -> tick

void touch(std::size_t f) {
    if (auto it = tick_of_.find(f); it != tick_of_.end()) {
        by_tick_.erase(it->second);
        tick_of_.erase(it);
    }
    by_tick_.emplace(tick_, f);
    tick_of_.emplace(f, tick_);
    ++tick_;
}
```

- **Costs:** hooks O(log F); `pick_victim` walks `by_tick_` from `begin()` and
  returns the first frame found in `candidates` — O(C) membership *probes*
  (each O(log F)) but visits frames in true recency order, like A.
- **Complexity in the code:** strictly worse than B and C — it is the list
  approach's dual bookkeeping (two structures that must agree) with map nodes
  instead of list nodes, and it still needs a second lookup to answer
  "is this frame a candidate?".
- **When it wins:** only if candidates were *not* materialized by the caller
  and the policy had to walk recency order against a predicate. That is not
  our interface.

### E. `std::priority_queue` (min-heap of ticks)

- Rejected: a heap cannot update or erase an arbitrary frame's entry without
  lazy deletion (push duplicates, discard stale pops in `pick_victim`). The
  bookkeeping — staleness checks per frame — is more code than A, B, or C, and
  the "heap" abstraction hides the very recency reasoning this exercise is
  about.

## 3. Comparison

Per call, with F = pool size, C = |candidates|:

| Alternative         | touch / load / access | on_remove | pick_victim      | State per frame        | Invariant risk |
|---------------------|-----------------------|-----------|------------------|------------------------|----------------|
| A. list + iterators | O(1)                  | O(1)      | O(C) probes… O(F) walk | 1 list node + 1 cached iterator | iterator validity + end() sentinel |
| B. map frame→tick   | O(log F)              | O(log F)  | O(C · log F)     | 1 map node             | none           |
| C. vector of ticks  | O(1)                  | O(1)      | O(C)             | 8 bytes in one array   | tick 0 sentinel |
| D. two tick maps    | O(log F)              | O(log F)  | O(C · log F)     | 2 map nodes            | two structures must agree |
| E. priority queue   | O(log F)              | lazy      | O(C · log F) + stale skips | heap + version stamps | stale entries |

Readability ranking (most → least direct): **C > B > A > D > E**.
Speed ranking at this pool size: indistinguishable; asymptotically A/C have
the cheapest hooks, A/D visit candidates in recency order.

## 4. Observations specific to this interface

- The `candidates` parameter weakens the case for *maintaining order at all*.
  A, B, C, and D all end up doing work proportional to C inside
  `pick_victim`; the difference is only whether the work is a walk (A, D) or a
  min-scan (B, C). The list's carefully maintained order is consumed by at
  most one eviction per miss.
- The manager itself already scans every frame to build the candidate vector
  (`BufferManager::find_victim_frame`), so an O(C) or O(C log F) victim pick
  does not change the overall complexity class of an eviction.
- `on_access` and `on_load` are semantically identical in every LRU variant
  ("make this frame MRU"), which is why all alternatives collapse them into
  one `touch`-style helper.

## 5. Trade-off summary

- **A (current)** pays in *code sophistication* (cached iterators, sentinel)
  to keep every hook O(1) and the victim walk order-optimal. It is the
  textbook LRU implementation and the right thing to study — but its
  complexity serves a workload (huge pools, millions of accesses per second)
  that this project does not have.
- **B** trades log-factor map overhead for the removal of all iterator
  reasoning. Good middle ground if the policy were ever keyed by something
  non-dense (e.g. page ids instead of frame ids).
- **C** is the simplest correct implementation for this codebase: dense frame
  ids make a plain vector the natural map, and the "recompute by scan, keep
  no derived state" philosophy matches page.md §1a. The only sentinels are
  the integer 0 and a counter starting at 1.
- **D** and **E** add structure without removing any of A's or B's burdens;
  they are listed for completeness.

## 6. Recommendation

Keep A in place as the reference "classic" LRU (it already passes the ~200
checks in `main.cpp`), or switch to **C** if the goal is minimal code. Both
satisfy the same test plan; the choice is purely about which lesson the code
should teach: *how* classic O(1) LRU is built (A) or *that* a timestamp array
is all LRU needs (C).

## 7. Open questions

- Should `LRUPolicy` move to C while A's rationale is captured only here, or
  keep A and treat this document as the record of why the iterator exists?
  (Proposal: keep A; revisit if `pick_victim`'s candidate walk ever shows up
  in practice.)
- If another policy is added (e.g. MRU, FIFO, Clock), the tick-vector shape
  (C) generalizes most easily (FIFO = oldest *load* tick instead of last
  *access* tick; Clock = reference bit per frame in the same vector layout).
