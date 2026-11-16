#pragma once

// group/ — the static-hash GROUP BY operator (design D5, D6).
//
// GroupTable is the lecture's static hash scheme applied to aggregation:
// a fixed number of buckets, routing `h(key) mod N`, per-bucket chains of
// group entries accumulating aggregate states. No splits, no resize,
// ever — a skewed group distribution just grows chains. The table sizes
// with the group count, not the row count; an entry budget bounds it,
// and exhausting the budget is the operator's spill signal.
//
// The hash follows the ex14 convention: a single integer key hashes to
// its own bit pattern (the slides' h(4) = 100), everything else through
// FNV-1a 32-bit over a typed byte image of the key values (a tag byte
// per column, then the value's bytes), so `1` and "1" never collide.
//
// Pure structure: no heap access, no buffer pool, no executor. The
// caller folds one row at a time (group-key values plus one argument
// value per aggregate slot that takes one) and drains the finished
// groups.

#include "exec/Value.h"
#include "sql/Ast.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace group {

// The compiled shape of one aggregate slot: what fold updates. Parallel
// to the binder's inventory (exec::BoundAggregate); `takes_arg` is false
// only for COUNT(*) — with no NULLs, COUNT(col) still folds by counting
// rows, but its argument is evaluated (and spilled) for uniformity.
struct AggregateSpec {
    sql::FunctionCode fn = sql::FunctionCode::Count;
    exec::Type result_type = exec::Type::Int;
    exec::Type arg_type = exec::Type::Int;  // meaningful when takes_arg
    bool takes_arg = false;
};

// One aggregate slot's accumulated state.
struct AggregateState {
    std::uint64_t count = 0;      // COUNT, and AVG's denominator
    std::int64_t int_sum = 0;     // SUM/AVG over an integer argument
    double double_sum = 0.0;      // SUM/AVG over a double argument
    exec::Value extremum{0};      // MIN/MAX; identity value until set
    bool has_extremum = false;
};

// One group: its key values and one state per aggregate slot.
struct GroupEntry {
    std::vector<exec::Value> key;
    std::vector<AggregateState> states;
};

// The static hash table over group keys.
class GroupTable {
public:
    // `bucket_count` is fixed for the table's life; `entry_budget` bounds
    // the total group count (the spill signal when exceeded).
    GroupTable(const std::vector<AggregateSpec>& aggregates,
               std::size_t bucket_count, std::size_t entry_budget);

    // Folds one row: `key` = the group-key values, `args` = one value per
    // slot aligned with the specs (read only where takes_arg). Appends a
    // new entry on a key miss. False with a named error when the entry
    // budget is exhausted.
    bool fold(const std::vector<exec::Value>& key,
              const std::vector<exec::Value>& args, std::string& error);

    std::size_t entry_count() const { return entry_count_; }
    std::size_t bucket_count() const { return buckets_.size(); }

    // Drains the table: every group in bucket order (buckets 0..N-1,
    // entries in arrival order within a bucket), leaving the table empty.
    std::vector<GroupEntry> take_all();

private:
    struct Bucket {
        std::vector<GroupEntry> chain;  // the overflow chain, in memory
    };

    std::vector<GroupEntry*> find(const std::vector<exec::Value>& key);

    const std::vector<AggregateSpec>& aggregates_;
    std::vector<Bucket> buckets_;
    std::size_t entry_budget_;
    std::size_t entry_count_ = 0;
};

// The group-key hash: the ex14 convention over one or more values.
std::uint32_t hash_group_key(const std::vector<exec::Value>& key);

// The aggregate's final value over its accumulated state — the identity
// value for an empty group (COUNT 0, SUM 0 or 0.0, AVG 0.0, MIN/MAX the
// type's zero value; this engine has no NULLs).
exec::Value final_value(const AggregateSpec& spec, const AggregateState& state);

}
