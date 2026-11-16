#include "exec/Executor.h"

#include "common/Error.h"
#include "exec/Binder.h"
#include "exec/Evaluator.h"
#include "group/GroupBy.h"
#include "group/PartitionRun.h"
#include "schema/TableSchemaJson.h"
#include "sql/AstPrinter.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <optional>

namespace exec {

namespace {

// The group-key hash width: level L of the spill recursion routes by
// bits [L·log2K, (L+1)·log2K), so ceil(32 / log2 K) levels exhaust it
// (design D6) — the recursion's depth cap, derived from K, not fixed.
constexpr std::size_t kGroupHashBits = 32;

// SQL-created static hash indexes use this fixed bucket count (design
// D4): the teaching-scale directory, 2,720 direct slots before any
// chain grows. The module API still takes N explicitly.
constexpr std::uint32_t kDefaultStaticBuckets = 8;

// The catalog's kind string to the index kind (the catalog reader
// already validated the value).
sql::IndexKind index_kind_of(const std::string& kind) {
    if (kind == "isam") {
        return sql::IndexKind::Isam;
    }
    if (kind == "static hash") {
        return sql::IndexKind::Static;
    }
    return sql::IndexKind::Hash;
}

// The catalog's kind spelling for error messages.
const char* kind_label(sql::IndexKind kind) {
    switch (kind) {
        case sql::IndexKind::Isam:
            return "ISAM";
        case sql::IndexKind::Static:
            return "static hash";
        case sql::IndexKind::Hash:
            return "hash";
    }
    return "hash";
}

}

Executor::Executor(bufman::BufferManager& manager,
                   const std::string& catalog_directory)
    : manager_(manager), catalog_directory_(catalog_directory) {
    // The canonical home of table data: created on demand, silently
    // tolerated when it already exists (failures surface later, named,
    // at file open).
    ::mkdir("data", 0755);
}

std::string Executor::data_path(const std::string& table_name) const {
    return "data/" + table_name + ".tbl";
}

std::string Executor::index_path(const std::string& data_file,
                                 const std::string& column_name,
                                 sql::IndexKind kind) const {
    const char* extension = kind == sql::IndexKind::Hash    ? ".hdx"
                            : kind == sql::IndexKind::Isam  ? ".idx"
                                                            : ".shdx";
    return data_file + "." + column_name + extension;
}

void Executor::load_catalog(const std::string& name) {
    if (catalogs_.count(name) != 0) {
        return;
    }
    CatalogEntry entry;
    const std::string path = catalog_directory_ + "/" + name + ".json";
    try {
        schema::load_table_schema(path, entry.schema, entry.indexes);
    } catch (const common::Error& e) {
        throw common::Error("unknown table '" + name + "' (" + e.what() + ")");
    }
    catalogs_.emplace(name, std::move(entry));
}

void Executor::save_catalog(const std::string& name) {
    const auto found = catalogs_.find(name);
    if (found == catalogs_.end()) {
        throw common::Error("table '" + name + "' is not loaded");
    }
    const std::string path = catalog_directory_ + "/" + name + ".json";
    schema::save_table_schema(path, found->second.schema,
                              found->second.indexes);
}

void Executor::forget_file(const std::string& path) {
    files_.erase(path);
}

void Executor::index_insert(const SessionIndex& session_index,
                            const tuple::Column& value, isam::Rid rid) {
    switch (session_index.kind) {
        case sql::IndexKind::Hash:
            session_index.hash->insert(value, rid);
            return;
        case sql::IndexKind::Static:
            session_index.static_hash->insert(value, rid);
            return;
        case sql::IndexKind::Isam:
            session_index.index->insert(value, rid);
            return;
    }
    assert(false && "unknown index kind");
    throw common::Error("unknown index kind");
}

void Executor::index_erase(const SessionIndex& session_index,
                           const tuple::Column& value, isam::Rid rid) {
    switch (session_index.kind) {
        case sql::IndexKind::Hash:
            session_index.hash->erase(value, rid);
            return;
        case sql::IndexKind::Static:
            session_index.static_hash->erase(value, rid);
            return;
        case sql::IndexKind::Isam:
            session_index.index->erase(value, rid);
            return;
    }
    assert(false && "unknown index kind");
    throw common::Error("unknown index kind");
}

const schema::TableSchema* Executor::table(
    const std::string& name, std::vector<schema::TableIndex>& indexes) {
    load_catalog(name);
    indexes = catalogs_.at(name).indexes;
    return &catalogs_.at(name).schema;
}

heapfile::HeapFile* Executor::file_for(const std::string& table_name,
                                       const std::string& path,
                                       const schema::TableSchema& schema,
                                       bool create_when_missing) {
    const auto found = files_.find(path);
    if (found != files_.end()) {
        if (found->second.table_name != table_name) {
            throw common::Error("'" + path + "' is bound to table '" +
                                found->second.table_name + "'");
        }
        return found->second.file.get();
    }
    auto file = std::make_unique<heapfile::HeapFile>(manager_);
    try {
        file->open(path, schema);
    } catch (const common::Error&) {
        if (!create_when_missing) {
            throw;
        }
        // The last failure's message surfaces, as before: create's
        // error replaces open's, the second open's replaces create's.
        file->create(path, schema);
        file->open(path, schema);
    }
    SessionFile entry;
    entry.table_name = table_name;
    entry.file = std::move(file);

    // Open every catalog-declared index for this data file, building
    // any whose file is missing (lazy rebuild, spec: missing index file
    // rebuilt from the data file's rows, with its catalog kind).
    std::vector<schema::TableIndex> catalog_indexes;
    load_catalog(table_name);
    catalog_indexes = catalogs_.at(table_name).indexes;
    for (const schema::TableIndex& declared : catalog_indexes) {
        const std::optional<std::size_t> column =
            schema.column_index(declared.column);
        if (!column) {
            continue;  // the catalog is checked on write; tolerate here
        }
        const sql::IndexKind kind = index_kind_of(declared.kind);
        SessionIndex session_index;
        session_index.column_index = *column;
        session_index.index_name = declared.name;
        session_index.column_name = declared.column;
        session_index.kind = kind;
        if (kind == sql::IndexKind::Hash) {
            session_index.hash =
                std::make_unique<hashindex::ExtensibleHashIndex>(manager_,
                                                                 schema,
                                                                 *column);
        } else if (kind == sql::IndexKind::Static) {
            session_index.static_hash =
                std::make_unique<hashindex::StaticHashIndex>(manager_, schema,
                                                             *column);
        } else {
            session_index.index =
                std::make_unique<isam::IsamIndex>(manager_, schema, *column);
        }
        const std::string idx_path = index_path(path, declared.column, kind);
        try {
            switch (kind) {
                case sql::IndexKind::Hash:
                    session_index.hash->open(idx_path);
                    break;
                case sql::IndexKind::Static:
                    session_index.static_hash->open(idx_path);
                    break;
                case sql::IndexKind::Isam:
                    session_index.index->open(idx_path);
                    break;
            }
        } catch (const common::Error&) {
            // Build from the file's current rows. A scan or build
            // failure throws out of here; the iterator and the index
            // release their own pins and files.
            std::vector<std::pair<tuple::Column, isam::Rid>> entries;
            heapfile::HeapFileIterator it = entry.file->scan();
            tuple::Tuple tuple;
            while (it.next(tuple)) {
                entries.emplace_back(tuple.column(*column),
                                     isam::Rid{it.row_id().page_id,
                                               it.row_id().slot});
            }
            switch (kind) {
                case sql::IndexKind::Hash:
                    session_index.hash->build(idx_path, entries);
                    break;
                case sql::IndexKind::Static:
                    session_index.static_hash->build(
                        idx_path, kDefaultStaticBuckets, entries);
                    break;
                case sql::IndexKind::Isam:
                    session_index.index->build(idx_path, entries);
                    break;
            }
        }
        entry.indexes.push_back(std::move(session_index));
    }

    const auto inserted =
        files_.emplace(path, std::move(entry)).first;
    return inserted->second.file.get();
}

void Executor::close_session_indexes(SessionFile& entry) {
    std::string error;
    for (SessionIndex& session_index : entry.indexes) {
        try {
            switch (session_index.kind) {
                case sql::IndexKind::Hash:
                    session_index.hash->close();
                    break;
                case sql::IndexKind::Static:
                    session_index.static_hash->close();
                    break;
                case sql::IndexKind::Isam:
                    session_index.index->close();
                    break;
            }
        } catch (const common::Error& e) {
            if (!error.empty()) {
                error += "; ";
            }
            error += e.what();
        }
    }
    entry.indexes.clear();
    if (!error.empty()) {
        throw common::Error(error);
    }
}

void Executor::close_all() {
    std::string error;
    for (auto& [path, entry] : files_) {
        // The index files first: their dirty pages must reach disk, and
        // no frame may stay pinned across the heap file's close.
        try {
            close_session_indexes(entry);
        } catch (const common::Error& e) {
            if (!error.empty()) {
                error += "; ";
            }
            error += e.what();
        }
        try {
            entry.file->close();
        } catch (const common::Error& e) {
            if (!error.empty()) {
                error += "; ";
            }
            error += e.what();
        }
    }
    files_.clear();
    if (!error.empty()) {
        throw common::Error(error);
    }
}

void Executor::insert_into(const std::string& table_name,
                           const std::string& path,
                           const tuple::Tuple& tuple,
                           heapfile::RowId& row_id) {
    load_catalog(table_name);
    heapfile::HeapFile* file =
        file_for(table_name, path, catalogs_.at(table_name).schema, true);
    file->insert(tuple, row_id);
    const auto found = files_.find(path);
    for (const SessionIndex& session_index : found->second.indexes) {
        index_insert(session_index,
                     tuple.column(session_index.column_index),
                     isam::Rid{row_id.page_id, row_id.slot});
    }
}

void Executor::erase_row(const std::string& table_name,
                         const std::string& path, heapfile::RowId row_id) {
    load_catalog(table_name);
    const schema::TableSchema& schema = catalogs_.at(table_name).schema;
    heapfile::HeapFile* file = file_for(table_name, path, schema, false);
    // Read the row first: the index erases need the indexed values.
    tuple::Tuple tuple;
    try {
        file->find(row_id, tuple);
    } catch (const common::Error&) {
        throw common::Error("no record at row id (" +
                            std::to_string(row_id.page_id) + "," +
                            std::to_string(row_id.slot) + ") in '" + path +
                            "'");
    }
    file->erase(row_id);
    const auto found = files_.find(path);
    for (const SessionIndex& session_index : found->second.indexes) {
        index_erase(session_index,
                    tuple.column(session_index.column_index),
                    isam::Rid{row_id.page_id, row_id.slot});
    }
}

std::size_t Executor::count_rows(const std::string& table_name,
                                 const std::string& path,
                                 bool create_when_missing) {
    load_catalog(table_name);
    heapfile::HeapFile* file =
        file_for(table_name, path, catalogs_.at(table_name).schema,
                 create_when_missing);
    std::size_t count = 0;
    heapfile::HeapFileIterator it = file->scan();
    tuple::Tuple tuple;
    while (it.next(tuple)) {
        ++count;
    }
    return count;
}

void Executor::check_indexes(const std::string& table_name,
                             const std::string& path,
                             std::vector<std::string>& report, bool& clean) {
    report.clear();
    clean = true;
    load_catalog(table_name);
    const CatalogEntry& entry = catalogs_.at(table_name);
    heapfile::HeapFile* file = file_for(table_name, path, entry.schema, false);
    const auto found = files_.find(path);
    if (entry.indexes.empty()) {
        report.push_back("table '" + table_name + "' declares no indexes");
        return;
    }
    // A slot is occupied iff the heap can fetch its record.
    const auto occupied = [file](const isam::Rid& rid) {
        tuple::Tuple scratch;
        try {
            file->find(heapfile::RowId{rid.page_id, rid.slot}, scratch);
        } catch (const common::Error&) {
            return false;
        }
        return true;
    };
    for (const SessionIndex& session_index : found->second.indexes) {
        std::size_t walked = 0;
        std::vector<std::string> findings;
        switch (session_index.kind) {
            case sql::IndexKind::Hash:
                session_index.hash->verify(occupied, walked, findings);
                break;
            case sql::IndexKind::Static:
                session_index.static_hash->verify(occupied, walked, findings);
                break;
            case sql::IndexKind::Isam:
                session_index.index->verify(occupied, walked, findings);
                break;
        }
        std::string line = session_index.index_name + ": " +
                           std::to_string(walked) + " entries";
        if (findings.empty()) {
            line += ", OK";
        } else {
            clean = false;
            line += ", " + std::to_string(findings.size()) + " finding(s)";
            for (const std::string& finding : findings) {
                line += "\n  " + finding;
            }
        }
        report.push_back(line);
    }
}

// --- dispatch ----------------------------------------------------------------

void Executor::execute(const sql::Statement& statement,
                       StatementResult& result) {
    result = StatementResult{};
    switch (statement.kind) {
        case sql::Statement::Kind::CreateTable:
            handle_create_table(
                static_cast<const sql::CreateTableStatement&>(statement),
                result);
            return;
        case sql::Statement::Kind::DropTable:
            handle_drop_table(
                static_cast<const sql::DropTableStatement&>(statement),
                result);
            return;
        case sql::Statement::Kind::CreateIndex:
            handle_create_index(
                static_cast<const sql::CreateIndexStatement&>(statement),
                result);
            return;
        case sql::Statement::Kind::DropIndex:
            handle_drop_index(
                static_cast<const sql::DropIndexStatement&>(statement),
                result);
            return;
        case sql::Statement::Kind::Insert:
            handle_insert(
                static_cast<const sql::InsertStatement&>(statement), result);
            return;
        case sql::Statement::Kind::Select:
            handle_select(
                static_cast<const sql::SelectStatement&>(statement), result);
            return;
        case sql::Statement::Kind::Update:
            throw common::Error("UPDATE is not supported in ex17");
        case sql::Statement::Kind::Delete:
            throw common::Error("DELETE is not supported in ex17");
    }
    assert(false && "unknown statement kind");
    throw common::Error("unknown statement kind");
}

// --- DDL ---------------------------------------------------------------------

void Executor::handle_create_table(
    const sql::CreateTableStatement& statement, StatementResult& result) {
    const std::string catalog_path =
        catalog_directory_ + "/" + statement.table + ".json";
    // An unreadable catalog reads as absent, as before: only a loaded
    // catalog means the table already exists.
    bool exists = false;
    try {
        load_catalog(statement.table);
        exists = true;
    } catch (const common::Error&) {
    }
    if (exists) {
        throw common::Error("table '" + statement.table + "' already exists");
    }
    if (statement.columns.empty()) {
        throw common::Error("table '" + statement.table +
                            "' declares no columns");
    }
    // Build the catalog text from the column defs, then parse it through
    // the loader — the sanctioned writer of declared widths — and install
    // it through the writer. Duplicate/empty names are refused by parse.
    std::string text = "{\"table_name\":\"" + statement.table +
                       "\",\"columns\":[";
    for (std::size_t i = 0; i < statement.columns.size(); ++i) {
        const sql::ColumnDef& column = statement.columns[i];
        const char* type_text = "Integer";
        std::size_t size = 4;
        switch (column.type) {
            case sql::DataType::Integer:
                type_text = "Integer";
                size = 4;
                break;
            case sql::DataType::Double:
                type_text = "Double";
                size = 8;
                break;
            case sql::DataType::String:
                type_text = "String";
                size = column.size;
                break;
        }
        if (i > 0) {
            text += ",";
        }
        text += "{\"column_name\":\"" + column.name +
                "\",\"column_type\":\"" + type_text +
                "\",\"size\":" + std::to_string(size) + "}";
    }
    text += "],\"indexes\":[]}";
    schema::TableSchema schema("");
    std::vector<schema::TableIndex> no_indexes;
    schema::parse_table_schema(text, schema, no_indexes);
    // The catalog writer is the DDL seam: one atomic install.
    schema::save_table_schema(catalog_path, schema, {});
    CatalogEntry entry;
    entry.schema = std::move(schema);
    catalogs_.emplace(statement.table, std::move(entry));
    result.message = "table '" + statement.table + "' created";
}

void Executor::handle_drop_table(const sql::DropTableStatement& statement,
                                 StatementResult& result) {
    load_catalog(statement.table);  // unknown table: named error from the loader
    const std::string data_file = data_path(statement.table);
    // Close the session file first (erase_file refuses while open) —
    // the session indexes before the heap, so their files are complete
    // on disk before anything is removed.
    const auto found = files_.find(data_file);
    if (found != files_.end()) {
        close_session_indexes(found->second);
        found->second.file->close();
        files_.erase(found);
    }
    // Erase the data file and every declared index file. Missing files
    // are not errors — a table with no inserts has no data file.
    std::vector<schema::TableIndex> indexes =
        catalogs_.at(statement.table).indexes;
    if (std::remove(data_file.c_str()) != 0 && errno != ENOENT) {
        throw common::Error("cannot erase data file '" + data_file + "'");
    }
    for (const schema::TableIndex& declared : indexes) {
        const std::string idx = index_path(data_file, declared.column,
                                           index_kind_of(declared.kind));
        if (std::remove(idx.c_str()) != 0 && errno != ENOENT) {
            throw common::Error("cannot erase index file '" + idx + "'");
        }
    }
    // Remove the catalog file itself, then the cache entry.
    const std::string catalog_path =
        catalog_directory_ + "/" + statement.table + ".json";
    if (std::remove(catalog_path.c_str()) != 0 && errno != ENOENT) {
        throw common::Error("cannot erase catalog '" + catalog_path + "'");
    }
    catalogs_.erase(statement.table);
    result.message = "table '" + statement.table + "' dropped";
}

void Executor::handle_create_index(
    const sql::CreateIndexStatement& statement, StatementResult& result) {
    load_catalog(statement.table);
    CatalogEntry& entry = catalogs_.at(statement.table);
    const std::optional<std::size_t> column =
        entry.schema.column_index(statement.column);
    if (!column) {
        throw common::Error("unknown column '" + statement.column +
                            "' in table '" + statement.table + "'");
    }
    const sql::IndexKind kind = statement.using_kind;
    for (const schema::TableIndex& declared : entry.indexes) {
        if (declared.column == statement.column &&
            index_kind_of(declared.kind) == kind) {
            throw common::Error("column '" + statement.column +
                                "' already carries a " + kind_label(kind) +
                                " index (one index per column and kind)");
        }
        if (declared.name == statement.index_name) {
            throw common::Error("index '" + statement.index_name +
                                "' already exists");
        }
    }
    // Record first (catalog write is all-or-nothing), then build.
    schema::TableIndex declared;
    declared.name = statement.index_name;
    declared.column = statement.column;
    declared.kind = kind == sql::IndexKind::Hash     ? "hash"
                    : kind == sql::IndexKind::Isam   ? "isam"
                                                     : "static hash";
    entry.indexes.push_back(declared);
    try {
        save_catalog(statement.table);
    } catch (...) {
        entry.indexes.pop_back();
        throw;
    }
    // If the data file is already open in this session, open the index
    // into it now (otherwise file_for opens it lazily on next use).
    const std::string data_file = data_path(statement.table);
    const auto found = files_.find(data_file);
    if (found != files_.end()) {
        SessionIndex session_index;
        session_index.column_index = *column;
        session_index.index_name = statement.index_name;
        session_index.column_name = statement.column;
        session_index.kind = kind;
        if (kind == sql::IndexKind::Hash) {
            session_index.hash =
                std::make_unique<hashindex::ExtensibleHashIndex>(
                    manager_, entry.schema, *column);
        } else if (kind == sql::IndexKind::Static) {
            session_index.static_hash =
                std::make_unique<hashindex::StaticHashIndex>(
                    manager_, entry.schema, *column);
        } else {
            session_index.index =
                std::make_unique<isam::IsamIndex>(manager_, entry.schema,
                                                  *column);
        }
        const std::string idx_path =
            index_path(data_file, statement.column, kind);
        std::vector<std::pair<tuple::Column, isam::Rid>> entries;
        // A scan failure and a build failure both roll the declaration
        // back; the scan's message outranks the build's, and the
        // rollback save's outranks the build's — the old precedence.
        std::string scan_message;
        try {
            heapfile::HeapFileIterator it = found->second.file->scan();
            tuple::Tuple tuple;
            while (it.next(tuple)) {
                entries.emplace_back(tuple.column(*column),
                                     isam::Rid{it.row_id().page_id,
                                               it.row_id().slot});
            }
        } catch (const common::Error& e) {
            scan_message = e.what();
        }
        std::string build_message;
        try {
            switch (kind) {
                case sql::IndexKind::Hash:
                    session_index.hash->build(idx_path, entries);
                    break;
                case sql::IndexKind::Static:
                    session_index.static_hash->build(
                        idx_path, kDefaultStaticBuckets, entries);
                    break;
                case sql::IndexKind::Isam:
                    session_index.index->build(idx_path, entries);
                    break;
            }
        } catch (const common::Error& e) {
            build_message = e.what();
        }
        if (!scan_message.empty() || !build_message.empty()) {
            entry.indexes.pop_back();
            try {
                save_catalog(statement.table);
            } catch (const common::Error& e) {
                build_message = e.what();
            }
            throw common::Error(scan_message.empty() ? build_message
                                                     : scan_message);
        }
        found->second.indexes.push_back(std::move(session_index));
    }
    result.message = "index '" + statement.index_name + "' created on '" +
                     statement.table + "(" + statement.column + ")'";
}

void Executor::handle_drop_index(const sql::DropIndexStatement& statement,
                                 StatementResult& result) {
    // The index name is unique per table; search every catalog in the
    // directory (loading the not-yet-loaded ones) so a DROP works even
    // in a session that never mentioned the table before.
    for (auto& [name, entry] : catalogs_) {
        for (std::size_t i = 0; i < entry.indexes.size(); ++i) {
            if (entry.indexes[i].name == statement.index_name) {
                drop_index_in(name, entry, i, result);
                return;
            }
        }
    }
    // Load every catalog file in the directory and search again.
    std::vector<std::string> names;
    if (DIR* dir = opendir(catalog_directory_.c_str())) {
        while (const dirent* item = readdir(dir)) {
            const std::string file_name = item->d_name;
            const std::string suffix = ".json";
            if (file_name.size() > suffix.size() &&
                file_name.compare(file_name.size() - suffix.size(),
                                  suffix.size(), suffix) == 0) {
                names.push_back(
                    file_name.substr(0, file_name.size() - suffix.size()));
            }
        }
        closedir(dir);
    }
    for (const std::string& name : names) {
        try {
            load_catalog(name);
        } catch (const common::Error&) {
            continue;  // an unreadable catalog is not a DROP error
        }
        CatalogEntry& entry = catalogs_.at(name);
        for (std::size_t i = 0; i < entry.indexes.size(); ++i) {
            if (entry.indexes[i].name == statement.index_name) {
                drop_index_in(name, entry, i, result);
                return;
            }
        }
    }
    throw common::Error("unknown index '" + statement.index_name + "'");
}

void Executor::drop_index_in(const std::string& name, CatalogEntry& entry,
                             std::size_t position, StatementResult& result) {
    const std::string index_name = entry.indexes[position].name;
    const std::string data_file = data_path(name);
    const std::string idx = index_path(
        data_file, entry.indexes[position].column,
        index_kind_of(entry.indexes[position].kind));
    // Close the session file so the index file is flushed, unpinned,
    // and closed before erasure — the session indexes first.
    const auto found = files_.find(data_file);
    if (found != files_.end()) {
        close_session_indexes(found->second);
        found->second.file->close();
        files_.erase(found);
    }
    if (std::remove(idx.c_str()) != 0 && errno != ENOENT) {
        throw common::Error("cannot erase index file '" + idx + "'");
    }
    entry.indexes.erase(entry.indexes.begin() +
                        static_cast<long>(position));
    save_catalog(name);
    result.message = "index '" + index_name + "' dropped";
}

// --- INSERT ------------------------------------------------------------------

void Executor::handle_insert(const sql::InsertStatement& statement,
                             StatementResult& result) {
    load_catalog(statement.table);
    const CatalogEntry& entry = catalogs_.at(statement.table);
    const schema::TableSchema& schema = entry.schema;

    // Validate every row before any page is pinned: an INSERT is
    // all-or-nothing per statement.
    std::vector<tuple::Tuple> tuples;
    for (std::size_t r = 0; r < statement.rows.size(); ++r) {
        const std::vector<sql::ExprPtr>& row = statement.rows[r];
        if (row.size() != schema.column_count()) {
            throw common::Error(
                "row " + std::to_string(r + 1) + " has " +
                std::to_string(row.size()) + " value(s), table '" +
                statement.table + "' has " +
                std::to_string(schema.column_count()) + " columns");
        }
        tuple::Tuple tuple;
        for (std::size_t c = 0; c < row.size(); ++c) {
            const sql::Expr& cell = *row[c];
            if (cell.kind != sql::Expr::Kind::Literal) {
                throw common::Error(
                    "row " + std::to_string(r + 1) + ", column '" +
                    schema.column_info(c).column_name() +
                    "': only literal values are supported");
            }
            const sql::LiteralValue& literal = cell.value;
            const schema::ColumnSchema& meta = schema.column_info(c);
            switch (meta.column_type()) {
                case tuple::ColumnType::Integer:
                    if (literal.tag != sql::LiteralValue::Tag::Int) {
                        throw common::Error(
                            "row " + std::to_string(r + 1) + ", column '" +
                            meta.column_name() +
                            "': value must be an integer");
                    }
                    tuple.add(static_cast<int>(literal.i));
                    break;
                case tuple::ColumnType::Double:
                    if (literal.tag == sql::LiteralValue::Tag::Double) {
                        tuple.add(literal.d);
                    } else if (literal.tag == sql::LiteralValue::Tag::Int) {
                        tuple.add(static_cast<double>(literal.i));  // widening
                    } else {
                        throw common::Error(
                            "row " + std::to_string(r + 1) + ", column '" +
                            meta.column_name() + "': value must be a number");
                    }
                    break;
                case tuple::ColumnType::String:
                    if (literal.tag != sql::LiteralValue::Tag::String) {
                        throw common::Error(
                            "row " + std::to_string(r + 1) + ", column '" +
                            meta.column_name() + "': value must be a string");
                    }
                    tuple.add(literal.s);
                    break;
            }
        }
        schema.validate(tuple);  // width/NUL rules surface here, named
        tuples.push_back(std::move(tuple));
    }

    heapfile::HeapFile* file =
        file_for(statement.table, data_path(statement.table), schema, true);
    // Every insert lands in the heap first, then in each index of the
    // file (the mutation order the proposal pins).
    for (const tuple::Tuple& tuple : tuples) {
        heapfile::RowId row_id;
        file->insert(tuple, row_id);
        // Maintain this file's indexes; a failure surfaces by name.
        const auto found = files_.find(data_path(statement.table));
        for (const SessionIndex& session_index : found->second.indexes) {
            index_insert(session_index,
                         tuple.column(session_index.column_index),
                         isam::Rid{row_id.page_id, row_id.slot});
        }
    }
    result.message = "inserted " + std::to_string(tuples.size()) + " row(s)";
}

// --- SELECT ------------------------------------------------------------------

// The grouped branch: GROUP BY keys and every group-context expression
// (select items, HAVING) bind here; execution runs through the group
// module's static hash operator.
void Executor::handle_grouped_select(const sql::SelectStatement& statement,
                                     StatementResult& result) {
    load_catalog(statement.from.name);
    const CatalogEntry& entry = catalogs_.at(statement.from.name);
    const schema::TableSchema& schema = entry.schema;

    // The group context: keys as schema positions in GROUP BY order.
    exec::GroupBinding group;
    for (const sql::ExprPtr& key : statement.group_by) {
        exec::BoundExprPtr bound_key = exec::bind_expr(*key, schema);
        if (bound_key->kind != exec::BoundExpr::Kind::Column) {
            throw common::Error("GROUP BY must name plain columns");
        }
        group.key_columns.push_back(bound_key->column_index);
    }

    // Projection: star is refused under grouping (name the columns);
    // every item binds in group context, so aggregates and keys share
    // one inventory.
    std::vector<exec::BoundExprPtr> items;
    result.rows.columns.clear();
    for (const sql::SelectItem& item : statement.items) {
        if (item.star) {
            throw common::Error(
                "SELECT * with GROUP BY is not supported (name the "
                "grouped columns)");
        }
        exec::BoundExprPtr bound_item =
            exec::bind_group_expr(*item.expr, schema, group);
        result.rows.columns.push_back(
            item.alias.empty() ? sql::print_expr(*item.expr) : item.alias);
        items.push_back(std::move(bound_item));
    }

    exec::BoundExprPtr having;
    if (statement.having) {
        having = exec::bind_group_expr(*statement.having, schema, group);
        if (having->type != exec::Type::Bool) {
            throw common::Error("HAVING must be a boolean predicate");
        }
    }

    // Bind the WHERE: rows are filtered before they reach the groups.
    BoundExprPtr where;
    if (statement.where) {
        where = bind_expr(*statement.where, schema);
    }

    // The shared scan spine: same access paths, same oracle gates.
    ScanPlan plan;
    std::string scan_path;
    heapfile::HeapFile* file = nullptr;
    std::vector<heapfile::RowId> candidates;
    collect_candidates(statement, entry, std::move(where), plan, scan_path,
                       file, candidates);

    // The operator: the compiled aggregate shapes. The table sizes with
    // the group count, not the row count (design D5).
    std::vector<group::AggregateSpec> specs;
    specs.reserve(group.aggregates.size());
    for (const BoundAggregate& bound : group.aggregates) {
        group::AggregateSpec spec;
        spec.fn = bound.fn;
        spec.result_type = bound.type;
        spec.arg_type = bound.arg == nullptr ? Type::Int : bound.arg->type;
        spec.takes_arg = bound.arg != nullptr;
        specs.push_back(spec);
    }

    const auto column_to_value = [](const tuple::Column& column) {
        switch (column.column_type()) {
            case tuple::ColumnType::Integer:
                return Value(std::get<int>(column.column_value()));
            case tuple::ColumnType::Double:
                return Value(std::get<double>(column.column_value()));
            case tuple::ColumnType::String:
                return Value(std::get<std::string>(column.column_value()));
        }
        return Value(0);
    };

    // Fetch, residual-filter, and build the record stream: one record
    // per surviving row, carrying the group-key values then one value
    // per aggregate slot that takes an argument (the run-record shape
    // of design D7, reused as the in-memory attempt's fold input).
    std::vector<std::vector<Value>> records;
    if (file != nullptr) {
        for (const heapfile::RowId& row_id : candidates) {
            tuple::Tuple tuple;
            file->find(row_id, tuple);
            bool keep = true;
            for (const BoundExprPtr& conjunct : plan.residual) {
                Value value = evaluate(*conjunct, tuple);
                if (const int* b = std::get_if<int>(&value);
                    b == nullptr || *b == 0) {
                    keep = false;
                    break;
                }
            }
            if (!keep) {
                continue;
            }
            std::vector<Value> record;
            record.reserve(group.key_columns.size() + specs.size());
            for (std::size_t column_index : group.key_columns) {
                record.push_back(column_to_value(tuple.column(column_index)));
            }
            for (std::size_t i = 0; i < group.aggregates.size(); ++i) {
                if (group.aggregates[i].arg == nullptr) {
                    continue;  // COUNT(*): no argument column
                }
                record.push_back(evaluate(*group.aggregates[i].arg, tuple));
            }
            records.push_back(std::move(record));
        }
    }

    bool spilled = false;
    aggregate_records(records, schema, specs, group.key_columns, items,
                      having, statement.from.name, /*level=*/0, spilled,
                      result.rows.rows);

    // The announcement composes the scan path with the grouping.
    result.rows.access_path =
        spilled
            ? scan_path + " + hash group (spilled, " +
                  std::to_string(group_partitions_) + " partitions)"
            : scan_path + " + hash group";
}

// --- the grouped aggregation -------------------------------------------------

// Drains finished groups: HAVING filter, then one group-context
// evaluation per select item (design D8). Appends to out_rows.
void drain_groups(std::vector<group::GroupEntry> groups,
                  const std::vector<std::size_t>& key_columns,
                  const std::vector<group::AggregateSpec>& specs,
                  const std::vector<BoundExprPtr>& items,
                  const BoundExprPtr& having,
                  std::vector<std::vector<Value>>& out_rows) {
    for (const group::GroupEntry& finished : groups) {
        std::vector<Value> aggregate_values;
        aggregate_values.reserve(specs.size());
        for (std::size_t i = 0; i < specs.size(); ++i) {
            aggregate_values.push_back(
                group::final_value(specs[i], finished.states[i]));
        }
        if (having) {
            Value keep = evaluate_group(*having, key_columns, finished.key,
                                        aggregate_values);
            if (const int* b = std::get_if<int>(&keep);
                b == nullptr || *b == 0) {
                continue;
            }
        }
        std::vector<Value> row;
        row.reserve(items.size());
        for (const BoundExprPtr& item : items) {
            row.push_back(evaluate_group(*item, key_columns, finished.key,
                                         aggregate_values));
        }
        out_rows.push_back(std::move(row));
    }
}

void Executor::aggregate_records(
    const std::vector<std::vector<Value>>& records,
    const schema::TableSchema& schema,
    const std::vector<group::AggregateSpec>& specs,
    const std::vector<std::size_t>& key_columns,
    const std::vector<BoundExprPtr>& items, const BoundExprPtr& having,
    const std::string& table, std::size_t level, bool& spilled, std::vector<std::vector<Value>>& out_rows) {
    // The common case: fold everything into one in-memory table.
    group::GroupTable table_agg(specs, group_buckets_, group_budget_);
    for (const std::vector<Value>& record : records) {
        std::vector<Value> key(record.begin(),
                               record.begin() + key_columns.size());
        std::vector<Value> args(specs.size());  // COUNT(*) slots stay 0
        std::size_t cursor = key_columns.size();
        for (std::size_t i = 0; i < specs.size(); ++i) {
            if (specs[i].takes_arg) {
                args[i] = record[cursor++];
            }
        }
        // false = the entry budget is exhausted: the restart-into-spill
        // signal, not an error.
        if (!table_agg.fold(key, args)) {
            // Restart on overflow (design D6): the partial table dies
            // with this frame; the record stream restarts into runs.
            return spill_records(records, schema, specs, key_columns, items,
                                 having, table, level, spilled, out_rows);
        }
    }
    // Empty input with no GROUP BY: one identity row (design D8).
    if (records.empty() && key_columns.empty()) {
        std::vector<group::GroupEntry> identity(1);
        identity[0].states.resize(specs.size());
        drain_groups(std::move(identity), key_columns, specs, items, having,
                     out_rows);
        return;
    }
    drain_groups(table_agg.take_all(), key_columns, specs, items, having,
                 out_rows);
}

void Executor::spill_records(
    const std::vector<std::vector<Value>>& records,
    const schema::TableSchema& schema,
    const std::vector<group::AggregateSpec>& specs,
    const std::vector<std::size_t>& key_columns,
    const std::vector<BoundExprPtr>& items, const BoundExprPtr& having,
    const std::string& table, std::size_t level, bool& spilled, std::vector<std::vector<Value>>& out_rows) {
    const std::size_t partitions = group_partitions_;
    if (partitions < 2 || (partitions & (partitions - 1)) != 0) {
        throw common::Error("the group partition count must be a power of two");
    }
    std::size_t bits = 0;
    for (std::size_t k = partitions; (k >>= 1) > 0;) {
        ++bits;
    }
    if (level * bits >= kGroupHashBits) {
        // Every hash bit has been sliced: distinct keys must have
        // separated by now (design D6's termination argument), and the
        // level shift below would exceed the hash width. Reaching this
        // means something is wrong, so name it.
        throw common::Error("grouped execution exceeded the spill depth cap");
    }
    const auto run_number = [&](std::size_t p) {
        return level * partitions + p;  // level and partition in one number
    };
    const auto cleanup_runs = [&]() {
        for (std::size_t p = 0; p < partitions; ++p) {
            std::remove(group_run_path(table, run_number(p)).c_str());
        }
    };

    // The run mini-schema: key types then the taking-argument slot
    // types. String width: the widest string column of the source table
    // — the only way a spill record carries a string is a whole column
    // value (there are no string-producing expressions).
    std::vector<exec::Type> run_types;
    run_types.reserve(key_columns.size() + specs.size());
    for (std::size_t column_index : key_columns) {
        const tuple::ColumnType physical =
            schema.column_info(column_index).column_type();
        run_types.push_back(physical == tuple::ColumnType::Double
                                ? exec::Type::Double
                                : physical == tuple::ColumnType::String
                                      ? exec::Type::String
                                      : exec::Type::Int);
    }
    std::size_t string_width = 1;
    for (std::size_t i = 0; i < schema.column_count(); ++i) {
        if (schema.column_info(i).column_type() == tuple::ColumnType::String) {
            string_width =
                std::max(string_width, schema.column_info(i).column_size());
        }
    }
    for (const group::AggregateSpec& spec : specs) {
        if (spec.takes_arg) {
            run_types.push_back(spec.arg_type);
        }
    }
    const schema::TableSchema mini =
        group::run_schema(table, run_types, string_width);

    // Write the level's runs, routed by the level-shifted hash bits:
    // level L partitions by bits [L·log2K, (L+1)·log2K) — re-partitioning
    // with the same modulus would send every key back where it came from.
    std::vector<std::optional<group::PartitionRunWriter>> writers(partitions);
    std::vector<bool> has_rows(partitions, false);
    for (const std::vector<Value>& record : records) {
        const std::vector<Value> key(record.begin(),
                                     record.begin() + key_columns.size());
        const std::uint32_t hash = group::hash_group_key(key);
        const std::size_t partition =
            (hash >> (level * bits)) & (partitions - 1);
        if (!writers[partition].has_value()) {
            writers[partition].emplace();
            try {
                writers[partition]->open(
                    group_run_path(table, run_number(partition)), mini);
            } catch (...) {
                cleanup_runs();
                throw;
            }
        }
        try {
            writers[partition]->append(record);
        } catch (...) {
            cleanup_runs();
            throw;
        }
        has_rows[partition] = true;
    }
    for (std::optional<group::PartitionRunWriter>& writer : writers) {
        if (writer.has_value()) {
            try {
                writer->close();
            } catch (...) {
                cleanup_runs();
                throw;
            }
        }
    }
    spilled = true;

    // Re-aggregate each partition with the same grouping logic (memory
    // first, spill deeper on overflow), deleting each run as it is
    // consumed — best-effort deletion on the named error paths too.
    for (std::size_t p = 0; p < partitions; ++p) {
        if (!has_rows[p]) {
            continue;  // no rows routed here: no file was written
        }
        const std::string path = group_run_path(table, run_number(p));
        group::PartitionRunReader reader;
        try {
            reader.open(path, mini);
        } catch (...) {
            cleanup_runs();
            throw;
        }
        std::vector<std::vector<Value>> partition_records;
        std::vector<Value> record;
        try {
            while (reader.next(record)) {
                partition_records.push_back(record);
            }
        } catch (...) {
            std::remove(path.c_str());
            cleanup_runs();
            throw;
        }
        std::remove(path.c_str());  // consumed: deleted at drain (D7)
        try {
            aggregate_records(partition_records, schema, specs, key_columns,
                              items, having, table, level + 1, spilled,
                              out_rows);
        } catch (...) {
            cleanup_runs();
            throw;
        }
    }
}

std::string Executor::group_run_path(const std::string& table,
                                     std::size_t r) const {
    return data_path(table) + ".gspill" + std::to_string(r) + ".tmp";
}

// The scan spine shared by both SELECT paths: access-path selection with
// the oracle's kind gates, then candidate row-id collection. In oracle
// mode the planner sees no indexes, so every conjunct stays residual and
// the heap path runs — the suppression must gate the planner, not patch
// its output; the kind gates hide one kind at a time (the oracle's
// per-path runs).
void Executor::collect_candidates(const sql::SelectStatement& statement,
                                  const CatalogEntry& entry,
                                  BoundExprPtr where, ScanPlan& plan,
                                  std::string& access_path,
                                  heapfile::HeapFile*& file,
                                  std::vector<heapfile::RowId>& candidates) {
    const schema::TableSchema& schema = entry.schema;
    std::vector<CatalogIndex> planner_indexes;
    if (allow_index_) {
        for (const schema::TableIndex& declared : entry.indexes) {
            const sql::IndexKind kind = index_kind_of(declared.kind);
            if (kind == sql::IndexKind::Hash && !allow_hash_) {
                continue;
            }
            if (kind == sql::IndexKind::Isam && !allow_isam_) {
                continue;
            }
            if (kind == sql::IndexKind::Static && !allow_static_) {
                continue;
            }
            const std::optional<std::size_t> column =
                schema.column_index(declared.column);
            if (column) {
                planner_indexes.push_back(
                    CatalogIndex{declared.name, *column, kind});
            }
        }
    }
    plan = plan_where(std::move(where), planner_indexes);

    access_path =
        plan.use_hash     ? "hash scan"
        : plan.use_static ? "static scan"
        : plan.use_index  ? "index scan"
                          : "heap scan";

    // The session file, opened on first use. A data file that cannot be
    // opened here (missing, or an index that would not rebuild) leaves
    // no rows to answer from — the open error is swallowed, as before.
    const std::string data_file = data_path(statement.from.name);
    try {
        file = file_for(statement.from.name, data_file, schema, false);
    } catch (...) {
        file = nullptr;
    }
    if (file == nullptr) {
        return;
    }
    const auto found = files_.find(data_file);

    const auto to_column = [](const Value& value) {
        if (const int* v = std::get_if<int>(&value)) {
            return tuple::Column(*v);
        }
        if (const double* v = std::get_if<double>(&value)) {
            return tuple::Column(*v);
        }
        return tuple::Column(std::get<std::string>(value));
    };

    // Collect the candidate rows: the hash, static, and index scans
    // yield row ids; the heap path scans every record.
    if (plan.use_hash) {
        hashindex::ExtensibleHashIndex* index = nullptr;
        for (const SessionIndex& session_index : found->second.indexes) {
            if (session_index.kind == sql::IndexKind::Hash &&
                session_index.column_index == plan.bounds.column_index) {
                index = session_index.hash.get();
                break;
            }
        }
        if (index == nullptr) {
            plan.use_hash = false;  // tolerate and scan
            access_path = "heap scan";
        } else {
            std::vector<isam::Rid> rids;
            index->lookup(to_column(*plan.bounds.lo), rids);
            candidates.reserve(rids.size());
            for (const isam::Rid& rid : rids) {
                candidates.push_back(
                    heapfile::RowId{rid.page_id, rid.slot});
            }
        }
    } else if (plan.use_static) {
        hashindex::StaticHashIndex* index = nullptr;
        for (const SessionIndex& session_index : found->second.indexes) {
            if (session_index.kind == sql::IndexKind::Static &&
                session_index.column_index == plan.bounds.column_index) {
                index = session_index.static_hash.get();
                break;
            }
        }
        if (index == nullptr) {
            plan.use_static = false;  // tolerate and scan
            access_path = "heap scan";
        } else {
            std::vector<isam::Rid> rids;
            index->lookup(to_column(*plan.bounds.lo), rids);
            candidates.reserve(rids.size());
            for (const isam::Rid& rid : rids) {
                candidates.push_back(
                    heapfile::RowId{rid.page_id, rid.slot});
            }
        }
    } else if (plan.use_index) {
        isam::IsamIndex* index = nullptr;
        for (const SessionIndex& session_index : found->second.indexes) {
            if (session_index.kind == sql::IndexKind::Isam &&
                session_index.column_index == plan.bounds.column_index) {
                index = session_index.index.get();
                break;
            }
        }
        if (index == nullptr) {
            plan.use_index = false;  // tolerate and scan
            access_path = "heap scan";
        } else if (!plan.bounds.possible()) {
            return;  // conflicting bounds: the index answers nothing
        } else {
            std::optional<isam::RangeBound> lo;
            std::optional<isam::RangeBound> hi;
            if (plan.bounds.lo) {
                lo = isam::RangeBound{to_column(*plan.bounds.lo),
                                      plan.bounds.lo_inclusive};
            }
            if (plan.bounds.hi) {
                hi = isam::RangeBound{to_column(*plan.bounds.hi),
                                      plan.bounds.hi_inclusive};
            }
            std::vector<isam::Rid> rids;
            index->range(lo, hi, rids);
            candidates.reserve(rids.size());
            for (const isam::Rid& rid : rids) {
                candidates.push_back(
                    heapfile::RowId{rid.page_id, rid.slot});
            }
        }
    }

    if (!plan.use_hash && !plan.use_static && !plan.use_index) {
        heapfile::HeapFileIterator it = file->scan();
        tuple::Tuple tuple;
        while (it.next(tuple)) {
            candidates.push_back(it.row_id());
        }
    }
}

void Executor::handle_select(const sql::SelectStatement& statement,
                             StatementResult& result) {
    // The surface refusals: the grammar accepts these, the executor
    // does not — each names its feature. GROUP BY, HAVING, and aggregate
    // items are not refusals since ex15: they route to the grouped
    // branch.
    if (!statement.joins.empty()) {
        throw common::Error("joins are not supported in ex17");
    }
    if (!statement.order_by.empty()) {
        throw common::Error("ORDER BY is not supported in ex17");
    }
    if (statement.has_limit) {
        throw common::Error("LIMIT is not supported in ex17");
    }

    // Grouped-ness: an explicit GROUP BY, a HAVING, or any aggregate
    // select item (the implicit single group).
    bool grouped = !statement.group_by.empty() || statement.having != nullptr;
    for (const sql::SelectItem& item : statement.items) {
        if (!item.star && item.expr->kind == sql::Expr::Kind::Function) {
            grouped = true;
            break;
        }
    }
    if (grouped) {
        handle_grouped_select(statement, result);
        return;
    }

    load_catalog(statement.from.name);
    const CatalogEntry& entry = catalogs_.at(statement.from.name);
    const schema::TableSchema& schema = entry.schema;

    // Projection: star or plain columns (expressions in the projection
    // are outside the surface).
    std::vector<std::size_t> projected;
    result.rows.columns.clear();
    for (const sql::SelectItem& item : statement.items) {
        if (item.star) {
            for (std::size_t i = 0; i < schema.column_count(); ++i) {
                projected.push_back(i);
                result.rows.columns.push_back(
                    schema.column_info(i).column_name());
            }
            continue;
        }
        if (item.expr->kind != sql::Expr::Kind::ColumnRef) {
            if (item.expr->kind == sql::Expr::Kind::Function) {
                throw common::Error(
                    "aggregate functions are not supported in ex17");
            }
            throw common::Error(
                "expressions in the projection are not supported in "
                "ex17 (plain columns only)");
        }
        const sql::Expr& column_expr = *item.expr;
        if (!column_expr.table.empty()) {
            throw common::Error(
                "table-qualified columns need a join; single-table "
                "queries use bare names");
        }
        const std::optional<std::size_t> index =
            schema.column_index(column_expr.column);
        if (!index) {
            throw common::Error("unknown column '" + column_expr.column +
                                "' in table '" + statement.from.name + "'");
        }
        projected.push_back(*index);
        result.rows.columns.push_back(
            item.alias.empty() ? column_expr.column : item.alias);
    }

    // Bind the WHERE (bind-time type errors happen before any page pin).
    BoundExprPtr where;
    if (statement.where) {
        where = bind_expr(*statement.where, schema);
    }

    // The shared scan spine: plan the access path, collect candidates.
    ScanPlan plan;
    std::string scan_path;
    heapfile::HeapFile* file = nullptr;
    std::vector<heapfile::RowId> candidates;
    collect_candidates(statement, entry, std::move(where), plan, scan_path,
                       file, candidates);
    result.rows.access_path = scan_path;
    if (file == nullptr) {
        return;  // missing data file: no rows can exist
    }

    // Fetch, residual-filter, project. Candidates are fetched one at a
    // time; nothing is pinned across rows.
    for (const heapfile::RowId& row_id : candidates) {
        tuple::Tuple tuple;
        file->find(row_id, tuple);
        bool keep = true;
        for (const BoundExprPtr& conjunct : plan.residual) {
            Value value = evaluate(*conjunct, tuple);
            if (const int* b = std::get_if<int>(&value); b == nullptr || *b == 0) {
                keep = false;
                break;
            }
        }
        if (!keep) {
            continue;
        }
        std::vector<Value> row;
        row.reserve(projected.size());
        for (std::size_t column_index : projected) {
            const tuple::Column& column = tuple.column(column_index);
            switch (column.column_type()) {
                case tuple::ColumnType::Integer:
                    row.push_back(
                        Value(std::get<int>(column.column_value())));
                    break;
                case tuple::ColumnType::Double:
                    row.push_back(
                        Value(std::get<double>(column.column_value())));
                    break;
                case tuple::ColumnType::String:
                    row.push_back(
                        Value(std::get<std::string>(column.column_value())));
                    break;
            }
        }
        result.rows.rows.push_back(std::move(row));
    }
}

}
