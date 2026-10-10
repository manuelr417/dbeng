// ex13_cli — the session CLI: a SQL REPL over the ex11 storage engine
// plus ISAM indexes. SQL statements accumulate across lines until a
// semicolon; the ex11 row-id commands (scan, read, seek, bulk) run bare
// as escape hatches, and checkindex verifies a data file's indexes.
// One BufferManager (LRU, 8 frames) lives for the whole session and is
// shared by every heap file and index file. Errors print and the
// session continues; exit code 1 only if a flush fails at quit.

#include "bufpool/BufferManager.h"
#include "exec/Executor.h"
#include "policy/LRUPolicy.h"
#include "schema/TableGenerator.h"
#include "schema/TableSchema.h"
#include "schema/TableSchemaJson.h"
#include "sql/Parser.h"
#include "tuple/Tuple.h"

#include <cctype>
#include <charconv>
#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::size_t kPoolFrames = 8;

bool is_escape_command(const std::string& word) {
    return word == "scan" || word == "read" || word == "seek" ||
           word == "bulk" || word == "checkindex" || word == "quit" ||
           word == "help";
}

bool parse_uint64(const std::string& text, std::uint64_t& value) {
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

bool parse_row_id(const std::string& page_text, const std::string& slot_text,
                  heapfile::RowId& row_id) {
    std::uint64_t page = 0;
    std::uint64_t slot = 0;
    if (!parse_uint64(page_text, page) || !parse_uint64(slot_text, slot) ||
        page > 0xFFFFFFFFu || slot > 0xFFFFFFFFu) {
        return false;
    }
    row_id = heapfile::RowId{static_cast<std::uint32_t>(page),
                             static_cast<std::uint32_t>(slot)};
    return true;
}

// Escape-hatch path resolution (option a): a bare file name — no slash —
// lives in the canonical data directory under the canonical name, so it
// gains the `.tbl` extension unless it already carries it; anything
// containing a slash is used exactly as given, so absolute and
// subdirectory paths keep working.
std::string resolve_data_path(const std::string& argument) {
    if (argument.find('/') != std::string::npos) {
        return argument;
    }
    std::string name = argument;
    const std::string extension = ".tbl";
    if (name.size() < extension.size() ||
        name.compare(name.size() - extension.size(), extension.size(),
                     extension) != 0) {
        name += extension;
    }
    return "data/" + name;
}

std::string print_value(const exec::Value& value) {
    if (const int* v = std::get_if<int>(&value)) {
        return std::to_string(*v);
    }
    if (const double* v = std::get_if<double>(&value)) {
        char buffer[64];
        const auto written =
            std::to_chars(buffer, buffer + sizeof(buffer), *v);
        return std::string(buffer, written.ptr);
    }
    return std::get<std::string>(value);
}

// --- the escape-hatch commands (ex11 semantics) -----------------------------

void cmd_scan(exec::Executor& executor, const std::vector<std::string>& args) {
    std::vector<schema::TableIndex> indexes;
    std::string error;
    const schema::TableSchema* schema =
        executor.table(args[1], indexes, error);
    if (schema == nullptr) {
        std::cerr << error << "\n";
        return;
    }
    heapfile::HeapFile* file =
        executor.file_for(args[1], resolve_data_path(args[2]), *schema,
                          false, error);
    if (file == nullptr) {
        std::cerr << error << "\n";
        return;
    }
    std::size_t count = 0;
    heapfile::HeapFileIterator it = file->scan();
    tuple::Tuple tuple;
    while (it.next(tuple, error)) {
        const heapfile::RowId& row_id = it.row_id();
        std::cout << "(" << row_id.page_id << "," << row_id.slot << ")  ";
        for (std::size_t i = 0; i < schema->column_count(); ++i) {
            if (i != 0) {
                std::cout << ", ";
            }
            std::cout << schema->column_info(i).column_name() << "=";
            const tuple::Column& column = tuple.column(i);
            switch (column.column_type()) {
                case tuple::ColumnType::Integer:
                    std::cout << std::get<int>(column.column_value());
                    break;
                case tuple::ColumnType::Double:
                    std::cout << std::get<double>(column.column_value());
                    break;
                case tuple::ColumnType::String:
                    std::cout << std::get<std::string>(column.column_value());
                    break;
            }
        }
        std::cout << "\n";
        ++count;
    }
    if (!error.empty()) {
        std::cerr << error << "\n";
        return;
    }
    std::cout << "scanned " << count << " record(s)\n";
}

void cmd_read(exec::Executor& executor, const std::vector<std::string>& args) {
    std::vector<schema::TableIndex> indexes;
    std::string error;
    const schema::TableSchema* schema =
        executor.table(args[1], indexes, error);
    if (schema == nullptr) {
        std::cerr << error << "\n";
        return;
    }
    heapfile::HeapFile* file =
        executor.file_for(args[1], resolve_data_path(args[2]), *schema,
                          false, error);
    if (file == nullptr) {
        std::cerr << error << "\n";
        return;
    }
    heapfile::RowId row_id;
    if (!parse_row_id(args[3], args[4], row_id)) {
        std::cerr << "row id must be two nonnegative integers: page slot\n";
        return;
    }
    const bool hit = file->resident(row_id.page_id);
    tuple::Tuple tuple;
    if (!file->find(row_id, tuple, error)) {
        std::cerr << error << "\n";
        return;
    }
    std::cout << "(" << row_id.page_id << "," << row_id.slot << ")  "
              << (hit ? "(pool hit)  " : "");
    for (std::size_t i = 0; i < schema->column_count(); ++i) {
        if (i != 0) {
            std::cout << ", ";
        }
        std::cout << schema->column_info(i).column_name() << "=";
        const tuple::Column& column = tuple.column(i);
        switch (column.column_type()) {
            case tuple::ColumnType::Integer:
                std::cout << std::get<int>(column.column_value());
                break;
            case tuple::ColumnType::Double:
                std::cout << std::get<double>(column.column_value());
                break;
            case tuple::ColumnType::String:
                std::cout << std::get<std::string>(column.column_value());
                break;
        }
    }
    std::cout << "\n";
}

void cmd_seek(exec::Executor& executor, const std::vector<std::string>& args) {
    for (std::size_t i = 3; i + 1 < args.size(); i += 2) {
        // One read per row id: reuse cmd_read's printing through a
        // sub-argument vector (page, slot pair at position 3).
        std::vector<std::string> single = {
            args[0], args[1], resolve_data_path(args[2]), args[i],
            args[i + 1]};
        cmd_read(executor, single);
    }
}

void cmd_bulk(exec::Executor& executor, const std::vector<std::string>& args) {
    std::vector<schema::TableIndex> indexes;
    std::string error;
    const schema::TableSchema* schema =
        executor.table(args[1], indexes, error);
    if (schema == nullptr) {
        std::cerr << error << "\n";
        return;
    }
    std::uint64_t count = 0;
    if (!parse_uint64(args[2], count)) {
        std::cerr << "count must be a nonnegative integer\n";
        return;
    }
    if (count == 0) {
        std::cout << "nothing to insert\n";
        return;
    }
    const std::string data_file = resolve_data_path(args[3]);
    std::size_t existing = 0;
    // A fresh table has no data file yet: its count is zero and the
    // first generated row numbers at 1 (the ex11 semantics).
    if (!executor.count_rows(args[1], data_file, existing, error, true)) {
        std::cerr << error << "\n";
        return;
    }
    const std::vector<tuple::Tuple> generated =
        schema::generate_tuples(*schema, static_cast<std::size_t>(count), 42,
                                existing + 1);
    for (const tuple::Tuple& tuple : generated) {
        heapfile::RowId row_id;
        if (!executor.insert_into(args[1], data_file, tuple, row_id,
                                  error)) {
            std::cerr << error << "\n";
            return;
        }
    }
    std::cout << "appended " << generated.size()
              << " generated record(s)\n";
}

void cmd_checkindex(exec::Executor& executor,
                    const std::vector<std::string>& args) {
    std::vector<std::string> report;
    bool clean = true;
    std::string error;
    if (!executor.check_indexes(args[1], resolve_data_path(args[2]),
                                report, clean, error)) {
        std::cerr << error << "\n";
        return;
    }
    for (const std::string& line : report) {
        std::cout << line << "\n";
    }
}

void print_help() {
    std::cerr << "SQL statements end with ';'. Escape-hatch commands:\n"
              << "  scan <table> <file>\n"
              << "  read <table> <file> <page> <slot>\n"
              << "  seek <table> <file> <page> <slot>...\n"
              << "  bulk <table> <count> <file>\n"
              << "  checkindex <table> <file>\n"
              << "  quit\n"
              << "Tables: the names in tablecatalog/ (e.g. person, car)\n";
}

// Executes one SQL statement's text and prints the result.
void execute_statement(exec::Executor& executor, const std::string& text) {
    sql::ParseResult parsed;
    std::string error;
    if (!sql::parse(text, parsed, error)) {
        std::cerr << error << "\n";
        return;
    }
    for (std::unique_ptr<sql::Statement>& statement : parsed.statements) {
        exec::StatementResult result;
        if (!executor.execute(*statement, result, error)) {
            std::cerr << error << "\n";
            return;  // stop the line's statement batch on the first error
        }
        if (!result.rows.access_path.empty()) {
            std::cout << "(via " << result.rows.access_path << ")\n";
            for (const std::vector<exec::Value>& row : result.rows.rows) {
                for (std::size_t i = 0; i < row.size(); ++i) {
                    if (i != 0) {
                        std::cout << ", ";
                    }
                    std::cout << result.rows.columns[i] << "="
                              << print_value(row[i]);
                }
                std::cout << "\n";
            }
            std::cout << result.rows.rows.size() << " row(s)\n";
        } else if (!result.message.empty()) {
            std::cout << result.message << "\n";
        }
    }
}

}

