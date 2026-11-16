#include "schema/TableCsv.h"

#include "common/Error.h"
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

}

void parse_csv_row(const TableSchema& schema, const std::string& line,
                   tuple::Tuple& tuple) {
    tuple.clear();
    if (line.empty()) {
        throw common::Error("empty line");
    }
    const std::vector<std::string> tokens = split_row(line);
    if (tokens.size() != schema.column_count()) {
        throw common::Error("row has " + std::to_string(tokens.size()) +
                            " column(s), the table has " +
                            std::to_string(schema.column_count()));
    }
    tuple::Tuple out;
    for (std::size_t p = 0; p < schema.column_count(); ++p) {
        const ColumnSchema& column = schema.column_info(p);
        const std::string& token = tokens[p];
        const char* begin = token.data();
        const char* end = begin + token.size();
        switch (column.column_type()) {
            case tuple::ColumnType::Integer: {
                int value = 0;
                const auto result = std::from_chars(begin, end, value);
                if (result.ec != std::errc{} || result.ptr != end) {
                    throw common::Error("column '" + column.column_name() +
                                        "': expected an integer");
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
                    throw common::Error("column '" + column.column_name() +
                                        "': expected a number");
                }
                errno = 0;
                char* parse_end = nullptr;
                const double value = std::strtod(token.c_str(), &parse_end);
                if (parse_end != end || errno == ERANGE) {
                    throw common::Error("column '" + column.column_name() +
                                        "': expected a number");
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
}

void load_csv(const TableSchema& schema, const std::string& path,
              CsvLoadResult& result, std::ostream& diagnostics) {
    result.tuples.clear();
    result.skipped = 0;
    std::ifstream in(path);
    if (!in) {
        throw common::Error("cannot open '" + path + "'");
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
        try {
            parse_csv_row(schema, line, tuple);
        } catch (const common::Error& e) {
            ++result.skipped;
            diagnostics << "line " << line_number << ": " << e.what() << "\n";
            continue;
        }
        result.tuples.push_back(std::move(tuple));
    }
}

}
