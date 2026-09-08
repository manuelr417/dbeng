#include "BufferPool.h"
#include "DataFrame.h"

#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (condition) {
        std::cout << "PASS: " << message << "\n";
    } else {
        std::cout << "FAIL: " << message << "\n";
        ++failures;
    }
}

}

int main() {
    // --- init: pool with n entries, all clean and unpinned ---
    buffer_pool::BufferPool pool;
    pool.init(4);
    check(pool.size() == 4, "init creates 4 entries");

    bool all_clean = true;
    for (std::size_t i = 0; i < pool.size(); ++i) {
        const auto& f = pool.frame(i);
        all_clean = all_clean && f.pin_count == 0 && !f.dirty;
    }
    check(all_clean, "all entries start with pin_count = 0 and dirty = false");

    // --- access the buffer of entry n ---
    auto& f2 = pool.frame(2);
    f2.buffer[0] = 'A';
    check(pool.frame(2).buffer[0] == 'A',
          "buffer of entry 2 is accessible and writable");

    // --- dirty flag on / off ---
    pool.set_dirty(2, true);
    check(pool.frame(2).dirty, "set_dirty(2, true) turns the flag on");
    pool.set_dirty(2, false);
    check(!pool.frame(2).dirty, "set_dirty(2, false) turns the flag off");

    // --- pin / unpin ---
    pool.pin(2);
    pool.pin(2);
    check(pool.frame(2).pin_count == 2, "two pins give pin_count = 2");
    pool.unpin(2);
    check(pool.frame(2).pin_count == 1, "one unpin gives pin_count = 1");
    pool.unpin(2);
    check(pool.frame(2).pin_count == 0, "second unpin gives pin_count = 0");

    // --- out-of-range access is rejected ---
    bool threw = false;
    try {
        pool.frame(4);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    check(threw, "accessing entry 4 of a 4-entry pool throws");

    std::cout << (failures == 0 ? "\nAll checks passed." : "\nSome checks FAILED.")
              << "\n";
    return failures == 0 ? 0 : 1;
}
