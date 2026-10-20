#include "exec/Binder.h"

#include <optional>

namespace exec {

namespace {

bool is_numeric(Type type) {
    return type == Type::Int || type == Type::Double;
}

// Binds a subexpression and returns its type; nullopt on error (the
// error string already carries the message).
//
// `group` selects the context: null binds rows (WHERE, group keys,
// aggregate arguments — columns are free, aggregates are refused through
// `aggregate_refusal`); non-null binds group results (select items,
// HAVING — aggregate calls join the inventory, bare columns must be
// GROUP BY columns).
std::optional<Type> bind_node(const sql::Expr& expr,
                              const schema::TableSchema& schema,
                              BoundExprPtr& out, std::string& error,
                              GroupBinding* group = nullptr,
                              const char* aggregate_refusal =
                                  "aggregates are not allowed in WHERE") {
    switch (expr.kind) {
        case sql::Expr::Kind::Literal: {
            auto node = std::make_unique<BoundExpr>();
            node->kind = BoundExpr::Kind::Literal;
            const sql::LiteralValue& value = expr.value;
            switch (value.tag) {
                case sql::LiteralValue::Tag::Int:
                    node->type = Type::Int;
                    node->literal = Value(static_cast<int>(value.i));
                    break;
                case sql::LiteralValue::Tag::Double:
                    node->type = Type::Double;
                    node->literal = Value(value.d);
                    break;
                case sql::LiteralValue::Tag::String:
                    node->type = Type::String;
                    node->literal = Value(value.s);
                    break;
            }
            out = std::move(node);
            return out->type;
        }
        case sql::Expr::Kind::ColumnRef: {
            if (!expr.table.empty()) {
                error = "table-qualified column '" + expr.table + "." +
                        expr.column +
                        "' needs a join; single-table queries use bare names";
                return std::nullopt;
            }
            const std::optional<std::size_t> index =
                schema.column_index(expr.column);
            if (!index) {
                error = "unknown column '" + expr.column + "' in table '" +
                        schema.table_name() + "'";
                return std::nullopt;
            }
            if (group != nullptr) {
                bool is_key = false;
                for (const std::size_t key : group->key_columns) {
                    if (key == *index) {
                        is_key = true;
                        break;
                    }
                }
                if (!is_key) {
                    error = "column '" + expr.column +
                            "' must appear in GROUP BY or be inside an aggregate";
                    return std::nullopt;
                }
            }
            auto node = std::make_unique<BoundExpr>();
            node->kind = BoundExpr::Kind::Column;
            node->column_index = *index;
            switch (schema.column_info(*index).column_type()) {
                case tuple::ColumnType::Integer: node->type = Type::Int; break;
                case tuple::ColumnType::Double: node->type = Type::Double; break;
                case tuple::ColumnType::String: node->type = Type::String; break;
            }
            out = std::move(node);
            return out->type;
        }
        case sql::Expr::Kind::Function: {
            if (group == nullptr) {
                error = aggregate_refusal;
                return std::nullopt;
            }
            // The aggregate's argument sees rows, not groups: bare columns
            // are free there, and a nested aggregate is refused by name.
            BoundExprPtr arg;
            if (expr.arg != nullptr &&
                !bind_node(*expr.arg, schema, arg, error, nullptr,
                           "aggregate calls cannot nest")) {
                return std::nullopt;
            }
            Type result_type = Type::Int;
            switch (expr.fn) {
                case sql::FunctionCode::Count:
                    result_type = Type::Int;  // no NULLs: COUNT(col) == COUNT(*)
                    break;
                case sql::FunctionCode::Avg:
                    if (arg == nullptr || !is_numeric(arg->type)) {
                        error = std::string("AVG needs a numeric argument, got ") +
                                (arg == nullptr ? "no argument"
                                                : type_name(arg->type));
                        return std::nullopt;
                    }
                    result_type = Type::Double;
                    break;
                case sql::FunctionCode::Sum:
                    if (arg == nullptr || !is_numeric(arg->type)) {
                        error = std::string("SUM needs a numeric argument, got ") +
                                (arg == nullptr ? "no argument"
                                                : type_name(arg->type));
                        return std::nullopt;
                    }
                    result_type = arg->type;
                    break;
                case sql::FunctionCode::Min:
                case sql::FunctionCode::Max:
                    if (arg == nullptr || arg->type == Type::Bool) {
                        error = std::string(expr.fn == sql::FunctionCode::Min
                                                ? "MIN"
                                                : "MAX") +
                                " needs a numeric or string argument" +
                                (arg == nullptr
                                     ? std::string(", got no argument")
                                     : std::string(", got ") + type_name(arg->type));
                        return std::nullopt;
                    }
                    result_type = arg->type;
                    break;
            }
            // Dedup by structural identity: the same call spelled twice
            // shares one state slot.
            std::size_t index = group->aggregates.size();
            for (std::size_t i = 0; i < group->aggregates.size(); ++i) {
                const BoundAggregate& existing = group->aggregates[i];
                if (existing.fn == expr.fn &&
                    (existing.arg == nullptr) == (arg == nullptr) &&
                    (arg == nullptr || bound_equal(*existing.arg, *arg))) {
                    index = i;
                    break;
                }
            }
            if (index == group->aggregates.size()) {
                BoundAggregate entry;
                entry.fn = expr.fn;
                entry.arg = std::move(arg);
                entry.type = result_type;
                group->aggregates.push_back(std::move(entry));
            }
            auto node = std::make_unique<BoundExpr>();
            node->kind = BoundExpr::Kind::Aggregate;
            node->fn = expr.fn;
            node->type = result_type;
            node->aggregate_index = index;
            out = std::move(node);
            return out->type;
        }
        case sql::Expr::Kind::Unary: {
            BoundExprPtr operand;
            const std::optional<Type> operand_type =
                bind_node(*expr.operand, schema, operand, error, group,
                          aggregate_refusal);
            if (!operand_type) {
                return std::nullopt;
            }
            auto node = std::make_unique<BoundExpr>();
            node->kind = BoundExpr::Kind::Unary;
            node->un_op = expr.un_op;
            node->operand = std::move(operand);
            switch (expr.un_op) {
                case sql::UnaryOp::Neg:
                    if (!is_numeric(*operand_type)) {
                        error = std::string("unary '-' needs a numeric operand, got ") +
                                type_name(*operand_type);
                        return std::nullopt;
                    }
                    node->type = *operand_type;
                    break;
                case sql::UnaryOp::Not:
                    if (*operand_type != Type::Bool) {
                        error = std::string("NOT needs a boolean operand, got ") +
                                type_name(*operand_type);
                        return std::nullopt;
                    }
                    node->type = Type::Bool;
                    break;
            }
            out = std::move(node);
            return out->type;
        }
        case sql::Expr::Kind::Binary: {
            BoundExprPtr left;
            BoundExprPtr right;
            const std::optional<Type> left_type =
                bind_node(*expr.left, schema, left, error, group,
                          aggregate_refusal);
            if (!left_type) {
                return std::nullopt;
            }
            const std::optional<Type> right_type =
                bind_node(*expr.right, schema, right, error, group,
                          aggregate_refusal);
            if (!right_type) {
                return std::nullopt;
            }
            auto node = std::make_unique<BoundExpr>();
            node->kind = BoundExpr::Kind::Binary;
            node->bin_op = expr.bin_op;
            node->left = std::move(left);
            node->right = std::move(right);
            const sql::BinaryOp op = expr.bin_op;
            if (op == sql::BinaryOp::And || op == sql::BinaryOp::Or) {
                if (*left_type != Type::Bool || *right_type != Type::Bool) {
                    error = std::string(
                                op == sql::BinaryOp::And ? "AND" : "OR") +
                            " needs boolean operands, got " +
                            type_name(*left_type) + " and " +
                            type_name(*right_type);
                    return std::nullopt;
                }
                node->type = Type::Bool;
            } else if (op == sql::BinaryOp::Eq || op == sql::BinaryOp::Ne ||
                       op == sql::BinaryOp::Lt || op == sql::BinaryOp::Le ||
                       op == sql::BinaryOp::Gt || op == sql::BinaryOp::Ge) {
                const bool both_numeric =
                    is_numeric(*left_type) && is_numeric(*right_type);
                const bool both_string =
                    *left_type == Type::String && *right_type == Type::String;
                if (!both_numeric && !both_string) {
                    error = std::string("cannot compare ") +
                            type_name(*left_type) + " with " +
                            type_name(*right_type);
                    return std::nullopt;
                }
                node->type = Type::Bool;
            } else {  // Plus, Minus, Mul, Div, Mod
                if (!is_numeric(*left_type) || !is_numeric(*right_type)) {
                    error = std::string("arithmetic needs numeric operands, got ") +
                            type_name(*left_type) + " and " +
                            type_name(*right_type);
                    return std::nullopt;
                }
                if (op == sql::BinaryOp::Mod) {
                    if (*left_type != Type::Int || *right_type != Type::Int) {
                        error = std::string("MOD needs integer operands, got ") +
                                type_name(*left_type) + " and " +
                                type_name(*right_type);
                        return std::nullopt;
                    }
                    node->type = Type::Int;
                } else {
                    node->type = (*left_type == Type::Int &&
                                  *right_type == Type::Int)
                                     ? Type::Int
                                     : Type::Double;
                }
            }
            out = std::move(node);
            return out->type;
        }
    }
    error = "unknown expression kind";
    return std::nullopt;
}

}

bool bind_expr(const sql::Expr& expr, const schema::TableSchema& schema,
               BoundExprPtr& out, std::string& error) {
    out.reset();
    error.clear();
    return bind_node(expr, schema, out, error).has_value();
}

bool bind_group_expr(const sql::Expr& expr, const schema::TableSchema& schema,
                     GroupBinding& group, BoundExprPtr& out, std::string& error) {
    out.reset();
    error.clear();
    return bind_node(expr, schema, out, error, &group,
                     "aggregate calls cannot nest")
        .has_value();
}

bool bound_equal(const BoundExpr& a, const BoundExpr& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case BoundExpr::Kind::Literal:
            return a.literal == b.literal;
        case BoundExpr::Kind::Column:
            return a.column_index == b.column_index;
        case BoundExpr::Kind::Aggregate:
            return a.fn == b.fn && a.aggregate_index == b.aggregate_index;
        case BoundExpr::Kind::Unary:
            return a.un_op == b.un_op && bound_equal(*a.operand, *b.operand);
        case BoundExpr::Kind::Binary:
            return a.bin_op == b.bin_op && bound_equal(*a.left, *b.left) &&
                   bound_equal(*a.right, *b.right);
    }
    return false;
}

}
