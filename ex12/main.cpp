#include "sql/Ast.h"
#include "sql/AstDraw.h"
#include "sql/AstPrinter.h"
#include "sql/Lexer.h"
#include "sql/Parser.h"

#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (condition) {
        std::cout << "PASS: " << message << "\n";
    } else {
        std::cout << "FAIL: " << message << "\n";
        ++failures;
    }
}

// --- lexer helpers ---

bool tokens_are(const std::string& source,
                const std::vector<std::pair<sql::TokenType, std::string>>& expected) {
    std::vector<sql::Token> tokens;
    std::string error;
    if (!sql::tokenize(source, tokens, error)) {
        std::cout << "      tokenize failed: " << error << "\n";
        return false;
    }
    if (tokens.size() != expected.size() + 1) return false;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (tokens[i].type != expected[i].first || tokens[i].text != expected[i].second) {
            return false;
        }
    }
    return tokens.back().type == sql::TokenType::End;
}

bool lex_fails(const std::string& source, const std::string& line_prefix,
               const std::string& message_part) {
    std::vector<sql::Token> tokens;
    std::string error;
    if (sql::tokenize(source, tokens, error)) return false;
    return tokens.empty() && error.rfind(line_prefix, 0) == 0 &&
           error.find(message_part) != std::string::npos;
}

// --- parser helpers ---

using StatementPtr = std::unique_ptr<sql::Statement>;

// The parsed statement is kept alive in a suite-level holder: pointers
// into it (and the typed views derived from it) stay valid until the
// next ok_parse call. The suite consumes one statement at a time, so
// replacing the holder on each call keeps every pointer honest.
StatementPtr g_owner;

const sql::Statement* ok_parse(const std::string& query) {
    sql::ParseResult result;
    std::string error;
    if (!sql::parse(query, result, error) || result.statements.size() != 1) {
        std::cout << "      parse failed: " << error << "\n";
        g_owner.reset();
        return nullptr;
    }
    g_owner = std::move(result.statements[0]);
    return g_owner.get();
}

bool parses(const std::string& query) { return ok_parse(query) != nullptr; }

bool fails(const std::string& query, const std::string& line_prefix = "line ",
           const std::string& message_part = "") {
    sql::ParseResult result;
    std::string error;
    if (sql::parse(query, result, error)) return false;
    if (!result.statements.empty()) return false;
    if (error.rfind(line_prefix, 0) != 0) return false;
    if (!message_part.empty() && error.find(message_part) == std::string::npos) return false;
    return true;
}

bool round_trips(const std::string& query) {
    sql::ParseResult first;
    std::string error;
    if (!sql::parse(query, first, error) || first.statements.size() != 1) {
        std::cout << "      round-trip parse failed: " << error << "\n";
        return false;
    }
    const std::string text = sql::print(*first.statements[0]);
    sql::ParseResult second;
    if (!sql::parse(text, second, error) || second.statements.size() != 1) {
        std::cout << "      re-parse failed for '" << text << "': " << error << "\n";
        return false;
    }
    if (!(*first.statements[0] == *second.statements[0])) {
        std::cout << "      trees differ after re-parse of '" << text << "'\n";
        return false;
    }
    return true;
}

std::string draw_of(const std::string& query) {
    const sql::Statement* statement = ok_parse(query);
    return statement != nullptr ? sql::draw(*statement) : "";
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// --- typed statement views (kind tag decides, so static_cast is exact) ---

const sql::CreateTableStatement* as_create(const sql::Statement* s) {
    return s && s->kind == sql::Statement::Kind::CreateTable
               ? static_cast<const sql::CreateTableStatement*>(s)
               : nullptr;
}

const sql::DropTableStatement* as_drop(const sql::Statement* s) {
    return s && s->kind == sql::Statement::Kind::DropTable
               ? static_cast<const sql::DropTableStatement*>(s)
               : nullptr;
}

const sql::InsertStatement* as_insert(const sql::Statement* s) {
    return s && s->kind == sql::Statement::Kind::Insert
               ? static_cast<const sql::InsertStatement*>(s)
               : nullptr;
}

const sql::UpdateStatement* as_update(const sql::Statement* s) {
    return s && s->kind == sql::Statement::Kind::Update
               ? static_cast<const sql::UpdateStatement*>(s)
               : nullptr;
}

const sql::DeleteStatement* as_delete(const sql::Statement* s) {
    return s && s->kind == sql::Statement::Kind::Delete
               ? static_cast<const sql::DeleteStatement*>(s)
               : nullptr;
}

const sql::SelectStatement* as_select(const sql::Statement* s) {
    return s && s->kind == sql::Statement::Kind::Select
               ? static_cast<const sql::SelectStatement*>(s)
               : nullptr;
}

// Parses "SELECT <text> FROM t" and hands back the first item's expression;
// the tree lives in the ok_parse holder until the next call.
const sql::Expr* expr_of(const std::string& text) {
    const auto* select = as_select(ok_parse("SELECT " + text + " FROM t"));
    if (select == nullptr || select->items.empty() || select->items[0].star) return nullptr;
    return select->items[0].expr.get();
}

bool is_literal(const sql::Expr* e) { return e != nullptr && e->kind == sql::Expr::Kind::Literal; }

bool is_column(const sql::Expr* e, const std::string& table, const std::string& column) {
    return e != nullptr && e->kind == sql::Expr::Kind::ColumnRef && e->table == table &&
           e->column == column;
}

bool is_binary(const sql::Expr* e, sql::BinaryOp op) {
    return e != nullptr && e->kind == sql::Expr::Kind::Binary && e->bin_op == op;
}

bool is_unary(const sql::Expr* e, sql::UnaryOp op) {
    return e != nullptr && e->kind == sql::Expr::Kind::Unary && e->un_op == op;
}

bool is_function(const sql::Expr* e, sql::FunctionCode fn) {
    return e != nullptr && e->kind == sql::Expr::Kind::Function && e->fn == fn;
}

// Sub-expression payloads are unique_ptrs; forward to the raw-pointer forms.
bool is_literal(const sql::ExprPtr& e) { return is_literal(e.get()); }

bool is_column(const sql::ExprPtr& e, const std::string& table, const std::string& column) {
    return is_column(e.get(), table, column);
}

bool is_binary(const sql::ExprPtr& e, sql::BinaryOp op) { return is_binary(e.get(), op); }

bool is_unary(const sql::ExprPtr& e, sql::UnaryOp op) { return is_unary(e.get(), op); }

bool is_function(const sql::ExprPtr& e, sql::FunctionCode fn) { return is_function(e.get(), fn); }

}

