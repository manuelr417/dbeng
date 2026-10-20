#pragma once

#include "Ast.h"

#include <string>

namespace sql {

// Renders statements and expressions back to normalized SQL: keywords
// upper, single spaces, AS always explicit, ASC implicit, <> for the
// not-equal operator. Parentheses are emitted exactly when flat text
// would re-parse to a different tree.
std::string print(const Statement& statement);
std::string print_expr(const Expr& expr);

// Label pieces shared with the tree renderer (AstDraw): the value of a
// literal as SQL text, the operator spelling, the function's name.
std::string literal_text(const LiteralValue& value);
std::string binary_op_text(BinaryOp op);
std::string function_name(FunctionCode code);

}
