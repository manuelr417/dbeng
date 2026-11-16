#pragma once

// Evaluator — a bound expression tree over one row produces a Value.
// Semantics (design D6): mixed int/double arithmetic and comparison
// promote to double; int/int division truncates toward zero; strings
// compare lexicographically; AND/OR short-circuit; booleans are ints
// 0/1; division and modulo by zero are named runtime errors.
//
// A second walker evaluates group-context trees over one group's
// RESULTS (select items, HAVING): Column nodes read the group's key
// values by column index, Aggregate nodes read the group's final
// aggregate values by inventory slot. The operator application is
// shared, so both walkers agree on every type and error rule.

#include "exec/Binder.h"
#include "tuple/Tuple.h"

#include <cstddef>
#include <string>
#include <vector>

namespace exec {

bool evaluate(const BoundExpr& expr, const tuple::Tuple& row, Value& out,
              std::string& error);

// One group's inputs: the key column indexes in GROUP BY order, the
// drained key values, and the final aggregate values (one per inventory
// slot, from group::final_value).
bool evaluate_group(const BoundExpr& expr,
                    const std::vector<std::size_t>& key_columns,
                    const std::vector<Value>& key_values,
                    const std::vector<Value>& aggregate_values, Value& out,
                    std::string& error);

}
