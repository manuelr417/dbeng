// ex11_cli — the p1dbeng_demo-equivalent session CLI over the tuple heap
// file (see cli.md). Six commands: append, read, seek, scan, bulk, quit.
// One BufferManager for the whole session, shared by every heap file; the
// <table> argument is a catalog name resolved to tablecatalog/<name>.json.

#include "bufpool/BufferManager.h"
#include "file/BlockFile.h"
#include "heapfile/HeapFile.h"
#include "policy/LRUPolicy.h"
#include "schema/TableCsv.h"
#include "schema/TableGenerator.h"
#include "schema/TableSchema.h"
#include "schema/TableSchemaJson.h"
#include "tuple/Column.h"
#include "tuple/Tuple.h"

#include <charconv>
#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr const char* kCatalogDirectory = "tablecatalog";
constexpr std::size_t kPoolFrames = 8;

bool parse_uint64(const std::string& text, std::uint64_t& value) {
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

// The CLI's addressing unit: a (page, slot) argument pair narrowed to a
// RowId. False (with a message from the caller) on garbage or overflow.
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

// One generic printer for every table: each column's name and value.
std::string print_tuple(const tuple::Tuple& tuple,
                        const schema::TableSchema& schema) {
    std::string out;
    for (std::size_t i = 0; i < schema.column_count(); ++i) {
        const schema::ColumnSchema& column = schema.column_info(i);
        if (i != 0) {
            out += ", ";
        }
        out += column.column_name() + "=";
        const tuple::Column& value =
            tuple.column(column.column_position());
        switch (value.column_type()) {
            case tuple::ColumnType::Integer:
                out += std::to_string(std::get<int>(value.column_value()));
                break;
            case tuple::ColumnType::Double:
                out += std::to_string(std::get<double>(value.column_value()));
                break;
            case tuple::ColumnType::String:
                out += std::get<std::string>(value.column_value());
                break;
        }
    }
    return out;
}

// --- Session state: one manager, the catalog cache, the file table. -----

struct SessionFile {
    std::unique_ptr<heapfile::HeapFile> file; // bound to its table's schema
    const schema::TableSchema* schema;        // owned by the catalog cache
};

class Session {
public:
    Session() : manager_(std::make_unique<bufman::BufferManager>()) {
        manager_->init(kPoolFrames, std::make_unique<bufman::LRUPolicy>());
    }

    bufman::BufferManager& manager() { return *manager_; }

    // Resolves a table name to its schema, loading tablecatalog/<name>.json
    // once per name. A path or an extension is refused with a hint: the
    // CLI adds both parts itself.
    const schema::TableSchema* table(const std::string& name,
                                     std::ostream& diagnostics) {
        if (name.find('/') != std::string::npos ||
            (name.size() >= 5 &&
             name.compare(name.size() - 5, 5, ".json") == 0)) {
            diagnostics << "use the table name (e.g. person), not a path\n";
            return nullptr;
        }
        const auto found = catalogs_.find(name);
        if (found != catalogs_.end()) {
            return &found->second;
        }
        schema::TableSchema schema("");
        std::string error;
        const std::string path =
            std::string(kCatalogDirectory) + "/" + name + ".json";
        if (!schema::load_table_schema(path, schema, error)) {
            diagnostics << "unknown table '" << name << "' (" << error
                        << ")\n";
            return nullptr;
        }
        const auto inserted =
            catalogs_.emplace(name, std::move(schema)).first;
        return &inserted->second;
    }

    // Returns the session file for `path`, opened on first use. With
    // create_when_missing, a missing or empty path is formatted first —
    // p1dbeng's append-to-new-file behavior. A file already open under a
    // different table is refused.
    SessionFile* file_for(const std::string& path,
                          const schema::TableSchema* schema,
                          bool create_when_missing,
                          std::ostream& diagnostics) {
        const auto found = files_.find(path);
        if (found != files_.end()) {
            if (found->second.schema->table_name() != schema->table_name()) {
                diagnostics << "'" << path << "' is bound to table '"
                            << found->second.schema->table_name() << "'\n";
                return nullptr;
            }
            return &found->second;
        }
        auto file = std::make_unique<heapfile::HeapFile>(*manager_);
        std::string error;
        if (!file->open(path, *schema, error)) {
            if (!create_when_missing || !file->create(path, *schema, error) ||
                !file->open(path, *schema, error)) {
                diagnostics << error << "\n";
                return nullptr;
            }
        }
        const auto inserted = files_
                                  .emplace(path,
                                           SessionFile{std::move(file),
                                                       schema})
                                  .first;
        return &inserted->second;
    }

    // quit/EOF: close every session file exactly once — one flush per file
    // through the shared manager. False iff any flush failed.
    bool close_all_files(std::ostream& diagnostics) {
        bool ok = true;
        for (auto& entry : files_) {
            std::string error;
            if (!entry.second.file->close(error)) {
                diagnostics << error << "\n";
                ok = false;
            }
        }
        files_.clear();
        return ok;
    }

private:
    std::unique_ptr<bufman::BufferManager> manager_;
    std::unordered_map<std::string, schema::TableSchema> catalogs_;
    std::unordered_map<std::string, SessionFile> files_;
};

// --- Command implementations --------------------------------------------

// append <table> <csv-file> <file>: load_csv, create-if-missing, insert
// per tuple. Invalid rows were already reported by the loader.
void cmd_append(Session& session, const std::vector<std::string>& args) {
    const schema::TableSchema* schema = session.table(args[1], std::cerr);
    if (schema == nullptr) {
        return;
    }
    std::ostringstream loader_diagnostics;
    schema::CsvLoadResult loaded;
    std::string error;
    if (!schema::load_csv(*schema, args[2], loaded, loader_diagnostics,
                          error)) {
        std::cerr << error << "\n";
        return;
    }
    std::cerr << loader_diagnostics.str();
    if (loaded.tuples.empty()) {
        if (loaded.skipped != 0) {
            std::cerr << "no valid record(s) to append\n";
        }
        return;
    }
    SessionFile* entry = session.file_for(args[3], schema, true, std::cerr);
    if (entry == nullptr) {
        return;
    }
    heapfile::RowId row_id;
    std::size_t inserted = 0;
    std::size_t refused = 0;
    for (const tuple::Tuple& tuple : loaded.tuples) {
        if (!entry->file->insert(tuple, row_id, error)) {
            // A row that parses but violates the schema (e.g. a too-wide
            // string) is skipped, never fatal — p1dbeng's contract, with
            // the refusal surfacing here because width lives at insert.
            std::cerr << error << "\n";
            ++refused;
            continue;
        }
        ++inserted;
    }
    std::string flush_error;
    if (!session.manager().flush_all(flush_error)) {
        std::cerr << flush_error << "\n";
    }
    std::cout << "appended " << inserted << " record(s), skipped "
              << loaded.skipped + refused << " row(s)\n";
}

// read <table> <file> <page> <slot>: one record by row id.
void cmd_read(Session& session, const std::vector<std::string>& args) {
    const schema::TableSchema* schema = session.table(args[1], std::cerr);
    if (schema == nullptr) {
        return;
    }
    SessionFile* entry = session.file_for(args[2], schema, false, std::cerr);
    if (entry == nullptr) {
        return;
    }
    heapfile::RowId row_id;
    if (!parse_row_id(args[3], args[4], row_id)) {
        std::cerr << "row id must be two nonnegative integers: page slot\n";
        return;
    }
    const bool hit = entry->file->resident(row_id.page_id);
    tuple::Tuple tuple;
    std::string error;
    if (!entry->file->find(row_id, tuple, error)) {
        std::cerr << error << "\n";
        return;
    }
    std::cout << "(" << row_id.page_id << "," << row_id.slot << ")  "
              << (hit ? "(pool hit)  " : "")
              << print_tuple(tuple, *schema) << "\n";
}

// seek <table> <file> <page> <slot>...: the batch form of read.
void cmd_seek(Session& session, const std::vector<std::string>& args) {
    const schema::TableSchema* schema = session.table(args[1], std::cerr);
    if (schema == nullptr) {
        return;
    }
    SessionFile* entry = session.file_for(args[2], schema, false, std::cerr);
    if (entry == nullptr) {
        return;
    }
    for (std::size_t i = 3; i + 1 < args.size(); i += 2) {
        heapfile::RowId row_id;
        if (!parse_row_id(args[i], args[i + 1], row_id)) {
            std::cerr << "row id '" << args[i] << " " << args[i + 1]
                      << "': must be two nonnegative integers\n";
            continue;
        }
        const bool hit = entry->file->resident(row_id.page_id);
        tuple::Tuple tuple;
        std::string error;
        if (!entry->file->find(row_id, tuple, error)) {
            std::cerr << error << "\n";
            continue;
        }
        std::cout << "(" << row_id.page_id << "," << row_id.slot << ")  "
                  << (hit ? "(pool hit)  " : "")
                  << print_tuple(tuple, *schema) << "\n";
    }
}

// scan <table> <file>: the whole chain, with row ids.
void cmd_scan(Session& session, const std::vector<std::string>& args) {
    const schema::TableSchema* schema = session.table(args[1], std::cerr);
    if (schema == nullptr) {
        return;
    }
    SessionFile* entry = session.file_for(args[2], schema, false, std::cerr);
    if (entry == nullptr) {
        return;
    }
    std::size_t count = 0;
    heapfile::HeapFileIterator it = entry->file->scan();
    tuple::Tuple tuple;
    std::string error;
    while (it.next(tuple, error)) {
        const heapfile::RowId& row_id = it.row_id();
        std::cout << "(" << row_id.page_id << "," << row_id.slot << ")  "
                  << print_tuple(tuple, *schema) << "\n";
        ++count;
    }
    if (!error.empty()) {
        std::cerr << error << "\n";
        return;
    }
    std::cout << "scanned " << count << " record(s)\n";
}

// bulk <table> <count> <file>: generator rows, numbering continuing after
// the existing record count.
void cmd_bulk(Session& session, const std::vector<std::string>& args) {
    const schema::TableSchema* schema = session.table(args[1], std::cerr);
    if (schema == nullptr) {
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
    SessionFile* entry = session.file_for(args[3], schema, true, std::cerr);
    if (entry == nullptr) {
        return;
    }
    std::size_t existing = 0;
    heapfile::HeapFileIterator it = entry->file->scan();
    tuple::Tuple tuple;
    std::string error;
    while (it.next(tuple, error)) {
        ++existing;
    }
    if (!error.empty()) {
        std::cerr << error << "\n";
        return;
    }
    const std::vector<tuple::Tuple> generated = schema::generate_tuples(
        *schema, static_cast<std::size_t>(count), 42,
        existing + 1);
    heapfile::RowId row_id;
    for (const tuple::Tuple& generated_tuple : generated) {
        if (!entry->file->insert(generated_tuple, row_id, error)) {
            std::cerr << error << "\n";
            return;
        }
    }
    std::string flush_error;
    if (!session.manager().flush_all(flush_error)) {
        std::cerr << flush_error << "\n";
    }
    std::cout << "appended " << generated.size()
              << " generated record(s)\n";
}

// --- REPL plumbing -------------------------------------------------------

void print_help() {
    std::cerr << "Commands:\n"
              << "  append <table> <csv-file> <file>\n"
              << "  read <table> <file> <page> <slot>\n"
              << "  seek <table> <file> <page> <slot>...\n"
              << "  scan <table> <file>\n"
              << "  bulk <table> <count> <file>\n"
              << "  quit\n"
              << "Tables: the names in tablecatalog/ (e.g. person, car)\n";
}

void usage_hint(const std::string& command) {
    static const std::unordered_map<std::string, std::string> kUsage = {
        {"append", "append <table> <csv-file> <file>"},
        {"read", "read <table> <file> <page> <slot>"},
        {"seek", "seek <table> <file> <page> <slot>..."},
        {"scan", "scan <table> <file>"},
        {"bulk", "bulk <table> <count> <file>"},
    };
    const auto found = kUsage.find(command);
    if (found != kUsage.end()) {
        std::cerr << "usage: " << found->second << "\n";
    } else {
        print_help();
    }
}

}

int main() {
    Session session;

    // The prompt is suppressed when input is piped, so sessions script.
    const bool interactive = isatty(fileno(stdin)) != 0;
    for (;;) {
        if (interactive) {
            std::cout << "> " << std::flush;
        }
        std::string line;
        if (!std::getline(std::cin, line)) {
            if (interactive) {
                std::cout << "\n";
            }
            break;
        }

        std::istringstream input(line);
        std::vector<std::string> args;
        std::string token;
        while (input >> token) {
            args.push_back(token);
        }
        if (args.empty()) {
            continue;
        }
        const std::string& command = args[0];
        if (command == "quit") {
            break;
        }

        if (command == "append") {
            if (args.size() != 4) {
                usage_hint(command);
            } else {
                cmd_append(session, args);
            }
        } else if (command == "read") {
            if (args.size() != 5) {
                usage_hint(command);
            } else {
                cmd_read(session, args);
            }
        } else if (command == "seek") {
            if (args.size() < 5 || (args.size() - 3) % 2 != 0) {
                usage_hint(command);
            } else {
                cmd_seek(session, args);
            }
        } else if (command == "scan") {
            if (args.size() != 3) {
                usage_hint(command);
            } else {
                cmd_scan(session, args);
            }
        } else if (command == "bulk") {
            if (args.size() != 4) {
                usage_hint(command);
            } else {
                cmd_bulk(session, args);
            }
        } else {
            usage_hint(command);
        }
    }

    // Flush every session file exactly once; exit code 1 only on failure.
    const bool flushed = session.close_all_files(std::cerr);
    return flushed ? 0 : 1;
}
