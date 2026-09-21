#include "AstPrinter.h"

#include <charconv>
#include <sstream>

namespace sql {

namespace {

// Higher binds tighter. Not (3) sits between And (2) and comparison (4);
// Neg (7) sits just under primaries (8), matching the grammar.
int precedence(const Expr& expr) {
    switch (expr.kind) {
        case Expr::Kind::Literal:
        case Expr::Kind::ColumnRef:
        case Expr::Kind::Function:
            return 8;
        case Expr::Kind::Unary:
            return expr.un_op == UnaryOp::Not ? 3 : 7;
        case Expr::Kind::Binary:
            switch (expr.bin_op) {
                case BinaryOp::Or: return 1;
                case BinaryOp::And: return 2;
                case BinaryOp::Eq:
                case BinaryOp::Ne:
                case BinaryOp::Lt:
                case BinaryOp::Le:
                case BinaryOp::Gt:
                case BinaryOp::Ge:
                    return 4;
                case BinaryOp::Plus:
                case BinaryOp::Minus:
                    return 5;
                case BinaryOp::Mul:
                case BinaryOp::Div:
                case BinaryOp::Mod:
                    return 6;
            }
            return 8;
    }
    return 8;
}

std::string parenthesize_if_needed(const Expr& expr, int min_precedence) {
    const std::string text = print_expr(expr);
    if (precedence(expr) < min_precedence) return "(" + text + ")";
    return text;
}

std::string double_to_text(double value) {
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    std::string text(buffer, result.ptr);
    if (text.find('e') != std::string::npos || text.find('E') != std::string::npos) {
        std::ostringstream out;
        out << std::fixed << value;
        text = out.str();
    }
    // A trailing ".0" keeps whole doubles lexing as doubles on re-parse.
    if (text.find('.') == std::string::npos && text.find('e') == std::string::npos) {
        text += ".0";
    }
    return text;
}

std::string string_to_text(const std::string& value) {
    std::string text = "'";
    for (const char c : value) {
        if (c == '\'') text += "''";
        else text += c;
    }
    text += "'";
    return text;
}

}

// Label pieces shared with the tree renderer; the header declares them.
std::string literal_text(const LiteralValue& value) {
    switch (value.tag) {
        case LiteralValue::Tag::Int: return std::to_string(value.i);
        case LiteralValue::Tag::Double: return double_to_text(value.d);
        case LiteralValue::Tag::String: return string_to_text(value.s);
    }
    return "";
}

std::string binary_op_text(BinaryOp op) {
    switch (op) {
        case BinaryOp::Eq: return "=";
        case BinaryOp::Ne: return "<>";
        case BinaryOp::Lt: return "<";
        case BinaryOp::Le: return "<=";
        case BinaryOp::Gt: return ">";
        case BinaryOp::Ge: return ">=";
        case BinaryOp::And: return "AND";
        case BinaryOp::Or: return "OR";
        case BinaryOp::Plus: return "+";
        case BinaryOp::Minus: return "-";
        case BinaryOp::Mul: return "*";
        case BinaryOp::Div: return "/";
        case BinaryOp::Mod: return "%";
    }
    return "";
}

std::string function_name(FunctionCode code) {
    switch (code) {
        case FunctionCode::Count: return "COUNT";
        case FunctionCode::Sum: return "SUM";
        case FunctionCode::Avg: return "AVG";
        case FunctionCode::Min: return "MIN";
        case FunctionCode::Max: return "MAX";
    }
    return "";
}

namespace {

std::string print_binary(const Expr& expr) {
    const int my = precedence(expr);
    // The parser folds left, so only the right operand needs parentheses
    // at equal precedence.
    const std::string left = parenthesize_if_needed(*expr.left, my);
    const std::string right = parenthesize_if_needed(*expr.right, my + 1);
    return left + " " + binary_op_text(expr.bin_op) + " " + right;
}

std::string print_unary(const Expr& expr) {
    const std::string operand = parenthesize_if_needed(*expr.operand, precedence(expr));
    if (expr.un_op == UnaryOp::Not) return "NOT " + operand;
    // "- -3" with no space would lex as a line comment.
    if (!operand.empty() && operand[0] == '-') return "- " + operand;
    return "-" + operand;
}

std::string print_function(const Expr& expr) {
    if (expr.arg == nullptr) return function_name(expr.fn) + "(*)";
    return function_name(expr.fn) + "(" + print_expr(*expr.arg) + ")";
}

std::string print_literal(const Expr& expr) {
    return literal_text(expr.value);
}

std::string data_type_text(const ColumnDef& column) {
    switch (column.type) {
        case DataType::Integer: return "INTEGER";
        case DataType::Double: return "DOUBLE";
        case DataType::String: return "VARCHAR(" + std::to_string(column.size) + ")";
    }
    return "";
}

std::string print_select_item(const SelectItem& item) {
    if (item.star) return "*";
    std::string text = print_expr(*item.expr);
    if (!item.alias.empty()) text += " AS " + item.alias;
    return text;
}

std::string print_table_ref(const TableRef& table) {
    std::string text = table.name;
    if (!table.alias.empty()) text += " AS " + table.alias;
    return text;
}

std::string print_select(const SelectStatement& statement) {
    std::string text = "SELECT ";
    for (std::size_t i = 0; i < statement.items.size(); ++i) {
        if (i > 0) text += ", ";
        text += print_select_item(statement.items[i]);
    }
    text += " FROM " + print_table_ref(statement.from);
    for (const JoinClause& join : statement.joins) {
        text += " JOIN " + print_table_ref(join.table) + " ON " + print_expr(*join.on);
    }
    if (statement.where) text += " WHERE " + print_expr(*statement.where);
    if (!statement.group_by.empty()) {
        text += " GROUP BY ";
        for (std::size_t i = 0; i < statement.group_by.size(); ++i) {
            if (i > 0) text += ", ";
            text += print_expr(*statement.group_by[i]);
        }
    }
    if (!statement.order_by.empty()) {
        text += " ORDER BY ";
        for (std::size_t i = 0; i < statement.order_by.size(); ++i) {
            if (i > 0) text += ", ";
            text += print_expr(*statement.order_by[i].expr);
            if (statement.order_by[i].descending) text += " DESC";
        }
    }
    if (statement.has_limit) text += " LIMIT " + std::to_string(statement.limit);
    return text;
}

}

std::string print_expr(const Expr& expr) {
    switch (expr.kind) {
        case Expr::Kind::Literal: return print_literal(expr);
        case Expr::Kind::ColumnRef:
            return expr.table.empty() ? expr.column : expr.table + "." + expr.column;
        case Expr::Kind::Binary: return print_binary(expr);
        case Expr::Kind::Unary: return print_unary(expr);
        case Expr::Kind::Function: return print_function(expr);
    }
    return "";
}

std::string print(const Statement& statement) {
    switch (statement.kind) {
        case Statement::Kind::CreateTable: {
            const auto& create = static_cast<const CreateTableStatement&>(statement);
            std::string text = "CREATE TABLE " + create.table + " (";
            for (std::size_t i = 0; i < create.columns.size(); ++i) {
                if (i > 0) text += ", ";
                text += create.columns[i].name + " " + data_type_text(create.columns[i]);
            }
            return text + ")";
        }
        case Statement::Kind::DropTable:
            return "DROP TABLE " + static_cast<const DropTableStatement&>(statement).table;
        case Statement::Kind::Insert: {
            const auto& insert = static_cast<const InsertStatement&>(statement);
            std::string text = "INSERT INTO " + insert.table + " VALUES ";
            for (std::size_t i = 0; i < insert.rows.size(); ++i) {
                if (i > 0) text += ", ";
                text += "(";
                for (std::size_t j = 0; j < insert.rows[i].size(); ++j) {
                    if (j > 0) text += ", ";
                    text += print_expr(*insert.rows[i][j]);
                }
                text += ")";
            }
            return text;
        }
        case Statement::Kind::Update: {
            const auto& update = static_cast<const UpdateStatement&>(statement);
            std::string text = "UPDATE " + update.table + " SET ";
            for (std::size_t i = 0; i < update.sets.size(); ++i) {
                if (i > 0) text += ", ";
                text += update.sets[i].column + " = " + print_expr(*update.sets[i].value);
            }
            if (update.where) text += " WHERE " + print_expr(*update.where);
            return text;
        }
        case Statement::Kind::Delete: {
            const auto& del = static_cast<const DeleteStatement&>(statement);
            std::string text = "DELETE FROM " + del.table;
            if (del.where) text += " WHERE " + print_expr(*del.where);
            return text;
        }
        case Statement::Kind::Select:
            return print_select(static_cast<const SelectStatement&>(statement));
    }
    return "";
}

}
