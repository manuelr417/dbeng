#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sql {

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

// Mirrors the catalog's column types one-for-one (ex13 writes
// tablecatalog/<name>.json without translation); size carries VARCHAR(n).
enum class DataType { Integer, String, Double };

struct LiteralValue {
    enum class Tag { Int, Double, String };
    Tag tag = Tag::Int;
    std::int64_t i = 0;
    double d = 0.0;
    std::string s;
};

enum class BinaryOp { Eq, Ne, Lt, Le, Gt, Ge, And, Or, Plus, Minus, Mul, Div, Mod };
enum class UnaryOp { Neg, Not };
enum class FunctionCode { Count, Sum, Avg, Min, Max };

// One tagged struct for all five expression kinds; exactly one payload
// view is live per kind — tree walks switch on kind and never guess.
struct Expr {
    enum class Kind { Literal, ColumnRef, Binary, Unary, Function };
    Kind kind = Kind::Literal;
    LiteralValue value;                    // Literal
    std::string table;                     // ColumnRef, "" when unqualified
    std::string column;                    // ColumnRef
    BinaryOp bin_op = BinaryOp::Eq;        // Binary
    ExprPtr left;                          // Binary
    ExprPtr right;                         // Binary
    UnaryOp un_op = UnaryOp::Not;          // Unary
    ExprPtr operand;                       // Unary
    FunctionCode fn = FunctionCode::Count; // Function
    ExprPtr arg;                           // Function, nullptr == COUNT(*)
};

struct SelectItem { ExprPtr expr; std::string alias; bool star = false; };
struct OrderItem  { ExprPtr expr; bool descending = false; };
struct TableRef   { std::string name; std::string alias; };  // alias "" if none
struct JoinClause { TableRef table; ExprPtr on; };
struct ColumnDef  { std::string name; DataType type = DataType::Integer; std::size_t size = 0; };
struct SetClause  { std::string column; ExprPtr value; };

struct Statement {
    enum class Kind { CreateTable, DropTable, Insert, Update, Delete, Select };
    Kind kind;
    virtual ~Statement() = default;

protected:
    explicit Statement(Kind k) : kind(k) {}
};

struct CreateTableStatement : Statement {
    std::string table;
    std::vector<ColumnDef> columns;
    CreateTableStatement() : Statement(Kind::CreateTable) {}
};

struct DropTableStatement : Statement {
    std::string table;
    DropTableStatement() : Statement(Kind::DropTable) {}
};

struct InsertStatement : Statement {
    std::string table;
    std::vector<std::vector<ExprPtr>> rows;
    InsertStatement() : Statement(Kind::Insert) {}
};

struct UpdateStatement : Statement {
    std::string table;
    std::vector<SetClause> sets;
    ExprPtr where;
    UpdateStatement() : Statement(Kind::Update) {}
};

struct DeleteStatement : Statement {
    std::string table;
    ExprPtr where;
    DeleteStatement() : Statement(Kind::Delete) {}
};

struct SelectStatement : Statement {
    std::vector<SelectItem> items;
    TableRef from;
    std::vector<JoinClause> joins;
    ExprPtr where;
    std::vector<ExprPtr> group_by;
    std::vector<OrderItem> order_by;
    std::uint64_t limit = 0;
    bool has_limit = false;
    SelectStatement() : Statement(Kind::Select) {}
};

// --- structural equality (round-trip checks compare whole trees) ---

inline bool operator==(const Expr& a, const Expr& b);

inline bool expr_equal(const ExprPtr& a, const ExprPtr& b) {
    if (a == nullptr || b == nullptr) return a == b;
    return *a == *b;
}

inline bool exprs_equal(const std::vector<ExprPtr>& a, const std::vector<ExprPtr>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!expr_equal(a[i], b[i])) return false;
    }
    return true;
}

inline bool operator==(const LiteralValue& a, const LiteralValue& b) {
    if (a.tag != b.tag) return false;
    switch (a.tag) {
        case LiteralValue::Tag::Int: return a.i == b.i;
        case LiteralValue::Tag::Double: return a.d == b.d;
        case LiteralValue::Tag::String: return a.s == b.s;
    }
    return false;
}

inline bool operator==(const Expr& a, const Expr& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case Expr::Kind::Literal: return a.value == b.value;
        case Expr::Kind::ColumnRef: return a.table == b.table && a.column == b.column;
        case Expr::Kind::Binary:
            return a.bin_op == b.bin_op && expr_equal(a.left, b.left) &&
                   expr_equal(a.right, b.right);
        case Expr::Kind::Unary: return a.un_op == b.un_op && expr_equal(a.operand, b.operand);
        case Expr::Kind::Function: return a.fn == b.fn && expr_equal(a.arg, b.arg);
    }
    return false;
}

inline bool operator==(const SelectItem& a, const SelectItem& b) {
    return a.star == b.star && a.alias == b.alias && expr_equal(a.expr, b.expr);
}

inline bool operator==(const OrderItem& a, const OrderItem& b) {
    return a.descending == b.descending && expr_equal(a.expr, b.expr);
}

inline bool operator==(const TableRef& a, const TableRef& b) {
    return a.name == b.name && a.alias == b.alias;
}

inline bool operator==(const JoinClause& a, const JoinClause& b) {
    return a.table == b.table && expr_equal(a.on, b.on);
}

inline bool operator==(const ColumnDef& a, const ColumnDef& b) {
    return a.name == b.name && a.type == b.type && a.size == b.size;
}

inline bool operator==(const SetClause& a, const SetClause& b) {
    return a.column == b.column && expr_equal(a.value, b.value);
}

// Kind determines the dynamic type, so the casts below are exact.
inline bool operator==(const Statement& a, const Statement& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case Statement::Kind::CreateTable: {
            const auto& x = static_cast<const CreateTableStatement&>(a);
            const auto& y = static_cast<const CreateTableStatement&>(b);
            return x.table == y.table && x.columns == y.columns;
        }
        case Statement::Kind::DropTable: {
            const auto& x = static_cast<const DropTableStatement&>(a);
            const auto& y = static_cast<const DropTableStatement&>(b);
            return x.table == y.table;
        }
        case Statement::Kind::Insert: {
            const auto& x = static_cast<const InsertStatement&>(a);
            const auto& y = static_cast<const InsertStatement&>(b);
            if (x.table != y.table || x.rows.size() != y.rows.size()) return false;
            for (std::size_t i = 0; i < x.rows.size(); ++i) {
                if (!exprs_equal(x.rows[i], y.rows[i])) return false;
            }
            return true;
        }
        case Statement::Kind::Update: {
            const auto& x = static_cast<const UpdateStatement&>(a);
            const auto& y = static_cast<const UpdateStatement&>(b);
            return x.table == y.table && x.sets == y.sets && expr_equal(x.where, y.where);
        }
        case Statement::Kind::Delete: {
            const auto& x = static_cast<const DeleteStatement&>(a);
            const auto& y = static_cast<const DeleteStatement&>(b);
            return x.table == y.table && expr_equal(x.where, y.where);
        }
        case Statement::Kind::Select: {
            const auto& x = static_cast<const SelectStatement&>(a);
            const auto& y = static_cast<const SelectStatement&>(b);
            return x.items == y.items && x.from == y.from && x.joins == y.joins &&
                   expr_equal(x.where, y.where) && exprs_equal(x.group_by, y.group_by) &&
                   x.order_by == y.order_by && x.limit == y.limit && x.has_limit == y.has_limit;
        }
    }
    return false;
}

}