int main() {
    bufman::BufferManager manager;
    manager.init(kPoolFrames, std::make_unique<bufman::LRUPolicy>());
    exec::Executor executor(manager);

    const bool interactive = isatty(fileno(stdin)) != 0;
    std::string pending;  // the accumulating SQL statement text
    bool quit = false;
    while (!quit) {
        if (interactive) {
            std::cout << (pending.empty() ? "> " : "  ... ") << std::flush;
        }
        std::string line;
        if (!std::getline(std::cin, line)) {
            if (interactive && pending.empty()) {
                std::cout << "\n";
            }
            break;
        }

        // Escape hatches run immediately — but only when nothing is
        // being accumulated (mid-statement text is statement text).
        if (pending.empty()) {
            std::istringstream words(line);
            std::string first;
            words >> first;
            if (first == "quit") {
                quit = true;
                break;
            }
            if (is_escape_command(first)) {
                std::vector<std::string> args;
                args.push_back(first);
                std::string token;
                while (words >> token) {
                    args.push_back(token);
                }
                static const std::unordered_map<std::string, std::size_t>
                    kArity = {{"scan", 3},   {"read", 5}, {"seek", 5},
                              {"bulk", 4},   {"checkindex", 3}};
                const auto arity = kArity.find(first);
                if (first == "help") {
                    print_help();
                } else if (arity == kArity.end() || args.size() < arity->second) {
                    std::cerr << "usage: " << first << " (see help)\n";
                } else if (first == "scan") {
                    cmd_scan(executor, args);
                } else if (first == "read") {
                    cmd_read(executor, args);
                } else if (first == "seek") {
                    cmd_seek(executor, args);
                } else if (first == "bulk") {
                    cmd_bulk(executor, args);
                } else if (first == "checkindex") {
                    cmd_checkindex(executor, args);
                }
                continue;
            }
        }

        // SQL: accumulate until the statement's semicolon, then execute
        // everything up to it (several statements may share one line).
        pending += line + "\n";
        std::size_t semicolon = pending.find(';');
        while (semicolon != std::string::npos) {
            const std::string statement_text =
                pending.substr(0, semicolon);
            pending.erase(0, semicolon + 1);
            execute_statement(executor, statement_text);
            semicolon = pending.find(';');
        }
        // A statement's trailing newline must not count as an unfinished
        // statement: pure-whitespace leftovers are cleared so escape
        // commands dispatch again.
        if (pending.find_first_not_of(" \t\r\n") == std::string::npos) {
            pending.clear();
        }
    }

    // Flush every session file exactly once; exit code 1 on failure.
    std::string close_error;
    const bool flushed = executor.close_all(close_error);
    if (!flushed) {
        std::cerr << close_error << "\n";
    }
    return flushed ? 0 : 1;
}
