#include "schema/TableCsv.h"

#include "schema/ColumnSchema.h"
#include "tuple/Column.h"

#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <utility>

namespace schema {

namespace {

// Splits on ',' — no quoting, no escaping: a comma cannot appear inside a
// value. Empty tokens are kept, so a trailing empty field survives.
std::vector<std::string> split_row(const std::string& line) {
    std::vector<std::string> tokens;
    std::size_t start = 0;
    while (true) {
        const std::size_t comma = line.find(',', start);
        if (comma == std::string::npos) {
            tokens.push_back(line.substr(start));
            break;
        }
        tokens.push_back(line.substr(start, comma - start));
        start = comma + 1;
    }
    return tokens;
}

const ColumnSchema& column_at_position(const TableSchema& schema,
                                       std::size_t position) {
    for (std::size_t i = 0; i < schema.column_count(); ++i) {
        if (schema.column_info(i).column_position() == position) {
            return schema.column_info(i);
        }
    }
    return schema.column_info(0); // unreachable: positions are a permutation
}

}

bool parse_csv_row(const TableSchema& schema, const std::string& line,
                   tuple::Tuple& tuple, std::string& reason) {
    reason.clear();
    tuple.clear();
    if (line.empty()) {
        reason = "empty line";
        return false;
    }
    const std::vector<std::string> tokens = split_row(line);
    if (tokens.size() != schema.column_count()) {
        reason = "row has " + std::to_string(tokens.size()) +
                 " column(s), the table has " +
                 std::to_string(schema.column_count());
        return false;
    }
    tuple::Tuple out;
    for (std::size_t p = 0; p < schema.column_count(); ++p) {
        const ColumnSchema& column = column_at_position(schema, p);
        const std::string& token = tokens[p];
        const char* begin = token.data();
        const char* end = begin + token.size();
        switch (column.column_type()) {
            case tuple::ColumnType::Integer: {
                int value = 0;
                const auto result = std::from_chars(begin, end, value);
                if (result.ec != std::errc{} || result.ptr != end) {
                    reason = "column '" + column.column_name() +
                             "': expected an integer";
                    return false;
                }
                out.add(value);
                break;
            }
            case tuple::ColumnType::Double: {
                // std::from_chars for double is unavailable on this
                // toolchain's libc++, so strtod plays the same strict role:
                // no leading whitespace, fully consumed, no overflow.
                if (token.empty() || token.front() == ' ' ||
                    token.front() == '\t') {
                    reason = "column '" + column.column_name() +
                             "': expected a number";
                    return false;
                }
                errno = 0;
                char* parse_end = nullptr;
                const double value = std::strtod(token.c_str(), &parse_end);
                if (parse_end != end || errno == ERANGE) {
                    reason = "column '" + column.column_name() +
                             "': expected a number";
                    return false;
                }
                out.add(value);
                break;
            }
            case tuple::ColumnType::String: {
                // Verbatim; width and NUL rules belong to validate/pack at
                // insert time — the module refuses a row only for syntax.
                out.add(token);
                break;
            }
        }
    }
    tuple = out;
    return true;
}

bool load_csv(const TableSchema& schema, const std::string& path,
              CsvLoadResult& result, std::ostream& diagnostics,
              std::string& error) {
    error.clear();
    result.tuples.clear();
    result.skipped = 0;
    std::ifstream in(path);
    if (!in) {
        error = "cannot open '" + path + "'";
        return false;
    }
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(in, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back(); // files written by Windows editors still load
        }
        if (line.empty()) {
            continue; // blank lines are skipped silently
        }
        tuple::Tuple tuple;
        std::string reason;
        if (!parse_csv_row(schema, line, tuple, reason)) {
            ++result.skipped;
            diagnostics << "line " << line_number << ": " << reason << "\n";
            continue;
        }
        result.tuples.push_back(std::move(tuple));
    }
    return true;
}

}
