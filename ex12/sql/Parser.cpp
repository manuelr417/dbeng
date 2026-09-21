#include "Parser.h"

#include "AstPrinter.h"
#include "Lexer.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>

namespace sql {

namespace {

using StatementPtr = std::unique_ptr<Statement>;

std::string lowered(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

bool int64_from(const std::string& text, std::int64_t& out) {
    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0') return false;
    out = value;
    return true;
}

bool double_from(const std::string& text, double& out) {
    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (errno == ERANGE || end == text.c_str() || *end != '\0') return false;
    out = value;
    return true;
}

ExprPtr make_literal(LiteralValue value) {
    auto expr = std::make_unique<Expr>();
    expr->kind = Expr::Kind::Literal;
    expr->value = std::move(value);
    return expr;
}

ExprPtr make_column_ref(std::string table, std::string column) {
    auto expr = std::make_unique<Expr>();
    expr->kind = Expr::Kind::ColumnRef;
    expr->table = std::move(table);
    expr->column = std::move(column);
    return expr;
}

ExprPtr make_binary(BinaryOp op, ExprPtr left, ExprPtr right) {
    auto expr = std::make_unique<Expr>();
    expr->kind = Expr::Kind::Binary;
    expr->bin_op = op;
    expr->left = std::move(left);
    expr->right = std::move(right);
    return expr;
}

ExprPtr make_unary(UnaryOp op, ExprPtr operand) {
    auto expr = std::make_unique<Expr>();
    expr->kind = Expr::Kind::Unary;
    expr->un_op = op;
    expr->operand = std::move(operand);
    return expr;
}

ExprPtr make_function(FunctionCode fn, ExprPtr arg) {
    auto expr = std::make_unique<Expr>();
    expr->kind = Expr::Kind::Function;
    expr->fn = fn;
    expr->arg = std::move(arg);
    return expr;
}

// One function per grammar rule (sql.md), consuming tokens through the
// cursor below. Every rule returns nullptr / false with the error set
// once it cannot continue; callers propagate without touching error_.
class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens) : tokens_(tokens) {}

    bool run(ParseResult& result, std::string& error) {
        error_ = &error;
        while (peek().type != TokenType::End) {
            while (accept(TokenType::Semicolon)) {
            }
            if (peek().type == TokenType::End) break;
            auto statement = parse_statement();
            if (!statement) break;
            result.statements.push_back(std::move(statement));
            if (peek().type == TokenType::Semicolon) {
                advance();
            } else if (peek().type != TokenType::End) {
                fail("expected ';'");
                break;
            }
        }
        if (!ok_) {
            result.statements.clear();
            return false;
        }
        return true;
    }

private:
    // --- token cursor ---

    const Token& peek(std::size_t ahead = 0) const {
        const std::size_t i = pos_ + ahead;
        return i < tokens_.size() ? tokens_[i] : tokens_.back();
    }

    const Token& advance() {
        if (pos_ < tokens_.size() - 1) return tokens_[pos_++];
        return tokens_.back();  // never step past the End token
    }

    bool accept(TokenType type) {
        if (peek().type != type) return false;
        advance();
        return true;
    }

    bool expect(TokenType type, const std::string& what) {
        if (!accept(type)) return fail("expected " + what);
        return true;
    }

    std::string expect_identifier(const std::string& what = "identifier") {
        if (peek().type != TokenType::Identifier) {
            fail("expected " + what);
            return "";
        }
        return advance().text;
    }

    bool fail(const std::string& message) { return fail_at(peek().line, message); }

    bool fail_at(std::size_t line, const std::string& message) {
        if (ok_) {
            *error_ = "line " + std::to_string(line) + ": " + message;
            ok_ = false;
        }
        return false;
    }

    // --- statements ---

    StatementPtr parse_statement() {
        switch (peek().type) {
            case TokenType::Create: return parse_create_table();
            case TokenType::Drop: return parse_drop_table();
            case TokenType::Insert: return parse_insert();
            case TokenType::Update: return parse_update();
            case TokenType::Delete: return parse_delete();
            case TokenType::Select: return parse_select();
            default: fail("expected a statement"); return nullptr;
        }
    }

