#include "schema/TableSchemaJson.h"

#include <climits>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace schema {

namespace {

// One parsed column entry, held until the whole catalog parses cleanly —
// the schema is built only after every check has passed, so a failure
// mid-file never leaves a half-loaded schema behind.
struct LoadedColumn {
    std::string name;
    tuple::ColumnType type = tuple::ColumnType::Integer;
    long long size = 0;
};

// A cursor over the catalog text: position, 1-based line counter, and the
// one error slot. Parse and validation stop at the first error; fail()
// only writes the first message and always returns false, so every caller
// can simply `return cur.fail(...)`.
class Cursor {
public:
    Cursor(const std::string& text, std::string& error)
        : text_(text), error_(error) {}

    bool done() const { return pos_ >= text_.size(); }
    char peek() const { return done() ? '\0' : text_[pos_]; }
    void advance() { ++pos_; }

    bool fail(const std::string& message) {
        if (error_.empty()) {
            error_ = "line " + std::to_string(line_) + ": " + message;
        }
        return false;
    }

    void skip_ws() {
        while (!done()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\r') {
                ++pos_;
            } else if (c == '\n') {
                ++pos_;
                ++line_;
            } else {
                break;
            }
        }
    }

    bool expect(char c, const std::string& what) {
        skip_ws();
        if (peek() != c) {
            return fail("expected '" + std::string(1, c) + "' " + what);
        }
        ++pos_;
        return true;
    }

    // A JSON string: double-quoted, standard escapes, \uXXXX for BMP code
    // points (encoded as UTF-8).
    bool parse_string(std::string& out) {
        skip_ws();
        if (peek() == 't' || peek() == 'f' || peek() == 'n') {
            return fail("the catalog uses only objects, arrays, strings, and integers");
        }
        if (peek() != '"') {
            return fail("expected a string");
        }
        ++pos_;
        out.clear();
        while (true) {
            if (done()) {
                return fail("unterminated string");
            }
            const unsigned char c = static_cast<unsigned char>(text_[pos_]);
            ++pos_;
            if (c == '"') {
                return true;
            }
            if (c == '\\') {
                if (!parse_escape(out)) {
                    return false;
                }
                continue;
            }
            if (c < 0x20) {
                return fail("control character in string");
            }
            out += static_cast<char>(c);
        }
    }

    // A JSON integer: optional '-', digits, no fraction or exponent — the
    // only number shape the catalog format needs.
    bool parse_integer(long long& out) {
        skip_ws();
        if (peek() == 't' || peek() == 'f' || peek() == 'n') {
            return fail("the catalog uses only objects, arrays, strings, and integers");
        }
        bool negative = false;
        if (peek() == '-') {
            negative = true;
            ++pos_;
        }
        if (peek() < '0' || peek() > '9') {
            return fail(negative ? "expected digits after '-'" : "expected an integer");
        }
        long long value = 0;
        while (peek() >= '0' && peek() <= '9') {
            const int digit = peek() - '0';
            if (value > (LLONG_MAX - digit) / 10) {
                return fail("integer too large");
            }
            value = value * 10 + digit;
            ++pos_;
        }
        if (peek() == '.' || peek() == 'e' || peek() == 'E') {
            return fail("the catalog uses integers only");
        }
        out = negative ? -value : value;
        return true;
    }

private:
    bool parse_escape(std::string& out) {
        if (done()) {
            return fail("unterminated string");
        }
        const char e = text_[pos_];
        ++pos_;
        switch (e) {
            case '"': out += '"'; return true;
            case '\\': out += '\\'; return true;
            case '/': out += '/'; return true;
            case 'b': out += '\b'; return true;
            case 'f': out += '\f'; return true;
            case 'n': out += '\n'; return true;
            case 'r': out += '\r'; return true;
            case 't': out += '\t'; return true;
            case 'u': return parse_unicode_escape(out);
            default:
                if (e == '\n') {
                    ++line_;
                }
                return fail(std::string("unknown escape '\\") + e + "'");
        }
    }

    bool parse_unicode_escape(std::string& out) {
        unsigned code = 0;
        for (int i = 0; i < 4; ++i) {
            if (done()) {
                return fail("unterminated \\u escape");
            }
            const char h = text_[pos_];
            ++pos_;
            code <<= 4;
            if (h >= '0' && h <= '9') {
                code |= static_cast<unsigned>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
                code |= static_cast<unsigned>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
                code |= static_cast<unsigned>(h - 'A' + 10);
            } else {
                return fail("bad \\u escape");
            }
        }
        if (code >= 0xD800 && code <= 0xDFFF) {
            return fail("surrogate \\u escapes are not supported");
        }
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
        return true;
    }

    const std::string& text_;
    std::string& error_;
    std::size_t pos_ = 0;
    std::size_t line_ = 1;
};

