#ifndef EX09_REPLACEMENT_POLICY_H
#define EX09_REPLACEMENT_POLICY_H

#include <cstddef>
#include <optional>
#include <vector>

namespace bufman {

// Interface every buffer replacement policy implements. The manager keeps
// the policy informed via the on_* hooks and asks pick_victim for a frame
// to evict. Candidates are pre-filtered to frames that hold a page and have
// pin_count == 0, so policies never touch BufferPool.
class ReplacementPolicy {
public:
    virtual ~ReplacementPolicy() = default;

    virtual void init(std::size_t pool_size) = 0;
    virtual void on_access(std::size_t frame) = 0;
    virtual void on_load(std::size_t frame) = 0;
    virtual void on_remove(std::size_t frame) = 0;
    virtual std::optional<std::size_t> pick_victim(
        const std::vector<std::size_t>& candidates) const = 0;
};

}

#endif // EX09_REPLACEMENT_POLICY_H
