#ifndef EX08_BUFFER_POOL_H
#define EX08_BUFFER_POOL_H

#include "DataFrame.h"

#include <cstddef>
#include <vector>

namespace bufman {

/// A fixed set of DataFrame entries that queries share.
class BufferPool {
public:
    /// Creates n empty frames (pin_count = 0, dirty = false, zeroed buffer).
    void init(std::size_t n);

    /// Returns the frame at index, throwing std::out_of_range if invalid.
    DataFrame& frame(std::size_t index);
    const DataFrame& frame(std::size_t index) const;

    /// Turns the dirty flag of frame index on (true) or off (false).
    void set_dirty(std::size_t index, bool dirty);

    /// Records that one more query uses frame index.
    void pin(std::size_t index);

    /// Records that one query stopped using frame index.
    /// Throws std::logic_error if the pin count is already zero.
    void unpin(std::size_t index);

    /// Number of frames in the pool.
    std::size_t size() const;

private:
    std::vector<DataFrame> frames_;
};

}

#endif // EX08_BUFFER_POOL_H