    StatementPtr parse_create_table() {
        advance();  // CREATE
        if (!expect(TokenType::Table, "TABLE")) return nullptr;
        auto statement = std::make_unique<CreateTableStatement>();
        statement->table = expect_identifier("table name");
        if (statement->table.empty()) return nullptr;
        if (!expect(TokenType::LParen, "'('")) return nullptr;
        if (!parse_column_defs(*statement)) return nullptr;
        if (!expect(TokenType::RParen, "')'")) return nullptr;
        return statement;
    }

    bool parse_column_defs(CreateTableStatement& statement) {
        if (!parse_column_def(statement)) return false;
        while (accept(TokenType::Comma)) {
            if (!parse_column_def(statement)) return false;
        }
        return true;
    }

    bool parse_column_def(CreateTableStatement& statement) {
        ColumnDef column;
        column.name = expect_identifier("column name");
        if (column.name.empty()) return false;
        if (!parse_type(column)) return false;
        statement.columns.push_back(std::move(column));
        return true;
    }

    // INTEGER and DOUBLE carry their catalog widths (4 and 8 bytes) so a
    // CREATE TABLE round-trips to the exact catalog JSON ex13 will write.
    bool parse_type(ColumnDef& column) {
        switch (peek().type) {
            case TokenType::Integer:
            case TokenType::Int:
                column.type = DataType::Integer;
                column.size = 4;
                advance();
                return true;
            case TokenType::Double:
                column.type = DataType::Double;
                column.size = 8;
                advance();
                return true;
            case TokenType::Varchar: {
                advance();
                if (!expect(TokenType::LParen, "'(' after VARCHAR")) return false;
                if (peek().type != TokenType::IntLiteral) return fail("expected VARCHAR size");
                std::int64_t size = 0;
                if (!int64_from(advance().text, size) || size <= 0) {
                    return fail("VARCHAR size must be a positive integer");
                }
                column.type = DataType::String;
                column.size = static_cast<std::size_t>(size);
                return expect(TokenType::RParen, "')' after VARCHAR size");
            }
            default:
                return fail("expected a type");
        }
    }

    StatementPtr parse_drop_table() {
        advance();  // DROP
        if (!expect(TokenType::Table, "TABLE")) return nullptr;
        auto statement = std::make_unique<DropTableStatement>();
        statement->table = expect_identifier("table name");
        if (statement->table.empty()) return nullptr;
        return statement;
    }

    StatementPtr parse_insert() {
        advance();  // INSERT
        if (!expect(TokenType::Into, "INTO")) return nullptr;
        auto statement = std::make_unique<InsertStatement>();
        statement->table = expect_identifier("table name");
        if (statement->table.empty()) return nullptr;
        if (!expect(TokenType::Values, "VALUES")) return nullptr;
        if (!parse_value_row(*statement)) return nullptr;
        while (accept(TokenType::Comma)) {
            if (!parse_value_row(*statement)) return nullptr;
        }
        return statement;
    }

    bool parse_value_row(InsertStatement& statement) {
        if (!expect(TokenType::LParen, "'('")) return false;
        std::vector<ExprPtr> row;
        if (!parse_literal(row)) return false;
        while (accept(TokenType::Comma)) {
            if (!parse_literal(row)) return false;
        }
        if (!expect(TokenType::RParen, "')'")) return false;
        statement.rows.push_back(std::move(row));
        return true;
    }

    bool parse_literal(std::vector<ExprPtr>& row) {
        LiteralValue value;
        switch (peek().type) {
            case TokenType::IntLiteral: {
                std::int64_t i = 0;
                if (!int64_from(peek().text, i)) return fail("integer literal out of range");
                value.tag = LiteralValue::Tag::Int;
                value.i = i;
                break;
            }
            case TokenType::DoubleLiteral: {
                double d = 0.0;
                if (!double_from(peek().text, d)) return fail("double literal out of range");
                value.tag = LiteralValue::Tag::Double;
                value.d = d;
                break;
            }
            case TokenType::StringLiteral:
                value.tag = LiteralValue::Tag::String;
                value.s = peek().text;
                break;
            default:
                return fail("expected a literal value");
        }
        advance();
        row.push_back(make_literal(std::move(value)));
        return true;
    }

    StatementPtr parse_update() {
        advance();  // UPDATE
        auto statement = std::make_unique<UpdateStatement>();
        statement->table = expect_identifier("table name");
        if (statement->table.empty()) return nullptr;
        if (!expect(TokenType::Set, "SET")) return nullptr;
        if (!parse_set_clause(*statement)) return nullptr;
        while (accept(TokenType::Comma)) {
            if (!parse_set_clause(*statement)) return nullptr;
        }
        if (accept(TokenType::Where)) {
            statement->where = parse_expr();
            if (!statement->where) return nullptr;
        }
        return statement;
    }

