#include "Parser.h"

#include "AstPrinter.h"
#include "Lexer.h"

#include "common/Error.h"

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
// cursor below. A rule that cannot continue throws common::Error with
// the named message; the first failure unwinds the whole parse.
class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens) : tokens_(tokens) {}

    ParseResult run() {
        ParseResult result;
        while (peek().type != TokenType::End) {
            while (accept(TokenType::Semicolon)) {
            }
            if (peek().type == TokenType::End) break;
            result.statements.push_back(parse_statement());
            if (peek().type == TokenType::Semicolon) {
                advance();
            } else if (peek().type != TokenType::End) {
                fail("expected ';'");
            }
        }
        return result;
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

    void expect(TokenType type, const std::string& what) {
        if (!accept(type)) fail("expected " + what);
    }

    std::string expect_identifier(const std::string& what = "identifier") {
        if (peek().type != TokenType::Identifier) fail("expected " + what);
        return advance().text;
    }

    [[noreturn]] void fail(const std::string& message) { fail_at(peek().line, message); }

    [[noreturn]] void fail_at(std::size_t line, const std::string& message) {
        throw common::Error("line " + std::to_string(line) + ": " + message);
    }

    // --- statements ---

    StatementPtr parse_statement() {
        switch (peek().type) {
            case TokenType::Create:
                // CREATE TABLE vs CREATE INDEX: the second word decides.
                return peek(1).type == TokenType::Index ? parse_create_index()
                                                        : parse_create_table();
            case TokenType::Drop:
                return peek(1).type == TokenType::Index ? parse_drop_index()
                                                        : parse_drop_table();
            case TokenType::Insert: return parse_insert();
            case TokenType::Update: return parse_update();
            case TokenType::Delete: return parse_delete();
            case TokenType::Select: return parse_select();
            default: fail("expected a statement");
        }
    }

    StatementPtr parse_create_table() {
        advance();  // CREATE
        expect(TokenType::Table, "TABLE");
        auto statement = std::make_unique<CreateTableStatement>();
        statement->table = expect_identifier("table name");
        expect(TokenType::LParen, "'('");
        parse_column_defs(*statement);
        expect(TokenType::RParen, "')'");
        return statement;
    }

    void parse_column_defs(CreateTableStatement& statement) {
        parse_column_def(statement);
        while (accept(TokenType::Comma)) {
            parse_column_def(statement);
        }
    }

    void parse_column_def(CreateTableStatement& statement) {
        ColumnDef column;
        column.name = expect_identifier("column name");
        parse_type(column);
        statement.columns.push_back(std::move(column));
    }

    // INTEGER and DOUBLE carry their catalog widths (4 and 8 bytes) so a
    // CREATE TABLE round-trips to the exact catalog JSON ex14 will write.
    void parse_type(ColumnDef& column) {
        switch (peek().type) {
            case TokenType::Integer:
            case TokenType::Int:
                column.type = DataType::Integer;
                column.size = 4;
                advance();
                return;
            case TokenType::Double:
                column.type = DataType::Double;
                column.size = 8;
                advance();
                return;
            case TokenType::Varchar: {
                advance();
                expect(TokenType::LParen, "'(' after VARCHAR");
                if (peek().type != TokenType::IntLiteral) fail("expected VARCHAR size");
                std::int64_t size = 0;
                if (!int64_from(advance().text, size) || size <= 0) {
                    fail("VARCHAR size must be a positive integer");
                }
                column.type = DataType::String;
                column.size = static_cast<std::size_t>(size);
                expect(TokenType::RParen, "')' after VARCHAR size");
                return;
            }
            default:
                fail("expected a type");
        }
    }

    StatementPtr parse_drop_table() {
        advance();  // DROP
        expect(TokenType::Table, "TABLE");
        auto statement = std::make_unique<DropTableStatement>();
        statement->table = expect_identifier("table name");
        return statement;
    }

    StatementPtr parse_create_index() {
        advance();  // CREATE
        expect(TokenType::Index, "INDEX");  // dispatch already peeked it
        auto statement = std::make_unique<CreateIndexStatement>();
        statement->index_name = expect_identifier("index name");
        expect(TokenType::On, "ON");
        statement->table = expect_identifier("table name");
        expect(TokenType::LParen, "'('");
        statement->column = expect_identifier("column name");
        expect(TokenType::RParen, "')'");
        if (accept(TokenType::Using)) {
            if (accept(TokenType::Hash)) {
                statement->using_kind = IndexKind::Hash;
            } else if (accept(TokenType::Isam)) {
                statement->using_kind = IndexKind::Isam;
            } else if (accept(TokenType::Static)) {
                expect(TokenType::Hash, "HASH after STATIC");
                statement->using_kind = IndexKind::Static;
            } else {
                fail("expected HASH, ISAM, or STATIC HASH after USING");
            }
            statement->has_using = true;
        }
        return statement;
    }

    StatementPtr parse_drop_index() {
        advance();  // DROP
        expect(TokenType::Index, "INDEX");  // dispatch already peeked it
        auto statement = std::make_unique<DropIndexStatement>();
        statement->index_name = expect_identifier("index name");
        return statement;
    }

    StatementPtr parse_insert() {
        advance();  // INSERT
        expect(TokenType::Into, "INTO");
        auto statement = std::make_unique<InsertStatement>();
        statement->table = expect_identifier("table name");
        expect(TokenType::Values, "VALUES");
        parse_value_row(*statement);
        while (accept(TokenType::Comma)) {
            parse_value_row(*statement);
        }
        return statement;
    }

    void parse_value_row(InsertStatement& statement) {
        expect(TokenType::LParen, "'('");
        std::vector<ExprPtr> row;
        parse_literal(row);
        while (accept(TokenType::Comma)) {
            parse_literal(row);
        }
        expect(TokenType::RParen, "')'");
        statement.rows.push_back(std::move(row));
    }

    void parse_literal(std::vector<ExprPtr>& row) {
        LiteralValue value;
        switch (peek().type) {
            case TokenType::IntLiteral: {
                std::int64_t i = 0;
                if (!int64_from(peek().text, i)) fail("integer literal out of range");
                value.tag = LiteralValue::Tag::Int;
                value.i = i;
                break;
            }
            case TokenType::DoubleLiteral: {
                double d = 0.0;
                if (!double_from(peek().text, d)) fail("double literal out of range");
                value.tag = LiteralValue::Tag::Double;
                value.d = d;
                break;
            }
            case TokenType::StringLiteral:
                value.tag = LiteralValue::Tag::String;
                value.s = peek().text;
                break;
            default:
                fail("expected a literal value");
        }
        advance();
        row.push_back(make_literal(std::move(value)));
    }

    StatementPtr parse_update() {
        advance();  // UPDATE
        auto statement = std::make_unique<UpdateStatement>();
        statement->table = expect_identifier("table name");
        expect(TokenType::Set, "SET");
        parse_set_clause(*statement);
        while (accept(TokenType::Comma)) {
            parse_set_clause(*statement);
        }
        if (accept(TokenType::Where)) {
            statement->where = parse_expr();
        }
        return statement;
    }

    void parse_set_clause(UpdateStatement& statement) {
        SetClause set;
        set.column = expect_identifier("column name");
        expect(TokenType::Eq, "'='");
        set.value = parse_expr();
        statement.sets.push_back(std::move(set));
    }

    StatementPtr parse_delete() {
        advance();  // DELETE
        expect(TokenType::From, "FROM");
        auto statement = std::make_unique<DeleteStatement>();
        statement->table = expect_identifier("table name");
        if (accept(TokenType::Where)) {
            statement->where = parse_expr();
        }
        return statement;
    }

    StatementPtr parse_select() {
        const std::size_t line = peek().line;
        advance();  // SELECT
        auto statement = std::make_unique<SelectStatement>();
        parse_select_items(*statement);
        expect(TokenType::From, "FROM");
        parse_table_ref(statement->from);
        while (accept(TokenType::Join)) {
            JoinClause join;
            parse_table_ref(join.table);
            expect(TokenType::On, "ON");
            join.on = parse_expr();
            statement->joins.push_back(std::move(join));
        }
        if (accept(TokenType::Where)) {
            statement->where = parse_expr();
        }
        if (accept(TokenType::Group)) {
            expect(TokenType::By, "BY");
            parse_column_ref_list(statement->group_by);
        }
        if (accept(TokenType::Having)) {
            statement->having = parse_expr();
        }
        if (accept(TokenType::Order)) {
            expect(TokenType::By, "BY");
            parse_order_by(*statement);
        }
        if (accept(TokenType::Limit)) {
            if (peek().type != TokenType::IntLiteral) {
                fail("expected an integer after LIMIT");
            }
            std::int64_t limit = 0;
            if (!int64_from(advance().text, limit) || limit < 0) {
                fail("invalid LIMIT");
            }
            statement->limit = static_cast<std::uint64_t>(limit);
            statement->has_limit = true;
        }
        check_grouping(*statement, line);
        return statement;
    }

    void parse_select_items(SelectStatement& statement) {
        if (accept(TokenType::Star)) {
            SelectItem star;
            star.star = true;
            statement.items.push_back(std::move(star));
            return;
        }
        parse_select_item(statement);
        while (accept(TokenType::Comma)) {
            parse_select_item(statement);
        }
    }

    void parse_select_item(SelectStatement& statement) {
        SelectItem item;
        item.expr = parse_expr();
        if (accept(TokenType::As)) {
            item.alias = expect_identifier("alias after AS");
        } else if (peek().type == TokenType::Identifier) {
            item.alias = advance().text;
        }
        statement.items.push_back(std::move(item));
    }

    void parse_table_ref(TableRef& ref) {
        ref.name = expect_identifier("table name");
        if (accept(TokenType::As)) {
            ref.alias = expect_identifier("alias after AS");
        } else if (peek().type == TokenType::Identifier) {
            ref.alias = advance().text;
        }
    }

    void parse_column_ref_list(std::vector<ExprPtr>& out) {
        out.push_back(parse_column_ref());
        while (accept(TokenType::Comma)) {
            out.push_back(parse_column_ref());
        }
    }

    void parse_order_by(SelectStatement& statement) {
        parse_order_item(statement);
        while (accept(TokenType::Comma)) {
            parse_order_item(statement);
        }
    }

    void parse_order_item(SelectStatement& statement) {
        OrderItem item;
        item.expr = parse_column_ref();
        if (accept(TokenType::Desc)) {
            item.descending = true;
        } else {
            accept(TokenType::Asc);
        }
        statement.order_by.push_back(std::move(item));
    }

    // --- expressions ---

    ExprPtr parse_expr() { return parse_or(); }

    ExprPtr parse_or() {
        auto left = parse_and();
        while (accept(TokenType::Or)) {
            auto right = parse_and();
            left = make_binary(BinaryOp::Or, std::move(left), std::move(right));
        }
        return left;
    }

    ExprPtr parse_and() {
        auto left = parse_not();
        while (accept(TokenType::And)) {
            auto right = parse_not();
            left = make_binary(BinaryOp::And, std::move(left), std::move(right));
        }
        return left;
    }

    ExprPtr parse_not() {
        if (accept(TokenType::Not)) {
            auto operand = parse_not();
            return make_unary(UnaryOp::Not, std::move(operand));
        }
        return parse_comparison();
    }

    ExprPtr parse_comparison() {
        auto left = parse_additive();
        if (!is_comparison(peek().type)) return left;
        const BinaryOp op = comparison_op(advance().type);
        auto right = parse_additive();
        auto node = make_binary(op, std::move(left), std::move(right));
        // Exactly one comparison per level: 'a < b < c' stops here with a
        // named error instead of reassociating silently.
        if (is_comparison(peek().type)) {
            fail("comparison operators cannot be chained");
        }
        return node;
    }

    ExprPtr parse_additive() {
        auto left = parse_multiplicative();
        for (;;) {
            BinaryOp op;
            switch (peek().type) {
                case TokenType::Plus: op = BinaryOp::Plus; break;
                case TokenType::Minus: op = BinaryOp::Minus; break;
                default: return left;
            }
            advance();
            auto right = parse_multiplicative();
            left = make_binary(op, std::move(left), std::move(right));
        }
    }

    ExprPtr parse_multiplicative() {
        auto left = parse_unary();
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
            left = make_binary(op, std::move(left), std::move(right));
        }
    }

    ExprPtr parse_unary() {
        if (accept(TokenType::Minus)) {
            auto operand = parse_unary();
            return make_unary(UnaryOp::Neg, std::move(operand));
        }
        return parse_primary();
    }

    ExprPtr parse_primary() {
        switch (peek().type) {
            case TokenType::IntLiteral: {
                std::int64_t i = 0;
                if (!int64_from(peek().text, i)) fail("integer literal out of range");
                advance();
                LiteralValue value;
                value.tag = LiteralValue::Tag::Int;
                value.i = i;
                return make_literal(std::move(value));
            }
            case TokenType::DoubleLiteral: {
                double d = 0.0;
                if (!double_from(peek().text, d)) fail("double literal out of range");
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
                expect(TokenType::RParen, "')'");
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
        }
    }

    ExprPtr parse_function_call(const std::string& name) {
        FunctionCode code;
        if (!function_code(name, code)) {
            fail("unknown function '" + name + "'");
        }
        if (peek().type == TokenType::Identifier && lowered(peek().text) == "distinct") {
            fail("DISTINCT aggregates are not supported");
        }
        ExprPtr arg;
        if (accept(TokenType::Star)) {
            if (code != FunctionCode::Count) {
                fail("'*' is only allowed in COUNT(*)");
            }
        } else {
            arg = parse_expr();
        }
        expect(TokenType::RParen, "')'");
        return make_function(code, std::move(arg));
    }

    ExprPtr parse_column_ref() {
        if (peek().type != TokenType::Identifier) fail("expected a column");
        std::string table;
        std::string column = advance().text;
        if (accept(TokenType::Dot)) {
            table = column;
            column = expect_identifier("column name after '.'");
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
    // grammar, so they must appear verbatim. A HAVING expression may
    // reference aggregates and grouped columns anywhere in the tree: a
    // column leaf either sits under an aggregate subtree or is verbatim
    // in GROUP BY. Column existence waits for the ex17 binder — the
    // front-end accepts SELECT nope FROM t by design.
    void check_grouping(const SelectStatement& statement, std::size_t line) {
        if (!statement.group_by.empty()) {
            for (const SelectItem& item : statement.items) {
                if (item.star) continue;
                if (!grouped_ok(*item.expr, statement.group_by)) {
                    fail_at(line, "select item '" + print_expr(*item.expr) +
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
                    fail_at(line, "ORDER BY item '" + print_expr(*item.expr) +
                                             "' must appear in GROUP BY");
                }
            }
        }
        if (statement.having && !having_ok(*statement.having, statement.group_by)) {
            fail_at(line, "having item '" + print_expr(*statement.having) +
                                     "' must appear in GROUP BY or be an aggregate");
        }
    }

    static bool grouped_ok(const Expr& expr, const std::vector<ExprPtr>& group_by) {
        if (expr.kind == Expr::Kind::Function) return true;
        for (const ExprPtr& grouped : group_by) {
            if (expr == *grouped) return true;
        }
        return false;
    }

    // HAVING-ok: every column leaf either sits under an aggregate subtree
    // or is verbatim in GROUP BY; literals and arithmetic over the two are
    // fine. Unlike the select-item rule this walks the tree — a HAVING is
    // a predicate over group results, so comparisons over aggregates are
    // its normal shape.
    static bool having_ok(const Expr& expr, const std::vector<ExprPtr>& group_by) {
        switch (expr.kind) {
            case Expr::Kind::Function: return true;  // aggregate subtree
            case Expr::Kind::Literal: return true;
            case Expr::Kind::ColumnRef:
                for (const ExprPtr& grouped : group_by) {
                    if (expr == *grouped) return true;
                }
                return false;
            case Expr::Kind::Binary:
                return having_ok(*expr.left, group_by) &&
                       having_ok(*expr.right, group_by);
            case Expr::Kind::Unary:
                return having_ok(*expr.operand, group_by);
        }
        return false;
    }

    const std::vector<Token>& tokens_;
    std::size_t pos_ = 0;
};

}

ParseResult parse(const std::string& query) {
    const std::vector<Token> tokens = tokenize(query);
    Parser parser(tokens);
    return parser.run();
}

}
