#include "Lexer.h"

#include <cctype>

namespace sql {

namespace {

bool is_ident_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool is_digit(char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

class Scanner {
public:
    Scanner(const std::string& source, std::vector<Token>& tokens, std::string& error)
        : src_(source), tokens_(tokens), error_(error) {}

    bool scan() {
        while (!done()) {
            const char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                if (c == '\n') ++line_;
                ++pos_;
            } else if (c == '-' && peek(1) == '-') {
                skip_comment();
            } else if (is_ident_start(c)) {
                scan_word();
            } else if (is_digit(c)) {
                if (!scan_number()) return false;
            } else if (c == '\'') {
                if (!scan_string()) return false;
            } else if (!scan_symbol()) {
                return false;
            }
        }
        tokens_.push_back({TokenType::End, "", line_});
        return true;
    }

private:
    bool done() const { return pos_ >= src_.size(); }

    char peek(std::size_t ahead = 0) const {
        return pos_ + ahead < src_.size() ? src_[pos_ + ahead] : '\0';
    }

    bool fail(const std::string& message) { return fail_at(line_, message); }

    bool fail_at(std::size_t line, const std::string& message) {
        if (error_.empty()) error_ = "line " + std::to_string(line) + ": " + message;
        return false;
    }

    void skip_comment() {
        while (!done() && src_[pos_] != '\n') ++pos_;
    }

    void scan_word() {
        const std::size_t start = pos_;
        while (!done() && is_ident_char(src_[pos_])) ++pos_;
        const std::string word = src_.substr(start, pos_ - start);
        tokens_.push_back({keyword_of(word), word, line_});
    }

    bool scan_number() {
        const std::size_t start = pos_;
        while (is_digit(peek())) ++pos_;
        bool is_double = false;
        if (peek() == '.') {
            if (!is_digit(peek(1))) return malformed_number(start);
            ++pos_;
            while (is_digit(peek())) ++pos_;
            is_double = true;
        }
        if (peek() == '.') return malformed_number(start);
        const std::string lexeme = src_.substr(start, pos_ - start);
        tokens_.push_back(
            {is_double ? TokenType::DoubleLiteral : TokenType::IntLiteral, lexeme, line_});
        return true;
    }

    bool malformed_number(std::size_t start) {
        while (is_digit(peek()) || peek() == '.') ++pos_;
        return fail("malformed number '" + src_.substr(start, pos_ - start) + "'");
    }

    bool scan_string() {
        const std::size_t token_line = line_;
        ++pos_;  // opening quote
        std::string value;
        for (;;) {
            if (done()) return fail_at(token_line, "unterminated string");
            const char c = src_[pos_];
            if (c == '\n') {
                ++line_;
                ++pos_;
                value += c;
            } else if (c == '\'') {
                if (peek(1) == '\'') {
                    value += '\'';
                    pos_ += 2;
                } else {
                    ++pos_;
                    break;
                }
            } else {
                value += c;
                ++pos_;
            }
        }
        tokens_.push_back({TokenType::StringLiteral, value, token_line});
        return true;
    }

    bool scan_symbol() {
        const std::size_t start = pos_;
        TokenType type;
        switch (src_[pos_]) {
            case ',': type = TokenType::Comma; ++pos_; break;
            case '(': type = TokenType::LParen; ++pos_; break;
            case ')': type = TokenType::RParen; ++pos_; break;
            case '.': type = TokenType::Dot; ++pos_; break;
            case '*': type = TokenType::Star; ++pos_; break;
            case ';': type = TokenType::Semicolon; ++pos_; break;
            case '+': type = TokenType::Plus; ++pos_; break;
            case '-': type = TokenType::Minus; ++pos_; break;
            case '/': type = TokenType::Slash; ++pos_; break;
            case '%': type = TokenType::Percent; ++pos_; break;
            case '=': type = TokenType::Eq; ++pos_; break;
            case '<':
                if (peek(1) == '>') {
                    pos_ += 2;
                    type = TokenType::Ne;
                } else if (peek(1) == '=') {
                    pos_ += 2;
                    type = TokenType::Le;
                } else {
                    ++pos_;
                    type = TokenType::Lt;
                }
                break;
            case '>':
                if (peek(1) == '=') {
                    pos_ += 2;
                    type = TokenType::Ge;
                } else {
                    ++pos_;
                    type = TokenType::Gt;
                }
                break;
            case '!':
                if (peek(1) == '=') {
                    pos_ += 2;
                    type = TokenType::Ne;
                } else {
                    return fail("unexpected character '!'");
                }
                break;
            default:
                return fail("unexpected character '" + std::string(1, src_[pos_]) + "'");
        }
        tokens_.push_back({type, src_.substr(start, pos_ - start), line_});
        return true;
    }

    const std::string& src_;
    std::vector<Token>& tokens_;
    std::string& error_;
    std::size_t pos_ = 0;
    std::size_t line_ = 1;
};

}

bool tokenize(const std::string& source, std::vector<Token>& tokens, std::string& error) {
    tokens.clear();
    error.clear();
    Scanner scanner(source, tokens, error);
    if (!scanner.scan()) {
        tokens.clear();
        return false;
    }
    return true;
}

}