    bool parse_set_clause(UpdateStatement& statement) {
        SetClause set;
        set.column = expect_identifier("column name");
        if (set.column.empty()) return false;
        if (!expect(TokenType::Eq, "'='")) return false;
        set.value = parse_expr();
        if (!set.value) return false;
        statement.sets.push_back(std::move(set));
        return true;
    }

    StatementPtr parse_delete() {
        advance();  // DELETE
        if (!expect(TokenType::From, "FROM")) return nullptr;
        auto statement = std::make_unique<DeleteStatement>();
        statement->table = expect_identifier("table name");
        if (statement->table.empty()) return nullptr;
        if (accept(TokenType::Where)) {
            statement->where = parse_expr();
            if (!statement->where) return nullptr;
        }
        return statement;
    }

    StatementPtr parse_select() {
        const std::size_t line = peek().line;
        advance();  // SELECT
        auto statement = std::make_unique<SelectStatement>();
        if (!parse_select_items(*statement)) return nullptr;
        if (!expect(TokenType::From, "FROM")) return nullptr;
        if (!parse_table_ref(statement->from)) return nullptr;
        while (accept(TokenType::Join)) {
            JoinClause join;
            if (!parse_table_ref(join.table)) return nullptr;
            if (!expect(TokenType::On, "ON")) return nullptr;
            join.on = parse_expr();
            if (!join.on) return nullptr;
            statement->joins.push_back(std::move(join));
        }
        if (accept(TokenType::Where)) {
            statement->where = parse_expr();
            if (!statement->where) return nullptr;
        }
        if (accept(TokenType::Group)) {
            if (!expect(TokenType::By, "BY")) return nullptr;
            if (!parse_column_ref_list(statement->group_by)) return nullptr;
        }
        if (accept(TokenType::Order)) {
            if (!expect(TokenType::By, "BY")) return nullptr;
            if (!parse_order_by(*statement)) return nullptr;
        }
        if (accept(TokenType::Limit)) {
            if (peek().type != TokenType::IntLiteral) {
                fail("expected an integer after LIMIT");
                return nullptr;
            }
            std::int64_t limit = 0;
            if (!int64_from(advance().text, limit) || limit < 0) {
                fail("invalid LIMIT");
                return nullptr;
            }
            statement->limit = static_cast<std::uint64_t>(limit);
            statement->has_limit = true;
        }
        if (!check_grouping(*statement, line)) return nullptr;
        return statement;
    }

    bool parse_select_items(SelectStatement& statement) {
        if (accept(TokenType::Star)) {
            SelectItem star;
            star.star = true;
            statement.items.push_back(std::move(star));
            return true;
        }
        if (!parse_select_item(statement)) return false;
        while (accept(TokenType::Comma)) {
            if (!parse_select_item(statement)) return false;
        }
        return true;
    }

    bool parse_select_item(SelectStatement& statement) {
        SelectItem item;
        item.expr = parse_expr();
        if (!item.expr) return false;
        if (accept(TokenType::As)) {
            item.alias = expect_identifier("alias after AS");
            if (item.alias.empty()) return false;
        } else if (peek().type == TokenType::Identifier) {
            item.alias = advance().text;
        }
        statement.items.push_back(std::move(item));
        return true;
    }

    bool parse_table_ref(TableRef& ref) {
        ref.name = expect_identifier("table name");
        if (ref.name.empty()) return false;
        if (accept(TokenType::As)) {
            ref.alias = expect_identifier("alias after AS");
            if (ref.alias.empty()) return false;
        } else if (peek().type == TokenType::Identifier) {
            ref.alias = advance().text;
        }
        return true;
    }

    bool parse_column_ref_list(std::vector<ExprPtr>& out) {
        auto ref = parse_column_ref();
        if (!ref) return false;
        out.push_back(std::move(ref));
        while (accept(TokenType::Comma)) {
            ref = parse_column_ref();
            if (!ref) return false;
            out.push_back(std::move(ref));
        }
        return true;
    }

    bool parse_order_by(SelectStatement& statement) {
        if (!parse_order_item(statement)) return false;
        while (accept(TokenType::Comma)) {
            if (!parse_order_item(statement)) return false;
        }
        return true;
    }

