#pragma once

// Binder — the pass that turns a parsed sql::Expr tree into a resolved,
// statically typed BoundExpr tree against one table's schema. Column
// names become tuple positions; every node's type is inferred here, so
// type errors (unknown columns, string-vs-number comparison, arithmetic
// on strings, non-boolean logic) are named bind errors BEFORE any page
// is pinned. Aggregate function nodes are refused by name — the ex13
// surface has no aggregates.

#include "exec/Value.h"
#include "schema/TableSchema.h"
#include "sql/Ast.h"

#include <memory>
#include <string>

namespace exec {

struct BoundExpr {
    enum class Kind { Literal, Column, Binary, Unary };

    Kind kind = Kind::Literal;
    Type type = Type::Bool;
    Value literal;                          // Literal
    std::size_t column_index = 0;           // Column
    sql::BinaryOp bin_op = sql::BinaryOp::Eq;   // Binary
    sql::UnaryOp un_op = sql::UnaryOp::Not;     // Unary
    std::unique_ptr<BoundExpr> left;            // Binary
    std::unique_ptr<BoundExpr> right;           // Binary
    std::unique_ptr<BoundExpr> operand;         // Unary
};

using BoundExprPtr = std::unique_ptr<BoundExpr>;

// Binds `expr` against `schema`. On failure: false, a named error, and
// `out` left null.
bool bind_expr(const sql::Expr& expr, const schema::TableSchema& schema,
               BoundExprPtr& out, std::string& error);

}
