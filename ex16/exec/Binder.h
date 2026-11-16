#pragma once

// Binder — the pass that turns a parsed sql::Expr tree into a resolved,
// statically typed BoundExpr tree against one table's schema. Column
// names become tuple positions; every node's type is inferred here, so
// type errors (unknown columns, string-vs-number comparison, arithmetic
// on strings, non-boolean logic) are named bind errors BEFORE any page
// is pinned.
//
// Two binding contexts. The row context (bind_expr) refuses aggregate
// nodes by name — WHERE sees rows, not groups. The group context
// (bind_group_expr) accepts aggregate calls: each distinct call is
// recorded in a GroupBinding inventory in first-appearance order
// (structural equality decides identity, so `COUNT(*)` spelled twice
// shares one state slot), the BoundExpr node carries the inventory slot,
// and a bare Column is legal only when it names a GROUP BY column.

#include "exec/Value.h"
#include "schema/TableSchema.h"
#include "sql/Ast.h"

#include <memory>
#include <string>
#include <vector>

namespace exec {

struct BoundExpr {
    enum class Kind { Literal, Column, Binary, Unary, Aggregate };

    Kind kind = Kind::Literal;
    Type type = Type::Bool;
    Value literal;                          // Literal
    std::size_t column_index = 0;           // Column
    sql::BinaryOp bin_op = sql::BinaryOp::Eq;   // Binary
    sql::UnaryOp un_op = sql::UnaryOp::Not;     // Unary
    sql::FunctionCode fn = sql::FunctionCode::Count;  // Aggregate
    std::unique_ptr<BoundExpr> left;            // Binary
    std::unique_ptr<BoundExpr> right;           // Binary
    std::unique_ptr<BoundExpr> operand;         // Unary
    std::size_t aggregate_index = 0;            // Aggregate: inventory slot

    // The aggregate's bound argument lives in the GroupBinding inventory,
    // not here: per-row folding walks the inventory, per-group evaluation
    // reads final states by index, so the tree node never needs it.
};

using BoundExprPtr = std::unique_ptr<BoundExpr>;

// One distinct aggregate call of a grouped statement. `arg` is null for
// COUNT(*); `type` is the aggregate's result type (COUNT int, AVG double,
// SUM/MIN/MAX the argument's static type).
struct BoundAggregate {
    sql::FunctionCode fn = sql::FunctionCode::Count;
    BoundExprPtr arg;
    Type type = Type::Int;
};

// The grouped context of one SELECT: the GROUP BY columns as schema
// positions in GROUP BY order, and the distinct-aggregate inventory the
// bound trees point into.
struct GroupBinding {
    std::vector<std::size_t> key_columns;
    std::vector<BoundAggregate> aggregates;
};

// Binds `expr` in the row context (WHERE, group-by keys, index bounds).
// Aggregate nodes are refused by name. On failure: false, a named error,
// and `out` left null.
bool bind_expr(const sql::Expr& expr, const schema::TableSchema& schema,
               BoundExprPtr& out, std::string& error);

// Binds `expr` in the group context (select items, HAVING): aggregate
// calls land in `group`'s inventory, bare columns must be GROUP BY
// columns, and the aggregate's own argument binds in the row context
// (so nested aggregates are refused by name).
bool bind_group_expr(const sql::Expr& expr, const schema::TableSchema& schema,
                     GroupBinding& group, BoundExprPtr& out, std::string& error);

// Structural equality of bound trees — the inventory's dedup key and a
// check-suite convenience.
bool bound_equal(const BoundExpr& a, const BoundExpr& b);

}
