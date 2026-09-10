#include "LRUPolicy.h"

#include <algorithm>

namespace bufman {

void LRUPolicy::init(std::size_t pool_size) {
    order_.clear();
    positions_.assign(pool_size, order_.end());
}

void LRUPolicy::touch(std::size_t frame) {
    if (positions_[frame] != order_.end()) {
        order_.erase(positions_[frame]);
    }
    order_.push_front(frame);
    positions_[frame] = order_.begin();
}

void LRUPolicy::on_access(std::size_t frame) {
    touch(frame);
}

void LRUPolicy::on_load(std::size_t frame) {
    touch(frame);
}

void LRUPolicy::on_remove(std::size_t frame) {
    if (positions_[frame] != order_.end()) {
        order_.erase(positions_[frame]);
        positions_[frame] = order_.end();
    }
}

std::optional<std::size_t> LRUPolicy::pick_victim(
        const std::vector<std::size_t>& candidates) const {
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
        if (std::find(candidates.begin(), candidates.end(), *it) != candidates.end()) {
            return *it;
        }
    }
    return std::nullopt;
}

}
