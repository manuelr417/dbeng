#pragma once

// Evaluator — a bound expression tree over one row produces a Value.
// Semantics (design D6): mixed int/double arithmetic and comparison
// promote to double; int/int division truncates toward zero; strings
// compare lexicographically; AND/OR short-circuit; booleans are ints
// 0/1; division and modulo by zero are named runtime errors.

#include "exec/Binder.h"
#include "tuple/Tuple.h"

#include <string>

namespace exec {

bool evaluate(const BoundExpr& expr, const tuple::Tuple& row, Value& out,
              std::string& error);

}
