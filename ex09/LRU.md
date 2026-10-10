# LRU.md — LRUPolicy, method by method (ex09)

`LRUPolicy.{h,cpp}` implements the `ReplacementPolicy` interface: it decides
which frame the `BufferManager` evicts when every frame holds a page. The
rule is **least recently used** — the frame whose page has gone untouched
the longest is the first candidate. This note walks through every method and
the data structure they maintain.

## The data being maintained

The policy does not own frames and never touches `BufferPool`. All it keeps
is an ordering of frame indices:

```cpp
// MRU at front, LRU at back; positions_[frame] is end() while the
// frame holds no tracked page.
std::list<std::size_t> order_;
std::vector<std::list<std::size_t>::iterator> positions_;
```

- `order_` is a doubly linked list of frame indices sorted by recency: the
  **most recently used (MRU) frame at the front**, the **least recently
  used (LRU) frame at the back**.
- `positions_[frame]` caches the list node where that frame currently sits,
  so moving or removing a frame is **O(1)** — no search. Without it, every
  `touch` would be an O(n) scan to find the node to erase.
- `positions_[frame] == order_.end()` is the sentinel for *"this frame is
  not currently tracked"* (it holds no page). `init` fills the whole vector
  with that sentinel.

Why a list instead of `std::deque` or a vector? Erasing from the middle of
a list is O(1) given an iterator, and the iterators of a `std::list` stay
valid across inserts and erases elsewhere — exactly what `positions_`
relies on.

The manager→policy contract (see `bufman.md`) is what makes this simple
state sufficient: the hooks below fire **only for frames that currently
hold a page**, and `pick_victim` receives a **pre-filtered** candidate list
(page-holding, `pin_count == 0`), so the policy never has to check pin
counts itself.

## `init(std::size_t pool_size)`

```cpp
void LRUPolicy::init(std::size_t pool_size) {
    order_.clear();
    positions_.assign(pool_size, order_.end());
}
```

Resets the policy for a pool of `pool_size` frames: the ordering is emptied
and every frame is marked untracked. Called once by
`BufferManager::init`, and again by `close_all` (teardown must leave no
state behind, since fds and page-table entries are being dropped too) — so
a reused manager starts with a clean LRU slate every time.

## `touch(std::size_t frame)` — private helper

```cpp
void LRUPolicy::touch(std::size_t frame) {
    if (positions_[frame] != order_.end()) {
        order_.erase(positions_[frame]);
    }
    order_.push_front(frame);
    positions_[frame] = order_.begin();
}
```

"Use" a frame: if it is already tracked, unlink its node; then relink it at
the **front** (MRU position) and record the new node in `positions_`.
Both `on_access` and `on_load` reduce to this, which is why it is factored
out. Cost: O(1).

## `on_access(std::size_t frame)`

```cpp
void LRUPolicy::on_access(std::size_t frame) {
    touch(frame);
}
```

Reports a **cache hit**: the manager pinned a block that was already
resident in this frame. The frame moves to the front of `order_`, marking
it as freshly used — a hit protects the page from eviction for a while
longer. In the check suite this is the `access:0`-style event the
`RecordingPolicy` observes.

## `on_load(std::size_t frame)`

```cpp
void LRUPolicy::on_load(std::size_t frame) {
    touch(frame);
}
```

Reports a **new page placed in the frame** — either a miss that was just
filled from disk or a brand-new `alloc_page`. A freshly loaded page is by
definition the most recently used one, so again it goes to the front.

LRU treats `on_access` and `on_load` identically, so why two hooks? Because
other policies do not: an LFU policy would *reset* its counter on `on_load`
and *increment* it on `on_access`; an MRU policy evicts what `on_access`
just promoted. The interface anticipates them, and the manager can stay
oblivious to the differences.

## `on_remove(std::size_t frame)`

```cpp
void LRUPolicy::on_remove(std::size_t frame) {
    if (positions_[frame] != order_.end()) {
        order_.erase(positions_[frame]);
        positions_[frame] = order_.end();
    }
}
```

Reports that the frame's **page left the pool** — it was evicted by the
manager (after a write-back if dirty). The frame is unlinked from the
ordering and its `positions_` entry is reset to the `end()` sentinel. If
the frame is untracked (never loaded since the last reset), this is a
no-op. Without this cleanup a stale list node could hand an evicted frame
back to `pick_victim` later.

## `pick_victim(...)` — const

```cpp
std::optional<std::size_t> LRUPolicy::pick_victim(
        const std::vector<std::size_t>& candidates) const {
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
        if (std::find(candidates.begin(), candidates.end(), *it) != candidates.end()) {
            return *it;
        }
    }
    return std::nullopt;
}
```

Chooses the eviction target when the manager needs a frame and none is
free. It walks `order_` **backwards** (via `rbegin`, from the LRU end) and
returns the first frame that is also in `candidates` — i.e. the
least-recently-used frame among those the manager is allowed to evict
(pinned frames never appear in `candidates`). Returns `nullopt` when there
is nothing to evict (no candidates, or nothing tracked), which the manager
turns into `"all frames pinned"`.

Two details worth noting:

- **`candidates`, not just `order_.back()`**: the LRU frame overall may be
  pinned, so the victim is the LRU frame *that is unpinned*. Skipping past
  pinned frames is what lets a long-lived pinned page coexist with a churn
  of unpinned ones.
- **`const`** and side-effect free: picking a victim does not itself change
  the ordering; the subsequent `on_remove` + `on_load` hooks do. This keeps
  the decision step observable and testable (the `RecordingPolicy` in
  `main.cpp` captures `last_candidates` in this method).
- Cost is O(|order_| × |candidates|) worst case — the `std::find` inside the
  walk. Fine at course scale; a bitset or a per-frame "is candidate" array
  would make it O(|order_|) if it ever mattered.

## A worked example

Pool of 3 frames, blocks A, B, C, then A again, then a miss on D (all pages
unpinned when the miss happens):

| step | hook | `order_` (MRU → LRU) |
|------|------|----------------------|
| pin A (miss)  | `load:0` | `[0]` |
| pin B (miss)  | `load:1` | `[1, 0]` |
| pin C (miss)  | `load:2` | `[2, 1, 0]` |
| pin A (hit)   | `access:0` | `[0, 2, 1]` |
| pin D (miss)  | `pick_victim({0,1,2})` → **1**, `remove:1`, `load:1` | `[1, 0, 2]` |

Frame 1 (block B) is evicted even though frame 0 was loaded earlier,
because the hit on A promoted frame 0 and left frame 1 as the least
recently used. This is exactly the sequence the check suite asserts in
`main.cpp` ("block 3 evicts the LRU frame (block 1's), not block 0's").

## Complexity summary

| method | cost | effect on `order_` |
|--------|------|--------------------|
| `init` | O(n) | emptied; all `positions_` = `end()` |
| `on_access` | O(1) | frame moved to front |
| `on_load` | O(1) | frame moved to front |
| `on_remove` | O(1) | frame unlinked, untracked |
| `pick_victim` | O(n) scan | unchanged |

(`n` = pool size. `pick_victim` is the only non-O(1) method, and only
because it must skip candidates.)
