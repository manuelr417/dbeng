#pragma once

// Value — the executor's value domain: what an expression evaluates to
// and what a projected cell holds. Booleans are ints 0/1 (there are no
// NULLs in this SQL subset, so logic is two-valued).

#include <string>
#include <variant>

namespace exec {

using Value = std::variant<int, double, std::string>;

// Static expression types: the three storage types plus Bool, which
// exists only in the binder's type algebra (comparisons and logic).
enum class Type { Int, Double, String, Bool };

const char* type_name(Type type);

// A readable rendering for result printing and error messages.
std::string display(const Value& value);

// Comparison shared by the planner (bound intersection) and the
// evaluator: numeric operands promote to double, strings compare
// lexicographically. Negative/zero/positive like strcmp.
int compare_values(const Value& a, const Value& b);

// Arithmetic promotion: true when either side is Double.
bool promotes_to_double(const Value& a, const Value& b);

}