// The "columns" array: one object per entry, each with column_name,
// column_type, and size. Entries land in `columns` in file order; the
// schema itself is built later, after every entry has passed its checks.
bool walk_columns(Cursor& cur, std::vector<LoadedColumn>& columns) {
    if (!cur.expect('[', "for 'columns'")) {
        return false;
    }
    cur.skip_ws();
    if (cur.done()) {
        return cur.fail("unterminated columns array");
    }
    if (cur.peek() == ']') {
        cur.advance();
        return cur.fail("catalog declares no columns");
    }
    std::unordered_set<std::string> seen_names;
    while (true) {
        LoadedColumn column;
        long long raw_size = 0;
        if (!cur.expect('{', "for a column entry")) {
            return false;
        }
        bool seen_name = false;
        bool seen_type = false;
        bool seen_size = false;
        cur.skip_ws();
        if (cur.done()) {
            return cur.fail("unterminated column object");
        }
        if (cur.peek() != '}') {
            while (true) {
                std::string key;
                if (!cur.parse_string(key)) {
                    return false;
                }
                if (!cur.expect(':', "after the key")) {
                    return false;
                }
                if (key == "column_name") {
                    if (seen_name) return cur.fail("duplicate key 'column_name'");
                    seen_name = true;
                    if (!cur.parse_string(column.name)) return false;
                } else if (key == "column_type") {
                    if (seen_type) return cur.fail("duplicate key 'column_type'");
                    seen_type = true;
                    std::string type_text;
                    if (!cur.parse_string(type_text)) return false;
                    if (type_text == "Integer") {
                        column.type = tuple::ColumnType::Integer;
                    } else if (type_text == "String") {
                        column.type = tuple::ColumnType::String;
                    } else if (type_text == "Double") {
                        column.type = tuple::ColumnType::Double;
                    } else {
                        return cur.fail("unknown column type '" + type_text + "'");
                    }
                } else if (key == "size") {
                    if (seen_size) return cur.fail("duplicate key 'size'");
                    seen_size = true;
                    if (!cur.parse_integer(raw_size)) return false;
                } else {
                    return cur.fail("unexpected key '" + key + "'");
                }
                cur.skip_ws();
                if (cur.done()) {
                    return cur.fail("unterminated column object");
                }
                if (cur.peek() == ',') {
                    cur.advance();
                    continue;
                }
                if (cur.peek() == '}') {
                    cur.advance();
                    break;
                }
                return cur.fail("expected ',' or '}'");
            }
        }
        // The entry is complete: check it, then keep it.
        if (!seen_name) {
            return cur.fail("column entry is missing 'column_name'");
        }
        if (!seen_type) {
            return cur.fail("column entry is missing 'column_type'");
        }
        if (!seen_size) {
            return cur.fail("column entry is missing 'size'");
        }
        if (column.name.empty()) {
            return cur.fail("column name is empty");
        }
        if (raw_size <= 0) {
            return cur.fail("column '" + column.name +
                            "': size must be a positive integer");
        }
        column.size = raw_size;
        if (!seen_names.insert(column.name).second) {
            return cur.fail("column '" + column.name + "' already exists");
        }
        columns.push_back(column);
        cur.skip_ws();
        if (cur.done()) {
            return cur.fail("unterminated columns array");
        }
        if (cur.peek() == ',') {
            cur.advance();
            continue;
        }
        if (cur.peek() == ']') {
            cur.advance();
            return true;
        }
        return cur.fail("expected ',' or ']'");
    }
}

// The top-level catalog object: "table_name" and "columns", both required,
// unknown keys refused — a fixed format, and a typo'd key should be an
// error, not a silently ignored member.
bool walk_catalog(Cursor& cur, std::string& table_name,
                  std::vector<LoadedColumn>& columns) {
    if (!cur.expect('{', "opening the catalog")) {
        return false;
    }
    bool seen_name = false;
    bool seen_columns = false;
    cur.skip_ws();
    if (cur.done()) {
        return cur.fail("unterminated catalog object");
    }
    if (cur.peek() != '}') {
        while (true) {
            std::string key;
            if (!cur.parse_string(key)) {
                return false;
            }
            if (!cur.expect(':', "after the key")) {
                return false;
            }
            if (key == "table_name") {
                if (seen_name) return cur.fail("duplicate key 'table_name'");
                seen_name = true;
                if (!cur.parse_string(table_name)) return false;
            } else if (key == "columns") {
                if (seen_columns) return cur.fail("duplicate key 'columns'");
                seen_columns = true;
                if (!walk_columns(cur, columns)) return false;
            } else {
                return cur.fail("unexpected key '" + key + "'");
            }
            cur.skip_ws();
            if (cur.done()) {
                return cur.fail("unterminated catalog object");
            }
            if (cur.peek() == ',') {
                cur.advance();
                continue;
            }
            if (cur.peek() == '}') {
                cur.advance();
                break;
            }
            return cur.fail("expected ',' or '}'");
        }
    }
    if (!seen_name) {
        return cur.fail("catalog is missing 'table_name'");
    }
    if (!seen_columns) {
        return cur.fail("catalog is missing 'columns'");
    }
    return true;
}

}

bool parse_table_schema(const std::string& text, TableSchema& schema,
                        std::string& error) {
    error.clear();
    schema = TableSchema(""); // the out-schema is empty from here on
    Cursor cur(text, error);
    std::string table_name;
    std::vector<LoadedColumn> columns;
    if (!walk_catalog(cur, table_name, columns)) {
        return false;
    }
    cur.skip_ws();
    if (!cur.done()) {
        return cur.fail("unexpected text after the catalog");
    }
    if (table_name.empty()) {
        return cur.fail("table name is empty");
    }
    // All checks passed: build through add_column, the one writer of the
    // name map, the positions, and the identity column_order.
    TableSchema loaded(table_name);
    for (const LoadedColumn& column : columns) {
        if (!loaded.add_column(column.name, column.type, error)) {
            return false; // unreachable: names were pre-checked
        }
        loaded.column_info_.back().set_column_size(
            static_cast<std::size_t>(column.size));
    }
    schema = loaded;
    return true;
}

bool load_table_schema(const std::string& path, TableSchema& schema,
                       std::string& error) {
    error.clear();
    schema = TableSchema("");
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open '" + path + "'";
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return parse_table_schema(text.str(), schema, error);
}

}
