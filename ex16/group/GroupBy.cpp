#include "group/GroupBy.h"

#include <cstring>

namespace group {

namespace {

// FNV-1a 32-bit, the ex14 hash convention's workhorse (unlike the
// slides' a*k+b, it needs no tuned parameters).
constexpr std::uint32_t kFnvOffset = 2166136261u;
constexpr std::uint32_t kFnvPrime = 16777619u;

std::uint32_t fnv_1a(const char* bytes, std::size_t length) {
    std::uint32_t hash = kFnvOffset;
    for (std::size_t i = 0; i < length; ++i) {
        hash ^= static_cast<unsigned char>(bytes[i]);
        hash *= kFnvPrime;
    }
    return hash;
}

// Appends one value's typed byte image: a tag byte (so 1 and "1" never
// collide), then the value's bytes.
void append_value_image(std::string& image, const exec::Value& value) {
    if (const int* v = std::get_if<int>(&value)) {
        image.push_back('\0');
        image.append(reinterpret_cast<const char*>(v), sizeof(*v));
        return;
    }
    if (const double* v = std::get_if<double>(&value)) {
        image.push_back('\1');
        image.append(reinterpret_cast<const char*>(v), sizeof(*v));
        return;
    }
    const std::string& s = std::get<std::string>(value);
    image.push_back('\2');
    image.append(s);
    image.push_back('\3');  // length-free terminator for prefix safety
}

}

// --- the hash ----------------------------------------------------------------

std::uint32_t hash_group_key(const std::vector<exec::Value>& key) {
    if (key.size() == 1) {
        if (const int* v = std::get_if<int>(&key[0])) {
            // The lecture's identity for a lone integer key: the value's
            // own bit pattern, so h(4) = 100 and h(20) = 10100 hold.
            return static_cast<std::uint32_t>(*v);
        }
    }
    std::string image;
    for (const exec::Value& value : key) {
        append_value_image(image, value);
    }
    return fnv_1a(image.data(), image.size());
}

// --- states ------------------------------------------------------------------

exec::Value final_value(const AggregateSpec& spec, const AggregateState& state) {
    switch (spec.fn) {
        case sql::FunctionCode::Count:
            return exec::Value(static_cast<int>(state.count));
        case sql::FunctionCode::Avg:
            if (state.count == 0) {
                return exec::Value(0.0);  // the documented empty-group identity
            }
            if (spec.arg_type == exec::Type::Int) {
                return exec::Value(static_cast<double>(state.int_sum) /
                                   static_cast<double>(state.count));
            }
            return exec::Value(state.double_sum / static_cast<double>(state.count));
        case sql::FunctionCode::Sum:
            if (spec.result_type == exec::Type::Int) {
                return exec::Value(static_cast<int>(state.int_sum));
            }
            return exec::Value(state.double_sum);
        case sql::FunctionCode::Min:
        case sql::FunctionCode::Max:
            if (state.has_extremum) {
                return state.extremum;
            }
            // The type's zero value: this engine has no NULLs, so an
            // empty group answers the identity, not a null.
            switch (spec.result_type) {
                case exec::Type::Int:
                    return exec::Value(0);
                case exec::Type::Double:
                    return exec::Value(0.0);
                case exec::Type::String:
                    return exec::Value(std::string());
                default:
                    return exec::Value(0);
            }
    }
    return exec::Value(0);
}

// --- the table ---------------------------------------------------------------

GroupTable::GroupTable(const std::vector<AggregateSpec>& aggregates,
                       std::size_t bucket_count, std::size_t entry_budget)
    : aggregates_(aggregates),
      buckets_(bucket_count < 1 ? 1 : bucket_count),
      entry_budget_(entry_budget) {}

std::vector<GroupEntry*> GroupTable::find(const std::vector<exec::Value>& key) {
    // The bucket's chain, in arrival order — the static scheme's overflow
    // chain, grown in memory.
    std::vector<GroupEntry*> matches;
    Bucket& bucket = buckets_[hash_group_key(key) % buckets_.size()];
    for (GroupEntry& entry : bucket.chain) {
        if (entry.key == key) {
            matches.push_back(&entry);
        }
    }
    return matches;
}

bool GroupTable::fold(const std::vector<exec::Value>& key,
                      const std::vector<exec::Value>& args, std::string& error) {
    std::vector<GroupEntry*> matches = find(key);
    GroupEntry* entry = nullptr;
    if (!matches.empty()) {
        // Group keys are unique in the table; the chain holds at most one.
        entry = matches.front();
    } else {
        if (entry_count_ >= entry_budget_) {
            error = "group table exceeded its entry budget";
            return false;
        }
        Bucket& bucket = buckets_[hash_group_key(key) % buckets_.size()];
        bucket.chain.push_back(GroupEntry{key, {}});
        entry = &bucket.chain.back();
        entry->states.resize(aggregates_.size());
        ++entry_count_;
    }
    for (std::size_t i = 0; i < aggregates_.size(); ++i) {
        const AggregateSpec& spec = aggregates_[i];
        AggregateState& state = entry->states[i];
        switch (spec.fn) {
            case sql::FunctionCode::Count:
                ++state.count;  // no NULLs: every row counts
                break;
            case sql::FunctionCode::Avg:
                ++state.count;
                if (spec.arg_type == exec::Type::Int) {
                    state.int_sum += std::get<int>(args[i]);
                } else {
                    state.double_sum += std::get<double>(args[i]);
                }
                break;
            case sql::FunctionCode::Sum:
                if (spec.result_type == exec::Type::Int) {
                    state.int_sum += std::get<int>(args[i]);
                } else {
                    state.double_sum += std::get<double>(args[i]);
                }
                break;
            case sql::FunctionCode::Min:
            case sql::FunctionCode::Max: {
                const exec::Value& value = args[i];
                const bool better =
                    spec.fn == sql::FunctionCode::Min
                        ? !state.has_extremum ||
                              exec::compare_values(value, state.extremum) < 0
                        : !state.has_extremum ||
                              exec::compare_values(value, state.extremum) > 0;
                if (better) {
                    state.extremum = value;
                    state.has_extremum = true;
                }
                break;
            }
        }
    }
    return true;
}

std::vector<GroupEntry> GroupTable::take_all() {
    std::vector<GroupEntry> all;
    all.reserve(entry_count_);
    for (Bucket& bucket : buckets_) {
        for (GroupEntry& entry : bucket.chain) {
            all.push_back(std::move(entry));
        }
        bucket.chain.clear();
    }
    entry_count_ = 0;
    return all;
}

}
