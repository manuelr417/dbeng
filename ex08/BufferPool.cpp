#include "BufferPool.h"

#include <stdexcept>

namespace buffer_pool {

void BufferPool::init(std::size_t n) {
    frames_.assign(n, DataFrame{});
}

DataFrame& BufferPool::frame(std::size_t index) {
    if (index >= frames_.size()) {
        throw std::out_of_range("BufferPool::frame: index out of range");
    }
    return frames_[index];
}

const DataFrame& BufferPool::frame(std::size_t index) const {
    if (index >= frames_.size()) {
        throw std::out_of_range("BufferPool::frame: index out of range");
    }
    return frames_[index];
}

void BufferPool::set_dirty(std::size_t index, bool dirty) {
    frame(index).dirty = dirty;
}

void BufferPool::pin(std::size_t index) {
    ++frame(index).pin_count;
}

void BufferPool::unpin(std::size_t index) {
    DataFrame& f = frame(index);
    if (f.pin_count == 0) {
        throw std::logic_error("BufferPool::unpin: pin count is already zero");
    }
    --f.pin_count;
}

std::size_t BufferPool::size() const {
    return frames_.size();
}

}