    bool parse_order_item(SelectStatement& statement) {
        OrderItem item;
        item.expr = parse_column_ref();
        if (!item.expr) return false;
        if (accept(TokenType::Desc)) {
            item.descending = true;
        } else {
            accept(TokenType::Asc);
        }
        statement.order_by.push_back(std::move(item));
        return true;
    }

    // --- expressions ---

    ExprPtr parse_expr() { return parse_or(); }

    ExprPtr parse_or() {
        auto left = parse_and();
        if (!left) return nullptr;
        while (accept(TokenType::Or)) {
            auto right = parse_and();
            if (!right) return nullptr;
            left = make_binary(BinaryOp::Or, std::move(left), std::move(right));
        }
        return left;
    }

    ExprPtr parse_and() {
        auto left = parse_not();
        if (!left) return nullptr;
        while (accept(TokenType::And)) {
            auto right = parse_not();
            if (!right) return nullptr;
            left = make_binary(BinaryOp::And, std::move(left), std::move(right));
        }
        return left;
    }

    ExprPtr parse_not() {
        if (accept(TokenType::Not)) {
            auto operand = parse_not();
            if (!operand) return nullptr;
            return make_unary(UnaryOp::Not, std::move(operand));
        }
        return parse_comparison();
    }

    ExprPtr parse_comparison() {
        auto left = parse_additive();
        if (!left) return nullptr;
        if (!is_comparison(peek().type)) return left;
        const BinaryOp op = comparison_op(advance().type);
        auto right = parse_additive();
        if (!right) return nullptr;
        auto node = make_binary(op, std::move(left), std::move(right));
        // Exactly one comparison per level: 'a < b < c' stops here with a
        // named error instead of reassociating silently.
        if (is_comparison(peek().type)) {
            fail("comparison operators cannot be chained");
            return nullptr;
        }
        return node;
    }

    ExprPtr parse_additive() {
        auto left = parse_multiplicative();
        if (!left) return nullptr;
        for (;;) {
            BinaryOp op;
            switch (peek().type) {
                case TokenType::Plus: op = BinaryOp::Plus; break;
                case TokenType::Minus: op = BinaryOp::Minus; break;
                default: return left;
            }
            advance();
            auto right = parse_multiplicative();
            if (!right) return nullptr;
            left = make_binary(op, std::move(left), std::move(right));
        }
    }

    ExprPtr parse_multiplicative() {
        auto left = parse_unary();
        if (!left) return nullptr;
        for (;;) {
            BinaryOp op;
            switch (peek().type) {
                case TokenType::Star: op = BinaryOp::Mul; break;
                case TokenType::Slash: op = BinaryOp::Div; break;
                case TokenType::Percent: op = BinaryOp::Mod; break;
                default: return left;
            }
            advance();
            auto right = parse_unary();
            if (!right) return nullptr;
            left = make_binary(op, std::move(left), std::move(right));
        }
    }

    ExprPtr parse_unary() {
        if (accept(TokenType::Minus)) {
            auto operand = parse_unary();
            if (!operand) return nullptr;
            return make_unary(UnaryOp::Neg, std::move(operand));
        }
        return parse_primary();
    }

    ExprPtr parse_primary() {
        switch (peek().type) {
            case TokenType::IntLiteral: {
                std::int64_t i = 0;
                if (!int64_from(peek().text, i)) {
                    fail("integer literal out of range");
                    return nullptr;
                }
                advance();
                LiteralValue value;
                value.tag = LiteralValue::Tag::Int;
                value.i = i;
                return make_literal(std::move(value));
            }
            case TokenType::DoubleLiteral: {
                double d = 0.0;
                if (!double_from(peek().text, d)) {
                    fail("double literal out of range");
                    return nullptr;
                }
                advance();
                LiteralValue value;
                value.tag = LiteralValue::Tag::Double;
                value.d = d;
                return make_literal(std::move(value));
            }
            case TokenType::StringLiteral: {
                LiteralValue value;
                value.tag = LiteralValue::Tag::String;
                value.s = peek().text;
                advance();
                return make_literal(std::move(value));
            }
            case TokenType::LParen: {
                advance();
                auto expr = parse_expr();
                if (!expr) return nullptr;
                if (!expect(TokenType::RParen, "')'")) return nullptr;
                return expr;
            }
            case TokenType::Identifier: {
                if (peek(1).type == TokenType::LParen) {
                    const std::string name = advance().text;
                    advance();  // '('
                    return parse_function_call(name);
                }
                return parse_column_ref();
            }
            default:
                fail("expected an expression");
                return nullptr;
        }
    }

