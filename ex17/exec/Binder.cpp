#include "exec/Binder.h"

#include "common/Error.h"

#include <optional>

namespace exec {

namespace {

bool is_numeric(Type type) {
    return type == Type::Int || type == Type::Double;
}

// Binds a subexpression and returns the node; a named bind error throws.
//
// `group` selects the context: null binds rows (WHERE, group keys,
// aggregate arguments — columns are free, aggregates are refused through
// `aggregate_refusal`); non-null binds group results (select items,
// HAVING — aggregate calls join the inventory, bare columns must be
// GROUP BY columns).
BoundExprPtr bind_node(const sql::Expr& expr,
                       const schema::TableSchema& schema,
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
            return node;
        }
        case sql::Expr::Kind::ColumnRef: {
            if (!expr.table.empty()) {
                throw common::Error(
                    "table-qualified column '" + expr.table + "." + expr.column +
                    "' needs a join; single-table queries use bare names");
            }
            const std::optional<std::size_t> index =
                schema.column_index(expr.column);
            if (!index) {
                throw common::Error("unknown column '" + expr.column +
                                    "' in table '" + schema.table_name() + "'");
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
                    throw common::Error(
                        "column '" + expr.column +
                        "' must appear in GROUP BY or be inside an aggregate");
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
            return node;
        }
        case sql::Expr::Kind::Function: {
            if (group == nullptr) {
                throw common::Error(aggregate_refusal);
            }
            // The aggregate's argument sees rows, not groups: bare columns
            // are free there, and a nested aggregate is refused by name.
            BoundExprPtr arg;
            if (expr.arg != nullptr) {
                arg = bind_node(*expr.arg, schema, nullptr,
                                "aggregate calls cannot nest");
            }
            Type result_type = Type::Int;
            switch (expr.fn) {
                case sql::FunctionCode::Count:
                    result_type = Type::Int;  // no NULLs: COUNT(col) == COUNT(*)
                    break;
                case sql::FunctionCode::Avg:
                    if (arg == nullptr || !is_numeric(arg->type)) {
                        throw common::Error(
                            std::string("AVG needs a numeric argument, got ") +
                            (arg == nullptr ? "no argument"
                                            : type_name(arg->type)));
                    }
                    result_type = Type::Double;
                    break;
                case sql::FunctionCode::Sum:
                    if (arg == nullptr || !is_numeric(arg->type)) {
                        throw common::Error(
                            std::string("SUM needs a numeric argument, got ") +
                            (arg == nullptr ? "no argument"
                                            : type_name(arg->type)));
                    }
                    result_type = arg->type;
                    break;
                case sql::FunctionCode::Min:
                case sql::FunctionCode::Max:
                    if (arg == nullptr || arg->type == Type::Bool) {
                        throw common::Error(
                            std::string(expr.fn == sql::FunctionCode::Min
                                            ? "MIN"
                                            : "MAX") +
                            " needs a numeric or string argument" +
                            (arg == nullptr
                                 ? std::string(", got no argument")
                                 : std::string(", got ") + type_name(arg->type)));
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
            return node;
        }
        case sql::Expr::Kind::Unary: {
            BoundExprPtr operand = bind_node(*expr.operand, schema, group,
                                             aggregate_refusal);
            auto node = std::make_unique<BoundExpr>();
            node->kind = BoundExpr::Kind::Unary;
            node->un_op = expr.un_op;
            node->operand = std::move(operand);
            switch (expr.un_op) {
                case sql::UnaryOp::Neg:
                    if (!is_numeric(node->operand->type)) {
                        throw common::Error(
                            std::string("unary '-' needs a numeric operand, got ") +
                            type_name(node->operand->type));
                    }
                    node->type = node->operand->type;
                    break;
                case sql::UnaryOp::Not:
                    if (node->operand->type != Type::Bool) {
                        throw common::Error(
                            std::string("NOT needs a boolean operand, got ") +
                            type_name(node->operand->type));
                    }
                    node->type = Type::Bool;
                    break;
            }
            return node;
        }
        case sql::Expr::Kind::Binary: {
            BoundExprPtr left = bind_node(*expr.left, schema, group,
                                          aggregate_refusal);
            BoundExprPtr right = bind_node(*expr.right, schema, group,
                                           aggregate_refusal);
            auto node = std::make_unique<BoundExpr>();
            node->kind = BoundExpr::Kind::Binary;
            node->bin_op = expr.bin_op;
            node->left = std::move(left);
            node->right = std::move(right);
            const sql::BinaryOp op = expr.bin_op;
            if (op == sql::BinaryOp::And || op == sql::BinaryOp::Or) {
                if (node->left->type != Type::Bool ||
                    node->right->type != Type::Bool) {
                    throw common::Error(
                        std::string(
                            op == sql::BinaryOp::And ? "AND" : "OR") +
                        " needs boolean operands, got " +
                        type_name(node->left->type) + " and " +
                        type_name(node->right->type));
                }
                node->type = Type::Bool;
            } else if (op == sql::BinaryOp::Eq || op == sql::BinaryOp::Ne ||
                       op == sql::BinaryOp::Lt || op == sql::BinaryOp::Le ||
                       op == sql::BinaryOp::Gt || op == sql::BinaryOp::Ge) {
                const bool both_numeric =
                    is_numeric(node->left->type) && is_numeric(node->right->type);
                const bool both_string =
                    node->left->type == Type::String &&
                    node->right->type == Type::String;
                if (!both_numeric && !both_string) {
                    throw common::Error(
                        std::string("cannot compare ") +
                        type_name(node->left->type) + " with " +
                        type_name(node->right->type));
                }
                node->type = Type::Bool;
            } else {  // Plus, Minus, Mul, Div, Mod
                if (!is_numeric(node->left->type) ||
                    !is_numeric(node->right->type)) {
                    throw common::Error(
                        std::string("arithmetic needs numeric operands, got ") +
                        type_name(node->left->type) + " and " +
                        type_name(node->right->type));
                }
                if (op == sql::BinaryOp::Mod) {
                    if (node->left->type != Type::Int ||
                        node->right->type != Type::Int) {
                        throw common::Error(
                            std::string("MOD needs integer operands, got ") +
                            type_name(node->left->type) + " and " +
                            type_name(node->right->type));
                    }
                    node->type = Type::Int;
                } else {
                    node->type = (node->left->type == Type::Int &&
                                  node->right->type == Type::Int)
                                     ? Type::Int
                                     : Type::Double;
                }
            }
            return node;
        }
    }
    throw common::Error("unknown expression kind");
}

}

BoundExprPtr bind_expr(const sql::Expr& expr, const schema::TableSchema& schema) {
    return bind_node(expr, schema);
}

BoundExprPtr bind_group_expr(const sql::Expr& expr,
                             const schema::TableSchema& schema,
                             GroupBinding& group) {
    return bind_node(expr, schema, &group, "aggregate calls cannot nest");
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

