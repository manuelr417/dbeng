#ifndef EX09_LRU_POLICY_H
#define EX09_LRU_POLICY_H

#include "ReplacementPolicy.h"

#include <list>
#include <optional>
#include <vector>

namespace bufman {

// Evicts the least recently accessed frame among the candidates.
class LRUPolicy final : public ReplacementPolicy {
public:
    void init(std::size_t pool_size) override;
    void on_access(std::size_t frame) override;
    void on_load(std::size_t frame) override;
    void on_remove(std::size_t frame) override;
    std::optional<std::size_t> pick_victim(
        const std::vector<std::size_t>& candidates) const override;

private:
    void touch(std::size_t frame);

    // MRU at front, LRU at back; positions_[frame] is end() while the
    // frame holds no tracked page.
    std::list<std::size_t> order_;
    std::vector<std::list<std::size_t>::iterator> positions_;
};

}

#endif // EX09_LRU_POLICY_H