    ExprPtr parse_function_call(const std::string& name) {
        FunctionCode code;
        if (!function_code(name, code)) {
            fail("unknown function '" + name + "'");
            return nullptr;
        }
        ExprPtr arg;
        if (accept(TokenType::Star)) {
            if (code != FunctionCode::Count) {
                fail("'*' is only allowed in COUNT(*)");
                return nullptr;
            }
        } else {
            arg = parse_expr();
            if (!arg) return nullptr;
        }
        if (!expect(TokenType::RParen, "')'")) return nullptr;
        return make_function(code, std::move(arg));
    }

    ExprPtr parse_column_ref() {
        if (peek().type != TokenType::Identifier) {
            fail("expected a column");
            return nullptr;
        }
        std::string table;
        std::string column = advance().text;
        if (accept(TokenType::Dot)) {
            table = column;
            column = expect_identifier("column name after '.'");
            if (column.empty()) return nullptr;
        }
        return make_column_ref(std::move(table), std::move(column));
    }

    static bool function_code(const std::string& name, FunctionCode& out) {
        const std::string word = lowered(name);
        if (word == "count") { out = FunctionCode::Count; return true; }
        if (word == "sum") { out = FunctionCode::Sum; return true; }
        if (word == "avg") { out = FunctionCode::Avg; return true; }
        if (word == "min") { out = FunctionCode::Min; return true; }
        if (word == "max") { out = FunctionCode::Max; return true; }
        return false;
    }

    static bool is_comparison(TokenType type) {
        switch (type) {
            case TokenType::Eq:
            case TokenType::Ne:
            case TokenType::Lt:
            case TokenType::Le:
            case TokenType::Gt:
            case TokenType::Ge:
                return true;
            default: return false;
        }
    }

    static BinaryOp comparison_op(TokenType type) {
        switch (type) {
            case TokenType::Eq: return BinaryOp::Eq;
            case TokenType::Ne: return BinaryOp::Ne;
            case TokenType::Lt: return BinaryOp::Lt;
            case TokenType::Le: return BinaryOp::Le;
            case TokenType::Gt: return BinaryOp::Gt;
            case TokenType::Ge: return BinaryOp::Ge;
            default: return BinaryOp::Eq;  // not reached; is_comparison gates callers
        }
    }

    // --- structural semantics (catalog-free) ---

    // With GROUP BY present, every select item is either verbatim in
    // GROUP BY or an aggregate; ORDER BY items are column refs by the
    // grammar, so they must appear verbatim. Column existence waits for
    // the ex13 binder — the front-end accepts SELECT nope FROM t by design.
    bool check_grouping(const SelectStatement& statement, std::size_t line) {
        if (statement.group_by.empty()) return true;
        for (const SelectItem& item : statement.items) {
            if (item.star) continue;
            if (!grouped_ok(*item.expr, statement.group_by)) {
                return fail_at(line, "select item '" + print_expr(*item.expr) +
                                         "' must appear in GROUP BY or be an aggregate");
            }
        }
        for (const OrderItem& item : statement.order_by) {
            bool verbatim = false;
            for (const ExprPtr& grouped : statement.group_by) {
                if (*item.expr == *grouped) {
                    verbatim = true;
                    break;
                }
            }
            if (!verbatim) {
                return fail_at(line, "ORDER BY item '" + print_expr(*item.expr) +
                                         "' must appear in GROUP BY");
            }
        }
        return true;
    }

    static bool grouped_ok(const Expr& expr, const std::vector<ExprPtr>& group_by) {
        if (expr.kind == Expr::Kind::Function) return true;
        for (const ExprPtr& grouped : group_by) {
            if (expr == *grouped) return true;
        }
        return false;
    }

    const std::vector<Token>& tokens_;
    std::size_t pos_ = 0;
    std::string* error_ = nullptr;
    bool ok_ = true;
};

}

bool parse(const std::string& query, ParseResult& result, std::string& error) {
    result.statements.clear();
    error.clear();
    std::vector<Token> tokens;
    if (!tokenize(query, tokens, error)) {
        result.statements.clear();
        return false;
    }
    Parser parser(tokens);
    if (!parser.run(result, error)) {
        result.statements.clear();
        return false;
    }
    return true;
}

}