int main() {
    std::cout << "== ex12: SQL front-end checks ==\n\n";

    // --- 1. Lexer: token streams -------------------------------------------------
    {
        using TT = sql::TokenType;
        check(tokens_are("", {}), "lexer: empty input is just End");
        check(tokens_are("   \t\r\n", {}), "lexer: whitespace is only End");

        check(tokens_are(", ( ) . * ;",
                         {{TT::Comma, ","}, {TT::LParen, "("}, {TT::RParen, ")"},
                          {TT::Dot, "."}, {TT::Star, "*"}, {TT::Semicolon, ";"}}),
              "lexer: punctuation tokens");
        check(tokens_are("= <> != < <= > >= + - / %",
                         {{TT::Eq, "="}, {TT::Ne, "<>"}, {TT::Ne, "!="}, {TT::Lt, "<"},
                          {TT::Le, "<="}, {TT::Gt, ">"}, {TT::Ge, ">="}, {TT::Plus, "+"},
                          {TT::Minus, "-"}, {TT::Slash, "/"}, {TT::Percent, "%"}}),
              "lexer: operator tokens, both <> and != are Ne");
        check(tokens_are("t.5", {{TT::Identifier, "t"}, {TT::Dot, "."}, {TT::IntLiteral, "5"}}),
              "lexer: t.5 is identifier, dot, integer");
        check(tokens_are("-1", {{TT::Minus, "-"}, {TT::IntLiteral, "1"}}),
              "lexer: -1 is Minus then integer (no negative literals)");

        check(tokens_are("select from where group by order limit join on as "
                         "insert into values update set delete create table drop "
                         "asc desc integer int double varchar and or not",
                         {{TT::Select, "select"}, {TT::From, "from"}, {TT::Where, "where"},
                          {TT::Group, "group"}, {TT::By, "by"}, {TT::Order, "order"},
                          {TT::Limit, "limit"}, {TT::Join, "join"}, {TT::On, "on"},
                          {TT::As, "as"}, {TT::Insert, "insert"}, {TT::Into, "into"},
                          {TT::Values, "values"}, {TT::Update, "update"}, {TT::Set, "set"},
                          {TT::Delete, "delete"}, {TT::Create, "create"}, {TT::Table, "table"},
                          {TT::Drop, "drop"}, {TT::Asc, "asc"}, {TT::Desc, "desc"},
                          {TT::Integer, "integer"}, {TT::Int, "int"}, {TT::Double, "double"},
                          {TT::Varchar, "varchar"}, {TT::And, "and"}, {TT::Or, "or"},
                          {TT::Not, "not"}}),
              "lexer: all 28 keywords, lowercase");
        check(tokens_are("SELECT FROM WHERE GROUP BY ORDER LIMIT JOIN ON AS "
                         "INSERT INTO VALUES UPDATE SET DELETE CREATE TABLE DROP "
                         "ASC DESC INTEGER INT DOUBLE VARCHAR AND OR NOT",
                         {{TT::Select, "SELECT"}, {TT::From, "FROM"}, {TT::Where, "WHERE"},
                          {TT::Group, "GROUP"}, {TT::By, "BY"}, {TT::Order, "ORDER"},
                          {TT::Limit, "LIMIT"}, {TT::Join, "JOIN"}, {TT::On, "ON"},
                          {TT::As, "AS"}, {TT::Insert, "INSERT"}, {TT::Into, "INTO"},
                          {TT::Values, "VALUES"}, {TT::Update, "UPDATE"}, {TT::Set, "SET"},
                          {TT::Delete, "DELETE"}, {TT::Create, "CREATE"}, {TT::Table, "TABLE"},
                          {TT::Drop, "DROP"}, {TT::Asc, "ASC"}, {TT::Desc, "DESC"},
                          {TT::Integer, "INTEGER"}, {TT::Int, "INT"}, {TT::Double, "DOUBLE"},
                          {TT::Varchar, "VARCHAR"}, {TT::And, "AND"}, {TT::Or, "OR"},
                          {TT::Not, "NOT"}}),
              "lexer: all 28 keywords, uppercase");
        check(tokens_are("SeLeCt fRoM WhErE",
                         {{TT::Select, "SeLeCt"}, {TT::From, "fRoM"}, {TT::Where, "WhErE"}}),
              "lexer: keyword match is case-insensitive, text keeps its case");

        check(tokens_are("myCol Mixed_Case _hidden c1",
                         {{TT::Identifier, "myCol"}, {TT::Identifier, "Mixed_Case"},
                          {TT::Identifier, "_hidden"}, {TT::Identifier, "c1"}}),
              "lexer: identifiers keep their case and accept underscores");
        check(tokens_are("SELECTED", {{TT::Identifier, "SELECTED"}}),
              "lexer: SELECTED is an identifier, not Select");
        check(tokens_are("updated", {{TT::Identifier, "updated"}}),
              "lexer: updated is an identifier, not Update");
        check(tokens_are("once", {{TT::Identifier, "once"}}),
              "lexer: once is an identifier, not On");
        check(tokens_are("count", {{TT::Identifier, "count"}}),
              "lexer: function names are not keywords");
        check(tokens_are("varchar(10)",
                         {{TT::Varchar, "varchar"}, {TT::LParen, "("},
                          {TT::IntLiteral, "10"}, {TT::RParen, ")"}}),
              "lexer: VARCHAR(10) token stream");

        check(tokens_are("0 007 42",
                         {{TT::IntLiteral, "0"}, {TT::IntLiteral, "007"}, {TT::IntLiteral, "42"}}),
              "lexer: integers keep their spelling");
        check(tokens_are("3.14 0.5 12.0",
                         {{TT::DoubleLiteral, "3.14"}, {TT::DoubleLiteral, "0.5"},
                          {TT::DoubleLiteral, "12.0"}}),
              "lexer: doubles with fractions");
        check(tokens_are("'PR' 'it''s' ''",
                         {{TT::StringLiteral, "PR"}, {TT::StringLiteral, "it's"},
                          {TT::StringLiteral, ""}}),
              "lexer: strings unescape '' to one quote");
        check(tokens_are("'a, b (c)'", {{TT::StringLiteral, "a, b (c)"}}),
              "lexer: punctuation inside strings is text");
        check(tokens_are("'a -- b'", {{TT::StringLiteral, "a -- b"}}),
              "lexer: comment marker inside strings is text");

        check(tokens_are("select -- comment\n1",
                         {{TT::Select, "select"}, {TT::IntLiteral, "1"}}),
              "lexer: -- comment skipped to end of line");
        check(tokens_are("1 -- trailing", {{TT::IntLiteral, "1"}}),
              "lexer: comment at end of input");
        check(tokens_are("-- 'x'' --\n1", {{TT::IntLiteral, "1"}}),
              "lexer: quotes inside comments are not strings");

        {
            std::vector<sql::Token> tokens;
            std::string error;
            check(sql::tokenize("a\nb\nc", tokens, error) && tokens.size() == 4 &&
                      tokens[0].line == 1 && tokens[1].line == 2 && tokens[2].line == 3,
                  "lexer: line numbers count newlines");
        }
        {
            std::vector<sql::Token> tokens;
            std::string error;
            check(sql::tokenize("a\n", tokens, error) && tokens.size() == 2 &&
                      tokens[1].type == sql::TokenType::End && tokens[1].line == 2,
                  "lexer: End carries the last line number");
        }

        check(lex_fails("1.2.3", "line 1: ", "malformed number '1.2.3'"),
              "lexer: malformed number '1.2.3'");
        check(lex_fails("3.", "line 1: ", "malformed number '3.'"),
              "lexer: fraction needs digits after the dot");
        check(lex_fails("1..2", "line 1: ", "malformed number '1..2'"),
              "lexer: malformed number '1..2'");
        check(lex_fails("select 1\n1.2.3", "line 2: ", "malformed number"),
              "lexer: malformed number reports its line");
        check(lex_fails("'abc", "line 1: ", "unterminated string"),
              "lexer: unterminated string");
        check(lex_fails("select '\n", "line 1: ", "unterminated string"),
              "lexer: unterminated string reports its line");
        check(lex_fails("#", "line 1: ", "unexpected character '#'"),
              "lexer: unexpected character '#'");
        check(lex_fails("select\n@", "line 2: ", "unexpected character '@'"),
              "lexer: unexpected character reports its line");
        check(lex_fails("!", "line 1: ", "unexpected character '!'"),
              "lexer: lone ! is not a token");
        check(lex_fails("select $", "line 1: ", "unexpected character '$'"),
              "lexer: unexpected character '$'");
    }

    // --- 2. Expressions ----------------------------------------------------------
    {
        const sql::Expr* e = expr_of("42");
        check(is_literal(e) && e->value.tag == sql::LiteralValue::Tag::Int && e->value.i == 42,
              "expr: integer literal");

        e = expr_of("3.5");
        check(is_literal(e) && e->value.tag == sql::LiteralValue::Tag::Double && e->value.d == 3.5,
              "expr: double literal");

        e = expr_of("'hi'");
        check(is_literal(e) && e->value.tag == sql::LiteralValue::Tag::String &&
                  e->value.s == "hi",
              "expr: string literal");

        e = expr_of("age");
        check(is_column(e, "", "age"), "expr: unqualified column ref");

        e = expr_of("c.owner_id");
        check(is_column(e, "c", "owner_id"), "expr: qualified column ref");

        e = expr_of("1 + 2 * 3");
        check(is_binary(e, sql::BinaryOp::Plus), "expr: * binds tighter than +");
        check(e != nullptr && e->left != nullptr && e->left->kind == sql::Expr::Kind::Literal &&
                  is_binary(e->right, sql::BinaryOp::Mul),
              "expr: 1 + 2 * 3 is Plus(1, Mul(2, 3))");

        e = expr_of("10 - 2 / 3");
        check(is_binary(e, sql::BinaryOp::Minus) && is_binary(e->right, sql::BinaryOp::Div),
              "expr: / binds tighter than -");

        e = expr_of("10 % 3 * 2");
        check(is_binary(e, sql::BinaryOp::Mul) && is_binary(e->left, sql::BinaryOp::Mod),
              "expr: % binds tighter than *");

        e = expr_of("1 + 2 + 3");
        check(is_binary(e, sql::BinaryOp::Plus) && is_binary(e->left, sql::BinaryOp::Plus),
              "expr: + is left-associative");

        e = expr_of("10 - 4 - 3");
        check(is_binary(e, sql::BinaryOp::Minus) && is_binary(e->left, sql::BinaryOp::Minus),
              "expr: - is left-associative");

        e = expr_of("1 + 2 - 3");
        check(is_binary(e, sql::BinaryOp::Minus) && is_binary(e->left, sql::BinaryOp::Plus),
              "expr: 1 + 2 - 3 is Minus(Plus(1, 2), 3)");

        e = expr_of("2 * 3 % 4");
        check(is_binary(e, sql::BinaryOp::Mod) && is_binary(e->left, sql::BinaryOp::Mul),
              "expr: * and % are left-associative together");

        e = expr_of("(1 + 2) * 3");
        check(is_binary(e, sql::BinaryOp::Mul) && is_binary(e->left, sql::BinaryOp::Plus),
              "expr: parentheses group the addition");

        e = expr_of("((1))");
        check(is_literal(e) && e->value.i == 1, "expr: nested parentheses collapse");

        e = expr_of("-5");
        check(is_unary(e, sql::UnaryOp::Neg) && is_literal(e->operand.get()) &&
                  e->operand->value.i == 5,
              "expr: unary minus");

        e = expr_of("-(1 + 2)");
        check(is_unary(e, sql::UnaryOp::Neg) && is_binary(e->operand.get(), sql::BinaryOp::Plus),
              "expr: unary minus over a parenthesized sum");

        e = expr_of("- -5");
        check(is_unary(e, sql::UnaryOp::Neg) && is_unary(e->operand.get(), sql::UnaryOp::Neg),
              "expr: double unary minus");

        e = expr_of("-2 * 3");
        check(is_binary(e, sql::BinaryOp::Mul) && is_unary(e->left, sql::UnaryOp::Neg),
              "expr: unary binds tighter than *");

        e = expr_of("a + 1 < b * 2");
        check(is_binary(e, sql::BinaryOp::Lt) && is_binary(e->left, sql::BinaryOp::Plus) &&
                  is_binary(e->right, sql::BinaryOp::Mul),
              "expr: additive binds tighter than comparison");

        e = expr_of("a = 1");
        check(is_binary(e, sql::BinaryOp::Eq), "expr: = comparison");
        e = expr_of("a <> 2");
        check(is_binary(e, sql::BinaryOp::Ne), "expr: <> comparison");
        e = expr_of("a != 3");
        check(is_binary(e, sql::BinaryOp::Ne), "expr: != is the same Ne comparison");
        e = expr_of("a < 4");
        check(is_binary(e, sql::BinaryOp::Lt), "expr: < comparison");
        e = expr_of("a <= 5");
        check(is_binary(e, sql::BinaryOp::Le), "expr: <= comparison");
        e = expr_of("a > 6");
        check(is_binary(e, sql::BinaryOp::Gt), "expr: > comparison");
        e = expr_of("a >= 7");
        check(is_binary(e, sql::BinaryOp::Ge), "expr: >= comparison");

        e = expr_of("NOT age");
        check(is_unary(e, sql::UnaryOp::Not) && is_column(e->operand.get(), "", "age"),
              "expr: NOT");

        e = expr_of("NOT NOT age");
        check(is_unary(e, sql::UnaryOp::Not) && is_unary(e->operand.get(), sql::UnaryOp::Not),
              "expr: NOT NOT");

        e = expr_of("NOT a = 1");
        check(is_unary(e, sql::UnaryOp::Not) && is_binary(e->operand.get(), sql::BinaryOp::Eq),
              "expr: comparison binds tighter than NOT");

        e = expr_of("a = 1 OR b = 2 AND c = 3");
        check(is_binary(e, sql::BinaryOp::Or) && is_binary(e->right, sql::BinaryOp::And),
              "expr: AND binds tighter than OR");

        e = expr_of("a AND b AND c");
        check(is_binary(e, sql::BinaryOp::And) && is_binary(e->left, sql::BinaryOp::And),
              "expr: AND is left-associative");

        e = expr_of("NOT a AND b");
        check(is_binary(e, sql::BinaryOp::And) && is_unary(e->left, sql::UnaryOp::Not),
              "expr: NOT binds tighter than AND");

        e = expr_of("NOT (a AND b)");
        check(is_unary(e, sql::UnaryOp::Not) && is_binary(e->operand.get(), sql::BinaryOp::And),
              "expr: parentheses override NOT precedence");

        e = expr_of("1 < 2 AND 3 > 4 OR NOT 5 = 5");
        check(is_binary(e, sql::BinaryOp::Or) && is_binary(e->left, sql::BinaryOp::And) &&
                  is_unary(e->right, sql::UnaryOp::Not),
              "expr: full precedence stack Or(And(Lt, Gt), Not(Eq))");

        e = expr_of("SUM(age)");
        check(is_function(e, sql::FunctionCode::Sum) && is_column(e->arg.get(), "", "age"),
              "expr: SUM over a column");

        e = expr_of("SUM(p.age)");
        check(is_function(e, sql::FunctionCode::Sum) && is_column(e->arg.get(), "p", "age"),
              "expr: aggregate over a qualified column");

        e = expr_of("SUM(age + 1)");
        check(is_function(e, sql::FunctionCode::Sum) &&
                  is_binary(e->arg.get(), sql::BinaryOp::Plus),
              "expr: aggregate over an expression");

        e = expr_of("COUNT(*)");
        check(is_function(e, sql::FunctionCode::Count) && e->arg == nullptr,
              "expr: COUNT(*) carries a null argument");

        e = expr_of("COUNT(pid)");
        check(is_function(e, sql::FunctionCode::Count) && is_column(e->arg.get(), "", "pid"),
              "expr: COUNT over a column");

        e = expr_of("count(*)");
        check(is_function(e, sql::FunctionCode::Count), "expr: function name case-insensitive");

        e = expr_of("Avg(age)");
        check(is_function(e, sql::FunctionCode::Avg), "expr: AVG parses in any case");

        e = expr_of("MIN('a')");
        check(is_function(e, sql::FunctionCode::Min), "expr: MIN over a literal");
        e = expr_of("MAX(1)");
        check(is_function(e, sql::FunctionCode::Max), "expr: MAX over a literal");

        e = expr_of("COUNT");
        check(is_column(e, "", "COUNT"), "expr: function name without parens is a column ref");

        check(fails("SELECT a < b < c FROM t", "line ", "cannot be chained"),
              "expr: a < b < c is a parse error");
        check(fails("SELECT FOO(x) FROM t", "line ", "unknown function 'FOO'"),
              "expr: unknown function name");
        check(fails("SELECT SUM(*) FROM t", "line ", "'*' is only allowed in COUNT(*)"),
              "expr: * only allowed in COUNT(*)");
        check(fails("SELECT 1 + FROM t", "line ", "expected an expression"),
              "expr: missing operand");
        check(fails("SELECT (1 FROM t", "line ", "expected ')'"),
              "expr: unclosed parenthesis");
        check(fails("SELECT NOT FROM t", "line ", "expected an expression"),
              "expr: keywords cannot be operands");
        check(fails("SELECT a FROM t WHERE a > 99999999999999999999999", "line ",
                    "integer literal out of range"),
              "expr: oversized integer literal");
    }

    // --- 3. Statements: CREATE / DROP ---------------------------------------------
    {
        const auto create = as_create(ok_parse(
            "CREATE TABLE person (pid INTEGER, name VARCHAR(10), age INT, city VARCHAR(3))"));
        check(create != nullptr && create->table == "person", "create: table name");
        check(create != nullptr && create->columns.size() == 4, "create: four columns");
        if (create != nullptr && create->columns.size() == 4) {
            const auto& c = create->columns;
            check(c[0].name == "pid" && c[0].type == sql::DataType::Integer && c[0].size == 4,
                  "create: pid INTEGER carries size 4");
            check(c[1].name == "name" && c[1].type == sql::DataType::String && c[1].size == 10,
                  "create: name VARCHAR(10) carries size 10");
            check(c[2].name == "age" && c[2].type == sql::DataType::Integer && c[2].size == 4,
                  "create: INT is INTEGER with size 4");
            check(c[3].name == "city" && c[3].type == sql::DataType::String && c[3].size == 3,
                  "create: city VARCHAR(3) carries size 3");
        }

        const auto car = as_create(ok_parse(
            "CREATE TABLE car (car_id INTEGER, brand VARCHAR(15), model VARCHAR(10), "
            "year INT, owner_id INTEGER)"));
        check(car != nullptr && car->columns.size() == 5, "create: car table has five columns");
        check(car != nullptr && car->columns.size() == 5 && car->columns[1].size == 15,
              "create: car.brand VARCHAR(15)");

        const auto dbl = as_create(ok_parse("CREATE TABLE t (x DOUBLE)"));
        check(dbl != nullptr && dbl->columns.size() == 1 &&
                  dbl->columns[0].type == sql::DataType::Double && dbl->columns[0].size == 8,
              "create: DOUBLE carries size 8");

        check(parses("create table t (a integer)"), "create: lowercase keywords");

        check(fails("CREATE TABLE t (a INTEGER,)", "line ", "expected column name"),
              "create: trailing comma");
        check(fails("CREATE TABLE t (a)", "line ", "expected a type"), "create: missing type");
        check(fails("CREATE TABLE t (a TEXT)", "line ", "expected a type"),
              "create: unknown type name");
        check(fails("CREATE TABLE t (a INT", "line ", "expected ')'"), "create: missing ')'");
        check(fails("CREATE TABLE t a INT)", "line ", "expected '('"), "create: missing '('");
        check(fails("CREATE person (a INT)", "line ", "expected TABLE"),
              "create: missing TABLE");
        check(fails("CREATE TABLE (a INT)", "line ", "expected table name"),
              "create: missing table name");
        check(fails("CREATE TABLE t ()", "line ", "expected column name"),
              "create: empty column list");
        check(fails("CREATE TABLE t (a VARCHAR)", "line ", "expected '(' after VARCHAR"),
              "create: VARCHAR without size parens");
        check(fails("CREATE TABLE t (a VARCHAR(10)", "line ", "expected ')'"),
              "create: VARCHAR without closing paren");
        check(fails("CREATE TABLE t (a VARCHAR(0))", "line ", "must be a positive integer"),
              "create: VARCHAR(0) refused");
        check(fails("CREATE TABLE t (a VARCHAR(-1))", "line ", "expected VARCHAR size"),
              "create: VARCHAR size must be an integer");
        check(fails("CREATE TABLE t (a INT) extra", "line ", "expected ';'"),
              "create: trailing garbage after table");

        const auto drop = as_drop(ok_parse("DROP TABLE person"));
        check(drop != nullptr && drop->table == "person", "drop: table name");
        check(parses("drop table u"), "drop: lowercase keywords");
        check(fails("DROP person", "line ", "expected TABLE"), "drop: missing TABLE");
        check(fails("DROP TABLE", "line ", "expected table name"), "drop: missing table name");
        check(fails("DROP TABLE u u2", "line ", "expected ';'"), "drop: trailing garbage");
    }

    // --- 4. Statements: INSERT ----------------------------------------------------
    {
        const auto ins = as_insert(ok_parse("INSERT INTO person VALUES (1, 2)"));
        check(ins != nullptr && ins->table == "person", "insert: table name");
        check(ins != nullptr && ins->rows.size() == 1 && ins->rows[0].size() == 2,
              "insert: one row, two values");
        check(ins != nullptr && !ins->rows.empty() && !ins->rows[0].empty() &&
                  is_literal(ins->rows[0][0].get()) &&
                  ins->rows[0][0]->value.tag == sql::LiteralValue::Tag::Int,
              "insert: integer literal value");

        const auto mixed = as_insert(ok_parse("INSERT INTO t VALUES (1, 3.5, 'PR')"));
        check(mixed != nullptr && mixed->rows.size() == 1 && mixed->rows[0].size() == 3 &&
                  mixed->rows[0][0]->value.tag == sql::LiteralValue::Tag::Int &&
                  mixed->rows[0][1]->value.tag == sql::LiteralValue::Tag::Double &&
                  mixed->rows[0][2]->value.tag == sql::LiteralValue::Tag::String &&
                  mixed->rows[0][2]->value.s == "PR",
              "insert: mixed literal types in one row");

        const auto multi = as_insert(ok_parse("INSERT INTO t VALUES (1), (2), (3)"));
        check(multi != nullptr && multi->rows.size() == 3, "insert: three rows");

        const auto car =
            as_insert(ok_parse("INSERT INTO car VALUES (1, 'Fiat', 'Uno', 2020, 7)"));
        check(car != nullptr && car->rows.size() == 1 && car->rows[0].size() == 5 &&
                  car->rows[0][1]->value.s == "Fiat" && car->rows[0][3]->value.i == 2020,
              "insert: car row values");

        const auto quote = as_insert(ok_parse("INSERT INTO t VALUES ('it''s')"));
        check(quote != nullptr && quote->rows.size() == 1 && quote->rows[0][0]->value.s == "it's",
              "insert: escaped quote round value");

        check(parses("insert into t values (1)"), "insert: lowercase keywords");

        check(fails("INSERT INTO t VALUES (-1)", "line ", "expected a literal value"),
              "insert: arithmetic in VALUES refused");
        check(fails("INSERT INTO t VALUES (age)", "line ", "expected a literal value"),
              "insert: column ref in VALUES refused");
        check(fails("INSERT INTO t VALUES (1 + 2)", "line ", "expected ')'"),
              "insert: expression in VALUES refused");
        check(fails("INSERT t VALUES (1)", "line ", "expected INTO"), "insert: missing INTO");
        check(fails("INSERT INTO t (1)", "line ", "expected VALUES"), "insert: missing VALUES");
        check(fails("INSERT INTO t VALUES 1", "line ", "expected '('"),
              "insert: missing row paren");
        check(fails("INSERT INTO t VALUES (1", "line ", "expected ')'"),
              "insert: missing closing paren");
        check(fails("INSERT INTO t VALUES ()", "line ", "expected a literal value"),
              "insert: empty row");
        check(fails("INSERT INTO t VALUES (1,)", "line ", "expected a literal value"),
              "insert: trailing comma in row");
        check(fails("INSERT INTO t VALUES (1), 2", "line ", "expected '('"),
              "insert: second row needs parens");
        check(fails("INSERT INTO t VALUES (1),()", "line ", "expected a literal value"),
              "insert: empty second row");
        check(fails("INSERT INTO t VALUES (99999999999999999999999)", "line ",
                    "integer literal out of range"),
              "insert: oversized integer literal");
    }

    // --- 5. Statements: UPDATE / DELETE -------------------------------------------
    {
        const auto upd = as_update(ok_parse("UPDATE person SET age = 18 WHERE pid = 1"));
        check(upd != nullptr && upd->table == "person", "update: table name");
        check(upd != nullptr && upd->sets.size() == 1 && upd->sets[0].column == "age" &&
                  is_literal(upd->sets[0].value.get()) && upd->sets[0].value->value.i == 18,
              "update: set clause");
        check(upd != nullptr && is_binary(upd->where.get(), sql::BinaryOp::Eq),
              "update: where clause");

        const auto multi = as_update(ok_parse("UPDATE t SET a = 1, b = 'x'"));
        check(multi != nullptr && multi->sets.size() == 2 && multi->sets[1].column == "b" &&
                  multi->sets[1].value->value.tag == sql::LiteralValue::Tag::String,
              "update: multiple set clauses");

        const auto expr = as_update(ok_parse("UPDATE t SET age = age + 1"));
        check(expr != nullptr && expr->sets.size() == 1 &&
                  is_binary(expr->sets[0].value.get(), sql::BinaryOp::Plus) &&
                  is_column(expr->sets[0].value->left.get(), "", "age"),
              "update: expression on the right of =");

        const auto bare = as_update(ok_parse("UPDATE t SET a = 1"));
        check(bare != nullptr && bare->where == nullptr, "update: where is optional");

        check(parses("update t set a = 1"), "update: lowercase keywords");

        check(fails("UPDATE t a = 1", "line ", "expected SET"), "update: missing SET");
        check(fails("UPDATE t SET a 1", "line ", "expected '='"), "update: missing '='");
        check(fails("UPDATE t SET a =", "line ", "expected an expression"),
              "update: missing set value");
        check(fails("UPDATE SET a = 1", "line ", "expected table name"),
              "update: missing table name");
        check(fails("UPDATE t SET a = 1 WHERE", "line ", "expected an expression"),
              "update: dangling WHERE");

        const auto del = as_delete(ok_parse("DELETE FROM person"));
        check(del != nullptr && del->table == "person" && del->where == nullptr,
              "delete: without where");

        const auto delw = as_delete(ok_parse("DELETE FROM person WHERE age > 18"));
        check(delw != nullptr && is_binary(delw->where.get(), sql::BinaryOp::Gt),
              "delete: with where");

        check(fails("DELETE person", "line ", "expected FROM"), "delete: missing FROM");
        check(fails("DELETE FROM WHERE age = 1", "line ", "expected table name"),
              "delete: missing table name");
        check(fails("DELETE FROM t WHERE", "line ", "expected an expression"),
              "delete: dangling WHERE");
        check(parses("delete from t"), "delete: lowercase keywords");
    }

    // --- 6. Statements: SELECT ----------------------------------------------------
    {
        const auto star = as_select(ok_parse("SELECT * FROM person"));
        check(star != nullptr && star->items.size() == 1 && star->items[0].star &&
                  star->items[0].expr == nullptr,
              "select: star item");
        check(star != nullptr && star->from.name == "person" && star->from.alias.empty(),
              "select: from table without alias");

        const auto items = as_select(ok_parse("SELECT pid, name FROM person"));
        check(items != nullptr && items->items.size() == 2 &&
                  is_column(items->items[0].expr.get(), "", "pid") &&
                  is_column(items->items[1].expr.get(), "", "name"),
              "select: plain item list");

        const auto calc = as_select(ok_parse("SELECT age + 1 FROM person"));
        check(calc != nullptr && calc->items.size() == 1 &&
                  is_binary(calc->items[0].expr.get(), sql::BinaryOp::Plus),
              "select: expression item");

        const auto aliased = as_select(ok_parse("SELECT pid AS id FROM person"));
        check(aliased != nullptr && aliased->items[0].alias == "id", "select: AS alias");

        const auto bare = as_select(ok_parse("SELECT pid id FROM person"));
        check(bare != nullptr && bare->items[0].alias == "id", "select: bare alias");

        const auto qualified = as_select(ok_parse("SELECT p.age FROM person AS p"));
        check(qualified != nullptr && is_column(qualified->items[0].expr.get(), "p", "age"),
              "select: qualified item over aliased table");

        const auto join = as_select(ok_parse("SELECT * FROM person JOIN car ON pid = owner_id"));
        check(join != nullptr && join->joins.size() == 1 && join->joins[0].table.name == "car",
              "select: join table");
        check(join != nullptr && join->joins.size() == 1 &&
                  is_binary(join->joins[0].on.get(), sql::BinaryOp::Eq) &&
                  is_column(join->joins[0].on->left.get(), "", "pid") &&
                  is_column(join->joins[0].on->right.get(), "", "owner_id"),
              "select: join ON condition");

        const auto jalias = as_select(
            ok_parse("SELECT * FROM person AS p JOIN car AS c ON c.owner_id = p.pid"));
        check(jalias != nullptr && jalias->joins.size() == 1 &&
                  jalias->joins[0].table.alias == "c" &&
                  is_column(jalias->joins[0].on->left.get(), "c", "owner_id") &&
                  is_column(jalias->joins[0].on->right.get(), "p", "pid"),
              "select: join with aliases on both sides");

        const auto chain =
            as_select(ok_parse("SELECT * FROM a JOIN b ON a.x = b.y JOIN c ON b.z = c.w"));
        check(chain != nullptr && chain->joins.size() == 2 && chain->joins[1].table.name == "c",
              "select: two joins chain");

        const auto self = as_select(
            ok_parse("SELECT * FROM person AS p1 JOIN person AS p2 ON p1.pid = p2.pid"));
        check(self != nullptr && self->from.alias == "p1" &&
                  self->joins[0].table.name == "person",
              "select: self-join via aliases");

        const auto where = as_select(ok_parse("SELECT * FROM t WHERE a = 1 OR b = 2"));
        check(where != nullptr && is_binary(where->where.get(), sql::BinaryOp::Or),
              "select: where clause");

        const auto group = as_select(ok_parse("SELECT city FROM person GROUP BY city, age"));
        check(group != nullptr && group->group_by.size() == 2 &&
                  is_column(group->group_by[0].get(), "", "city") &&
                  is_column(group->group_by[1].get(), "", "age"),
              "select: group by list");

        const auto order = as_select(ok_parse("SELECT * FROM t ORDER BY age DESC, name"));
        check(order != nullptr && order->order_by.size() == 2 && order->order_by[0].descending &&
                  !order->order_by[1].descending,
              "select: order by directions");
        check(order != nullptr && order->order_by.size() == 2 &&
                  is_column(order->order_by[0].expr.get(), "", "age"),
              "select: order by items are columns");

        const auto asc = as_select(ok_parse("SELECT * FROM t ORDER BY age ASC"));
        check(asc != nullptr && asc->order_by.size() == 1 && !asc->order_by[0].descending,
              "select: explicit ASC");

        const auto limit = as_select(ok_parse("SELECT * FROM t LIMIT 5"));
        check(limit != nullptr && limit->has_limit && limit->limit == 5, "select: limit");

        const auto limit0 = as_select(ok_parse("SELECT * FROM t LIMIT 0"));
        check(limit0 != nullptr && limit0->has_limit && limit0->limit == 0, "select: limit 0");

        check(parses("SELECT city, COUNT(*), AVG(age) FROM person JOIN car ON pid = owner_id "
                     "WHERE age >= 18 GROUP BY city ORDER BY city LIMIT 5"),
              "select: the sql.md worked example parses");

        check(fails("SELECT a", "line ", "expected FROM"), "select: missing FROM");
        check(fails("SELECT *, a FROM t", "line ", "expected FROM"),
              "select: star must stand alone");
        check(fails("SELECT a FROM", "line ", "expected table name"),
              "select: missing table name");
        check(fails("SELECT * FROM a JOIN b", "line ", "expected ON"), "select: join without ON");
        check(fails("SELECT * FROM a JOIN ON x = y", "line ", "expected table name"),
              "select: join without table");
        check(fails("SELECT city FROM t GROUP city", "line ", "expected BY"),
              "select: GROUP without BY");
        check(fails("SELECT city FROM t ORDER city", "line ", "expected BY"),
              "select: ORDER without BY");
        check(fails("SELECT city FROM t ORDER BY age + 1", "line ", "expected ';'"),
              "select: order by only takes columns");
        check(fails("SELECT city FROM t LIMIT '5'", "line ", "expected an integer after LIMIT"),
              "select: limit must be an integer");
        check(fails("SELECT city FROM t LIMIT -1", "line ", "expected an integer after LIMIT"),
              "select: limit must be unsigned");
        check(fails("SELECT a AS FROM t", "line ", "expected alias after AS"),
              "select: dangling AS");
        check(fails("SELECT * FROM t WHERE", "line ", "expected an expression"),
              "select: dangling WHERE");
        check(fails("SELECT a t2 t3 FROM t", "line ", "expected FROM"),
              "select: one alias per item");
    }

    // --- 7. Semantics: aggregates, grouping, structural errors --------------------
    {
        check(parses("SELECT city FROM t GROUP BY city"),
              "semantics: select item verbatim in GROUP BY");
        check(parses("SELECT city, COUNT(*) FROM t GROUP BY city"),
              "semantics: aggregate item beside grouped column");
        check(parses("SELECT COUNT(*) FROM t"), "semantics: bare aggregate without GROUP BY");
        check(parses("SELECT COUNT(*), city FROM t"),
              "semantics: mixed items without GROUP BY pass the front-end");
        check(parses("SELECT SUM(age + 1) FROM t GROUP BY city"),
              "semantics: aggregate over an expression");
        check(parses("SELECT p.age FROM person AS p GROUP BY p.age"),
              "semantics: qualified verbatim match");
        check(parses("SELECT city FROM t GROUP BY city ORDER BY city"),
              "semantics: order by a grouped column");
        check(fails("SELECT city FROM t GROUP BY city ORDER BY COUNT(*)", "line ",
                    "must appear in GROUP BY"),
              "semantics: COUNT(*) is not orderable (order items are columns)");
        check(parses("SELECT * FROM t GROUP BY city"),
              "semantics: star skips the grouping rule");
        check(parses("SELECT city FROM t GROUP BY city LIMIT 5"),
              "semantics: limit after group by");

        check(fails("SELECT city FROM t GROUP BY age", "line ",
                    "must appear in GROUP BY or be an aggregate"),
              "semantics: ungrouped select item refused");
        check(fails("SELECT age, COUNT(*) FROM t GROUP BY city", "line ",
                    "must appear in GROUP BY"),
              "semantics: first offending item wins");
        check(fails("SELECT age + 1 FROM t GROUP BY age", "line ", "must appear in GROUP BY"),
              "semantics: near-miss expression is not verbatim");
        check(fails("SELECT p.age FROM person AS p GROUP BY age", "line ",
                    "must appear in GROUP BY"),
              "semantics: qualification must match verbatim");
        check(fails("SELECT city FROM t GROUP BY city ORDER BY age", "line ",
                    "must appear in GROUP BY"),
              "semantics: ungrouped order by refused");
        check(fails("SELECT 1 FROM t; SELECT city FROM t GROUP BY age", "line ",
                    "must appear in GROUP BY"),
              "semantics: first-error-wins across statements");

        check(fails("SELECT foo(x) FROM t", "line ", "unknown function 'foo'"),
              "semantics: unknown function is case-preserving in the error");
        check(fails("SELECT a FROM t WHERE a < b < c", "line ", "cannot be chained"),
              "semantics: chained comparison in WHERE");
    }

    // --- 8. Round-trips: parse, print, re-parse, equal -----------------------------
    {
        check(round_trips("SELECT * FROM person"), "round-trip: select star");
        check(round_trips("SELECT pid, name AS n FROM person WHERE age >= 18"),
              "round-trip: items, alias, where");
        check(round_trips(
                  "SELECT city, COUNT(*) FROM person GROUP BY city ORDER BY city DESC LIMIT 5"),
              "round-trip: group by, order by, limit");
        check(round_trips("SELECT city, COUNT(*), AVG(age) FROM person JOIN car ON pid = owner_id "
                          "WHERE age >= 18 GROUP BY city ORDER BY city LIMIT 5"),
              "round-trip: the worked example");
        check(round_trips("SELECT p.pid, c.brand FROM person AS p JOIN car AS c "
                          "ON p.pid = c.owner_id WHERE p.age > 18 AND c.year <> 2020"),
              "round-trip: aliased join");
        check(round_trips("SELECT * FROM a JOIN b ON a.x = b.y JOIN c ON b.z = c.w"),
              "round-trip: three-table chain");
        check(round_trips("CREATE TABLE person (pid INTEGER, name VARCHAR(10), age INT, "
                          "city VARCHAR(3))"),
              "round-trip: create table (INT normalizes to INTEGER)");
        check(round_trips("CREATE TABLE t (x DOUBLE, y VARCHAR(3))"),
              "round-trip: create table with double");
        check(round_trips("DROP TABLE person"), "round-trip: drop table");
        check(round_trips("INSERT INTO person VALUES (1, 'Ruth', 42, 'PR')"),
              "round-trip: insert row");
        check(round_trips("INSERT INTO person VALUES (1, 'a'), (2, 'b')"),
              "round-trip: multi-row insert");
        check(round_trips("UPDATE person SET age = 18, city = 'PR' WHERE pid = 1"),
              "round-trip: update with sets and where");
        check(round_trips("UPDATE t SET a = a + 1"), "round-trip: update with expression");
        check(round_trips("DELETE FROM person WHERE age > 18"), "round-trip: delete with where");
        check(round_trips("DELETE FROM person"), "round-trip: delete without where");

        check(round_trips("SELECT 1 + 2 * 3 FROM t"), "round-trip: precedence nesting");
        check(round_trips("SELECT (1 + 2) * 3 FROM t"), "round-trip: parentheses preserved");
        check(round_trips("SELECT 1 + 2 + 3 - 4 FROM t"), "round-trip: left-associative chain");
        check(round_trips("SELECT 10 % 3 FROM t"), "round-trip: modulo");
        check(round_trips("SELECT -(1 + 2) * 3 FROM t"), "round-trip: unary over parens");
        check(round_trips("SELECT NOT a = 1 AND b < 2 OR c <> 3 FROM t"),
              "round-trip: logic precedence");
        check(round_trips("SELECT NOT (a AND b) OR -c FROM t"), "round-trip: parens inside NOT");
        check(round_trips("SELECT a = -3 FROM t"), "round-trip: negative literal operand");
        check(round_trips("SELECT - -3 FROM t"), "round-trip: double negation keeps its space");
        check(round_trips("SELECT 3.14 FROM t"), "round-trip: double keeps its dot");
        check(round_trips("SELECT 'it''s' FROM t"), "round-trip: quote escaping");
        check(round_trips("SELECT COUNT(*), SUM(p.age + 1) AS s FROM t"),
              "round-trip: aggregates with alias");
        check(round_trips("SELECT * FROM t LIMIT 0"), "round-trip: limit zero");
        check(round_trips("select 1 from t"), "round-trip: lower-case input normalizes");
    }

    // --- 9. API contract -----------------------------------------------------------
    {
        sql::ParseResult result;
        std::string error;

        check(sql::parse("", result, error) && result.statements.empty(),
              "api: empty input parses to zero statements");
        check(sql::parse("   ", result, error) && result.statements.empty(),
              "api: whitespace input parses to zero statements");
        check(sql::parse(";", result, error) && result.statements.empty(),
              "api: lone semicolon parses to zero statements");
        check(sql::parse(";;;", result, error) && result.statements.empty(),
              "api: semicolons only parse to zero statements");
        check(sql::parse(";; SELECT 1 FROM t", result, error) && result.statements.size() == 1,
              "api: leading empty statements ignored");

        check(sql::parse("DROP TABLE a; DROP TABLE b", result, error) &&
                  result.statements.size() == 2 &&
                  result.statements[0]->kind == sql::Statement::Kind::DropTable &&
                  result.statements[1]->kind == sql::Statement::Kind::DropTable,
              "api: two statements");

        check(sql::parse("CREATE TABLE t (a INT); INSERT INTO t VALUES (1); SELECT * FROM t",
                         result, error) &&
                  result.statements.size() == 3 &&
                  result.statements[0]->kind == sql::Statement::Kind::CreateTable &&
                  result.statements[1]->kind == sql::Statement::Kind::Insert &&
                  result.statements[2]->kind == sql::Statement::Kind::Select,
              "api: three statements in order");

        check(sql::parse("SELECT 1 FROM t;", result, error) && result.statements.size() == 1,
              "api: trailing semicolon optional");

        check(sql::parse("SELECT 1 FROM t )", result, error) == false &&
                  result.statements.empty(),
              "api: failed parse leaves result empty");

        check(sql::parse("SELECT 1 FROM t", result, error) &&
                  sql::parse("SELECT FROM t", result, error) == false &&
                  result.statements.empty(),
              "api: result cleared on entry on every call");

        error = "stale";
        check(sql::parse("SELECT a FROM", result, error) == false &&
                  error.rfind("line 1: ", 0) == 0 &&
                  error.find("expected table name") != std::string::npos,
              "api: error carries line and message, overwritten on entry");

        check(sql::parse("SELECT 1 FROM; SELECT 1 FROM t", result, error) == false &&
                  error.find("expected table name") != std::string::npos,
              "api: first error wins");

        check(fails("DROP TABLE t extra; SELECT 1 FROM t", "line ", "expected ';'"),
              "api: first statement's garbage stops parsing");
    }

    // --- 10. AstDraw: the demo's tree drawings -------------------------------------
    {
        const std::string worked = draw_of(
            "SELECT city, COUNT(*), AVG(age) FROM person JOIN car ON pid = owner_id "
            "WHERE age >= 18 GROUP BY city ORDER BY city LIMIT 5");
        check(contains(worked, "SelectStatement"), "draw: select root");
        {
            std::size_t pos = 0;
            bool ordered = true;
            for (const char* clause : {"├─ items", "├─ from", "├─ joins", "├─ where",
                                       "├─ group_by", "├─ order_by", "└─ limit"}) {
                const std::size_t next = worked.find(clause, pos);
                if (next == std::string::npos) {
                    ordered = false;
                    break;
                }
                pos = next;
            }
            check(ordered, "draw: clauses in grammar order");
        }
        check(contains(worked, "Binary{ Eq }") && contains(worked, "Binary{ Ge }"),
              "draw: composite expressions drawn as nodes");
        check(contains(worked, "Function{ COUNT(*) }") && contains(worked, "Function{ AVG }"),
              "draw: function labels");
        check(contains(worked, "┌") && contains(worked, "┴") && contains(worked, "┐"),
              "draw: branch rows use the box glyphs");
        check(contains(worked, "│"), "draw: unary stubs");
        check(contains(worked, "└─ limit  5"), "draw: limit inline");
        {
            bool gutter_ok = true;
            const std::size_t start = worked.find("├─ items");
            const std::size_t end = worked.find("├─ from");
            if (start == std::string::npos || end == std::string::npos || end <= start) {
                gutter_ok = false;
            } else {
                std::size_t pos = worked.find('\n', start);
                while (gutter_ok && pos != std::string::npos) {
                    ++pos;  // start of the next line
                    if (pos >= end) break;  // reached the ├─ from line
                    if (worked.compare(pos, 3, "│") != 0) gutter_ok = false;
                    pos = worked.find('\n', pos);
                }
            }
            check(gutter_ok, "draw: nested blocks stay inside the gutter");
        }

        const std::string leafy = draw_of("SELECT age FROM t");
        check(!contains(leafy, "┌") && !contains(leafy, "┐") &&
                  contains(leafy, "ColumnRef{ age }"),
              "draw: leaf-only statement has no branch rows");
        check(!contains(draw_of("SELECT * FROM t"), "where"),
              "draw: absent clauses are absent");
        check(contains(draw_of("SELECT * FROM t"), "SelectItem[0]  *"), "draw: star item");

        const std::string create =
            draw_of("CREATE TABLE person (pid INTEGER, name VARCHAR(10))");
        check(contains(create, "table  person") && contains(create, "pid  INTEGER(4)") &&
                  contains(create, "name  VARCHAR(10)"),
              "draw: create table fields");
        const std::string insert = draw_of("INSERT INTO person VALUES (1, 'Ruth'), (2, 'Amy')");
        check(contains(insert, "row[0]") && contains(insert, "Literal{ 1 }") &&
                  contains(insert, "Literal{ 'Ruth' }"),
              "draw: insert rows as literal lists");
        const std::string update =
            draw_of("UPDATE person SET age = 18, city = 'PR' WHERE pid = 1");
        check(contains(update, "SetClause{ age }") && contains(update, "= Literal{ 18 }"),
              "draw: update set clauses");
        check(contains(draw_of("DELETE FROM person WHERE age > 18"), "Binary{ Gt }") &&
                  contains(draw_of("DELETE FROM person WHERE age > 18"), "table  person"),
              "draw: delete fields");
        check(contains(draw_of("DROP TABLE person"), "table  person"), "draw: drop table");

        const std::string logic =
            draw_of("SELECT name FROM person WHERE NOT city = 'PR' AND age >= 18");
        check(contains(logic, "Binary{ And }") && contains(logic, "Unary{ Not }") &&
                  contains(logic, "Binary{ Eq }"),
              "draw: precedence nesting in the where drawing");
    }

    std::cout << "\n"
              << (failures == 0 ? "ALL CHECKS PASSED"
                                : std::to_string(failures) + " CHECKS FAILED")
              << "\n";
    return failures == 0 ? 0 : 1;
}
