#pragma once

#include <cstddef>
#include <cctype>
#include <string>
#include <string_view>

namespace sql {

// One enum for every token: the 28 keywords of the SQL subset, the four
// literal shapes, punctuation, operators, and End. A single enum keeps
// the parser's switch flat — no keyword sub-tag to consult.
enum class TokenType {
    // keywords (matched case-insensitively by the lexer)
    And, Or, Not,
    Select, From, Where, Group, By, Order, Limit,
    Join, On, As,
    Insert, Into, Values,
    Update, Set, Delete,
    Create, Table, Drop,
    Asc, Desc,
    Integer, Int, Double, Varchar,
    // literals
    Identifier,
    IntLiteral, DoubleLiteral, StringLiteral,
    // punctuation
    Comma, LParen, RParen, Dot, Star, Semicolon,
    // operators
    Eq, Ne, Lt, Le, Gt, Ge, Plus, Minus, Slash, Percent,
    // end of input
    End
};

struct Token {
    TokenType type = TokenType::End;
    std::string text;      // identifiers keep their case, strings are unescaped
                           // to their value, numbers keep their spelling
    std::size_t line = 1;  // 1-based source line of the token's first character
};

// Case-insensitive keyword lookup for a word-shaped lexeme; Identifier
// when the word is not a keyword.
inline TokenType keyword_of(std::string_view word) {
    static constexpr struct {
        std::string_view word;
        TokenType type;
    } table[] = {
        {"and", TokenType::And},         {"or", TokenType::Or},
        {"not", TokenType::Not},         {"select", TokenType::Select},
        {"from", TokenType::From},       {"where", TokenType::Where},
        {"group", TokenType::Group},     {"by", TokenType::By},
        {"order", TokenType::Order},     {"limit", TokenType::Limit},
        {"join", TokenType::Join},       {"on", TokenType::On},
        {"as", TokenType::As},           {"insert", TokenType::Insert},
        {"into", TokenType::Into},       {"values", TokenType::Values},
        {"update", TokenType::Update},   {"set", TokenType::Set},
        {"delete", TokenType::Delete},   {"create", TokenType::Create},
        {"table", TokenType::Table},     {"drop", TokenType::Drop},
        {"asc", TokenType::Asc},         {"desc", TokenType::Desc},
        {"integer", TokenType::Integer}, {"int", TokenType::Int},
        {"double", TokenType::Double},   {"varchar", TokenType::Varchar},
    };
    for (const auto& entry : table) {
        if (word.size() != entry.word.size()) continue;
        std::size_t i = 0;
        while (i < word.size() &&
               std::tolower(static_cast<unsigned char>(word[i])) == entry.word[i]) {
            ++i;
        }
        if (i == word.size()) return entry.type;
    }
    return TokenType::Identifier;
}

}
