#include "exec/Evaluator.h"

#include "common/Error.h"

#include <cassert>
#include <utility>

namespace exec {

namespace {

Value column_value(const tuple::Column& column) {
    switch (column.column_type()) {
        case tuple::ColumnType::Integer:
            return Value(std::get<int>(column.column_value()));
        case tuple::ColumnType::Double:
            return Value(std::get<double>(column.column_value()));
        case tuple::ColumnType::String:
            return Value(std::get<std::string>(column.column_value()));
    }
    return Value(0);
}

bool truthy(const Value& value) {
    const int* v = std::get_if<int>(&value);
    return v != nullptr && *v != 0;
}

double numeric(const Value& value) {
    if (const double* v = std::get_if<double>(&value)) {
        return *v;
    }
    return static_cast<double>(std::get<int>(value));
}

int compare_op(sql::BinaryOp op, int cmp) {
    switch (op) {
        case sql::BinaryOp::Eq: return cmp == 0;
        case sql::BinaryOp::Ne: return cmp != 0;
        case sql::BinaryOp::Lt: return cmp < 0;
        case sql::BinaryOp::Le: return cmp <= 0;
        case sql::BinaryOp::Gt: return cmp > 0;
        case sql::BinaryOp::Ge: return cmp >= 0;
        default: return 0;
    }
}

// Applies one non-logic operator to already-evaluated operands — shared
// by the row and the group evaluator (AND/OR stay in the walkers: they
// short-circuit, so their operands are not both evaluated). Division and
// modulo by zero throw their named runtime errors.
Value apply_binary(sql::BinaryOp op, Type static_type, const Value& left,
                   const Value& right) {
    if (op == sql::BinaryOp::Eq || op == sql::BinaryOp::Ne ||
        op == sql::BinaryOp::Lt || op == sql::BinaryOp::Le ||
        op == sql::BinaryOp::Gt || op == sql::BinaryOp::Ge) {
        return Value(compare_op(op, compare_values(left, right)));
    }
    // Arithmetic: promotion per the binder's static type.
    if (op == sql::BinaryOp::Div || op == sql::BinaryOp::Mod) {
        const bool integer = static_type == Type::Int;
        if (integer) {
            const int r = std::get<int>(right);
            if (r == 0) {
                throw common::Error(op == sql::BinaryOp::Div
                                        ? "division by zero"
                                        : "modulo by zero");
            }
            const int l = std::get<int>(left);
            return Value(op == sql::BinaryOp::Div ? l / r : l % r);
        }
        const double r = numeric(right);
        if (r == 0.0) {
            throw common::Error("division by zero");
        }
        return Value(numeric(left) / r);
    }
    if (static_type == Type::Int) {
        const int l = std::get<int>(left);
        const int r = std::get<int>(right);
        switch (op) {
            case sql::BinaryOp::Plus: return Value(l + r);
            case sql::BinaryOp::Minus: return Value(l - r);
            case sql::BinaryOp::Mul: return Value(l * r);
            default: return Value(0);
        }
    }
    const double l = numeric(left);
    const double r = numeric(right);
    switch (op) {
        case sql::BinaryOp::Plus: return Value(l + r);
        case sql::BinaryOp::Minus: return Value(l - r);
        case sql::BinaryOp::Mul: return Value(l * r);
        default: return Value(0.0);
    }
}

Value apply_unary(sql::UnaryOp op, const Value& operand) {
    if (op == sql::UnaryOp::Neg) {
        if (const int* v = std::get_if<int>(&operand)) {
            return Value(-*v);
        } else {
            return Value(-std::get<double>(operand));
        }
    } else {  // Not
        return Value(truthy(operand) ? 0 : 1);
    }
}

// --- the row walker ----------------------------------------------------------

Value eval_node(const BoundExpr& expr, const tuple::Tuple& row) {
    switch (expr.kind) {
        case BoundExpr::Kind::Literal:
            return expr.literal;
        case BoundExpr::Kind::Column:
            return column_value(row.column(expr.column_index));
        case BoundExpr::Kind::Unary:
            return apply_unary(expr.un_op, eval_node(*expr.operand, row));
        case BoundExpr::Kind::Binary: {
            const sql::BinaryOp op = expr.bin_op;
            if (op == sql::BinaryOp::And) {
                if (!truthy(eval_node(*expr.left, row))) {
                    return Value(0);  // short-circuit
                }
                return Value(truthy(eval_node(*expr.right, row)) ? 1 : 0);
            }
            if (op == sql::BinaryOp::Or) {
                if (truthy(eval_node(*expr.left, row))) {
                    return Value(1);  // short-circuit
                }
                return Value(truthy(eval_node(*expr.right, row)) ? 1 : 0);
            }
            const Value left = eval_node(*expr.left, row);
            const Value right = eval_node(*expr.right, row);
            return apply_binary(op, expr.type, left, right);
        }
        case BoundExpr::Kind::Aggregate:
            break;  // a row has no groups; the binder refuses this shape
    }
    throw common::Error("unknown bound expression kind");
}

// --- the group walker --------------------------------------------------------

// Column nodes read the group's key values (matched by column index —
// the binder admits only GROUP BY columns here); Aggregate nodes read
// the group's final aggregate values by inventory slot.
Value eval_group_node(const BoundExpr& expr,
                      const std::vector<std::size_t>& key_columns,
                      const std::vector<Value>& key_values,
                      const std::vector<Value>& aggregate_values) {
    switch (expr.kind) {
        case BoundExpr::Kind::Literal:
            return expr.literal;
        case BoundExpr::Kind::Column: {
            for (std::size_t i = 0; i < key_columns.size(); ++i) {
                if (key_columns[i] == expr.column_index) {
                    return key_values[i];
                }
            }
            // Internal contract: the binder admits only GROUP BY columns
            // into a group-context expression.
            assert(false && "group-context column is not a group key");
            return Value(0);
        }
        case BoundExpr::Kind::Aggregate:
            if (expr.aggregate_index >= aggregate_values.size()) {
                // Internal contract: the binder numbers aggregate slots
                // within the inventory it built.
                assert(false && "aggregate slot out of range");
                return Value(0);
            }
            return aggregate_values[expr.aggregate_index];
        case BoundExpr::Kind::Unary:
            return apply_unary(expr.un_op,
                               eval_group_node(*expr.operand, key_columns,
                                               key_values, aggregate_values));
        case BoundExpr::Kind::Binary: {
            const sql::BinaryOp op = expr.bin_op;
            if (op == sql::BinaryOp::And || op == sql::BinaryOp::Or) {
                const Value left = eval_group_node(*expr.left, key_columns,
                                                   key_values,
                                                   aggregate_values);
                const bool left_truthy = truthy(left);
                if (op == sql::BinaryOp::And && !left_truthy) {
                    return Value(0);  // short-circuit
                }
                if (op == sql::BinaryOp::Or && left_truthy) {
                    return Value(1);  // short-circuit
                }
                const Value right = eval_group_node(*expr.right, key_columns,
                                                    key_values,
                                                    aggregate_values);
                return Value(truthy(right) ? 1 : 0);
            }
            const Value left = eval_group_node(*expr.left, key_columns,
                                               key_values, aggregate_values);
            const Value right = eval_group_node(*expr.right, key_columns,
                                                key_values, aggregate_values);
            return apply_binary(op, expr.type, left, right);
        }
    }
    throw common::Error("unknown bound expression kind");
}

}

Value evaluate(const BoundExpr& expr, const tuple::Tuple& row) {
    return eval_node(expr, row);
}

Value evaluate_group(const BoundExpr& expr,
                     const std::vector<std::size_t>& key_columns,
                     const std::vector<Value>& key_values,
                     const std::vector<Value>& aggregate_values) {
    return eval_group_node(expr, key_columns, key_values, aggregate_values);
}

}

