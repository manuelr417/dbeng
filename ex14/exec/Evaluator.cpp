#include "exec/Evaluator.h"

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

bool eval_node(const BoundExpr& expr, const tuple::Tuple& row, Value& out,
               std::string& error) {
    switch (expr.kind) {
        case BoundExpr::Kind::Literal:
            out = expr.literal;
            return true;
        case BoundExpr::Kind::Column:
            out = column_value(row.column(expr.column_index));
            return true;
        case BoundExpr::Kind::Unary: {
            Value operand;
            if (!eval_node(*expr.operand, row, operand, error)) {
                return false;
            }
            if (expr.un_op == sql::UnaryOp::Neg) {
                if (const int* v = std::get_if<int>(&operand)) {
                    out = Value(-*v);
                } else {
                    out = Value(-std::get<double>(operand));
                }
            } else {  // Not
                out = Value(truthy(operand) ? 0 : 1);
            }
            return true;
        }
        case BoundExpr::Kind::Binary: {
            const sql::BinaryOp op = expr.bin_op;
            if (op == sql::BinaryOp::And) {
                Value left;
                if (!eval_node(*expr.left, row, left, error)) {
                    return false;
                }
                if (!truthy(left)) {
                    out = Value(0);  // short-circuit
                    return true;
                }
                Value right;
                if (!eval_node(*expr.right, row, right, error)) {
                    return false;
                }
                out = Value(truthy(right) ? 1 : 0);
                return true;
            }
            if (op == sql::BinaryOp::Or) {
                Value left;
                if (!eval_node(*expr.left, row, left, error)) {
                    return false;
                }
                if (truthy(left)) {
                    out = Value(1);  // short-circuit
                    return true;
                }
                Value right;
                if (!eval_node(*expr.right, row, right, error)) {
                    return false;
                }
                out = Value(truthy(right) ? 1 : 0);
                return true;
            }
            Value left;
            Value right;
            if (!eval_node(*expr.left, row, left, error) ||
                !eval_node(*expr.right, row, right, error)) {
                return false;  // error already set
            }
            if (op == sql::BinaryOp::Eq || op == sql::BinaryOp::Ne ||
                op == sql::BinaryOp::Lt || op == sql::BinaryOp::Le ||
                op == sql::BinaryOp::Gt || op == sql::BinaryOp::Ge) {
                out = Value(compare_op(op, compare_values(left, right)));
                return true;
            }
            // Arithmetic: promotion per the binder's static type.
            if (op == sql::BinaryOp::Div || op == sql::BinaryOp::Mod) {
                const bool integer = expr.type == Type::Int;
                if (integer) {
                    const int r = std::get<int>(right);
                    if (r == 0) {
                        error = op == sql::BinaryOp::Div
                                    ? "division by zero"
                                    : "modulo by zero";
                        return false;
                    }
                    const int l = std::get<int>(left);
                    out = Value(op == sql::BinaryOp::Div ? l / r : l % r);
                } else {
                    const double r = numeric(right);
                    if (r == 0.0) {
                        error = "division by zero";
                        return false;
                    }
                    out = Value(numeric(left) / r);
                }
                return true;
            }
            if (expr.type == Type::Int) {
                const int l = std::get<int>(left);
                const int r = std::get<int>(right);
                switch (op) {
                    case sql::BinaryOp::Plus: out = Value(l + r); break;
                    case sql::BinaryOp::Minus: out = Value(l - r); break;
                    case sql::BinaryOp::Mul: out = Value(l * r); break;
                    default: out = Value(0); break;
                }
            } else {
                const double l = numeric(left);
                const double r = numeric(right);
                switch (op) {
                    case sql::BinaryOp::Plus: out = Value(l + r); break;
                    case sql::BinaryOp::Minus: out = Value(l - r); break;
                    case sql::BinaryOp::Mul: out = Value(l * r); break;
                    default: out = Value(0.0); break;
                }
            }
            return true;
        }
    }
    error = "unknown bound expression kind";
    return false;
}

}

bool evaluate(const BoundExpr& expr, const tuple::Tuple& row, Value& out,
              std::string& error) {
    error.clear();
    return eval_node(expr, row, out, error);
}

}
