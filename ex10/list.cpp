// list.cpp — a runnable illustration of the "iterator business" in
// policy/LRUPolicy.{h,cpp}, dressed up with Person records instead of
// bare frame numbers.
//
// The LRU policy keeps two parallel structures:
//
//   std::list<std::size_t>                        order_;      // recency queue
//   std::vector<std::list<std::size_t>::iterator> positions_;  // one per frame
//
// The list alone can answer "who is least recently used" (its back element),
// but moving an arbitrary frame to the front would require *finding* it first —
// an O(F) search per access. The trick: std::list::erase(iterator) is O(1)
// if you already hold the iterator, so the policy caches one iterator per
// frame in positions_[frame] and never searches. This demo mirrors exactly
// that shape, but the list stores person ids and the array holds Persons,
// so the recency queue is visible as names, not indices.
//
// Build & run:
//   cmake -S . -B build && cmake --build build --target ex10_list
//   ./build/ex10_list

#include "part/Person.h"

#include <cstddef>
#include <cstring>
#include <iostream>
#include <list>
#include <optional>
#include <string>
#include <vector>

namespace {

Person make_person(int pid, const char* name, int age, const char* city) {
    Person value{};
    value.pid = pid;
    std::strncpy(value.name, name, sizeof(value.name) - 1);
    value.age = age;
    std::strncpy(value.city, city, sizeof(value.city) - 1);
    return value;
}

std::string describe(const Person& p) {
    return std::string(p.name) + " (pid=" + std::to_string(p.pid) +
           ", age=" + std::to_string(p.age) + ")";
}

// The same two structures LRUPolicy uses, but the list carries person ids.
using Order = std::list<std::size_t>;              // MRU at front, LRU at back
using Position = Order::iterator;

class LruIllustration {
public:
    void init(std::size_t pool_size) {
        order_.clear();
        positions_.assign(pool_size, order_.end());  // end() = "not tracked"
    }

    // touch(): make persons_[frame] the most recently used.
    // The cached iterator makes this O(1) — erase at a known position,
    // no search. This is the whole point of positions_.
    void touch(std::size_t frame) {
        if (positions_[frame] != order_.end()) {
            std::cout << "    touch(" << frame << "): erase via cached iterator"
                      << " (O(1), no search), push_front, re-cache\n";
            order_.erase(positions_[frame]);
        } else {
            std::cout << "    touch(" << frame << "): not tracked yet,"
                      << " push_front, cache begin()\n";
        }
        order_.push_front(frame);
        positions_[frame] = order_.begin();
    }

    // on_remove(): forget the frame. Its iterator is retired back to end().
    void on_remove(std::size_t frame) {
        if (positions_[frame] != order_.end()) {
            order_.erase(positions_[frame]);
            positions_[frame] = order_.end();
        }
    }

    // pick_victim(): walk the list from the back (LRU end) and return the
    // first frame among the candidates.
    std::optional<std::size_t> pick_victim(
            const std::vector<std::size_t>& candidates) const {
        for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
            for (std::size_t c : candidates) {
                if (c == *it) {
                    return c;
                }
            }
        }
        return std::nullopt;
    }

    const Order& order() const { return order_; }
    Position position_of(std::size_t frame) const { return positions_[frame]; }

private:
    Order order_;
    std::vector<Position> positions_;
};

void show(const LruIllustration& lru, const Person* persons,
          std::size_t pool_size) {
    std::cout << "    order (MRU -> LRU): ";
    for (std::size_t frame : lru.order()) {
        std::cout << "[" << frame << "] " << persons[frame].name << "  ";
    }
    std::cout << "\n    cached positions:   ";
    for (std::size_t frame = 0; frame < pool_size; ++frame) {
        if (lru.position_of(frame) == lru.order().end()) {
            std::cout << frame << ":end(sentinel)  ";
        } else {
            std::cout << frame << ":points at \"" << persons[frame].name
                      << "\"  ";
        }
    }
    std::cout << "\n";
}

}  // namespace

int main() {
    // The "pool": a plain array of Persons, like the frames of a buffer pool.
    Person persons[4] = {
        make_person(1, "ada", 36, "LD"),
        make_person(2, "grace", 45, "NY"),
        make_person(3, "edsger", 72, "AM"),
        make_person(4, "donald", 67, "PD"),
    };
    const std::size_t pool_size = 4;

    LruIllustration lru;
    lru.init(pool_size);

    std::cout << "== load persons 0, 1, 2 (in that order) ==\n";
    for (std::size_t frame : {0, 1, 2}) {
        lru.touch(frame);
    }
    show(lru, persons, pool_size);

    std::cout << "== access person 0 (\"ada\") again ==\n";
    std::cout << "  " << describe(persons[0]) << " becomes MRU\n";
    lru.touch(0);
    show(lru, persons, pool_size);

    std::cout << "== load person 3 (\"donald\") ==\n";
    lru.touch(3);
    show(lru, persons, pool_size);

    std::cout << "== iterator validity: erasing [3] leaves the others' "
                 "cached iterators valid ==\n";
    lru.on_remove(3);
    show(lru, persons, pool_size);

    std::cout << "== evict: candidates are the still-tracked frames ==\n";
    std::vector<std::size_t> candidates = {0, 1, 2};
    std::optional<std::size_t> victim = lru.pick_victim(candidates);
    if (victim) {
        std::cout << "    victim = frame " << *victim << " -> "
                  << describe(persons[*victim]) << " (the LRU end of the list)\n";
        lru.on_remove(*victim);
    }
    show(lru, persons, pool_size);

    std::cout << "== sentinel: frame 3 was never re-tracked, its position is "
                 "end() ==\n";
    std::cout << "    touch(3) again would take the 'not tracked yet' branch\n";
    lru.touch(3);
    show(lru, persons, pool_size);

    return 0;
}
