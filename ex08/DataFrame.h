#ifndef EX08_DATA_FRAME_H
#define EX08_DATA_FRAME_H

#include <cstddef>

namespace buffer_pool {

/// Size of a frame's buffer in bytes (one "page" of the buffer pool).
constexpr std::size_t kPageSize = 4096;

/// One frame of the buffer pool: a fixed-size raw buffer plus
/// the bookkeeping the pool needs to manage it.
struct DataFrame {
    int pin_count = 0;  ///< how many queries currently use this frame
    bool dirty = false; ///< true if the buffer was modified and not yet saved
    char buffer[kPageSize] = {}; ///< the actual data page
};

}

#endif // EX08_DATA_FRAME_H
