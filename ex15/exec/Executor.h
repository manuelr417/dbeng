#pragma once

// Executor — the statement dispatcher and the session state it runs
// against: one buffer manager (shared with every heap file and index
// file), the catalog cache, and the file table. The statement surface
// is the proposal's contract; everything the grammar accepts but the
// surface excludes (joins, ORDER BY, LIMIT, UPDATE, DELETE) is refused
// here with a named error — the subset contract lives in the dispatcher,
// not the grammar. Grouped SELECTs (GROUP BY, HAVING, aggregate items)
// route to the group module's static hash operator.
//
// Physical naming: a table's data file is `data/<table>.tbl` (the
// `data` directory is created on demand); an index file is
// `data/<table>.tbl.<column>.idx` (ISAM), `.hdx` (hash), or `.shdx`
// (static hash) beside it.
// Opening a data file builds any catalog-declared index file that is
// missing (lazy rebuild), and every write through a session file
// maintains that file's indexes.

#include "bufpool/BufferManager.h"
#include "exec/Planner.h"
#include "exec/Value.h"
#include "group/GroupBy.h"
#include "hashindex/ExtensibleHashIndex.h"
#include "hashindex/StaticHashIndex.h"
#include "heapfile/HeapFile.h"
#include "index/IsamIndex.h"
#include "schema/TableSchema.h"
#include "schema/TableSchemaJson.h"
#include "sql/Ast.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace exec {

struct ResultSet {
    std::vector<std::string> columns;             // projected names
    std::vector<std::vector<Value>> rows;
    std::string access_path;                      // "index scan"/"heap scan"
};

struct StatementResult {
    std::string message;     // one-line report (DDL, INSERT)
    ResultSet rows;          // filled for SELECT
};

class Executor {
public:
    Executor(bufman::BufferManager& manager,
             const std::string& catalog_directory = "tablecatalog");

    // Dispatches one parsed statement. SELECT fills `result.rows`; other
    // statements fill `result.message`. Refusals and failures are named
    // errors; the session continues.
    bool execute(const sql::Statement& statement, StatementResult& result,
                 std::string& error);

    // The differential-oracle hooks: when false, the planner's index
    // paths are suppressed and every SELECT scans the heap. The two
    // kind gates let the oracle run one kind at a time (a kind-suppressed
    // index is invisible to the planner, so its conjuncts go residual).
    void set_allow_index(bool allow) { allow_index_ = allow; }
    void set_allow_hash(bool allow) { allow_hash_ = allow; }
    void set_allow_isam(bool allow) { allow_isam_ = allow; }
    void set_allow_static(bool allow) { allow_static_ = allow; }

    // The group operator's settings (design D8): bucket count, partition
    // count, and entry budget, with production defaults. Injectable for
    // the oracle's parameter-invariance runs and the spill tests — never
    // SQL surface.
    void set_group_buckets(std::size_t buckets) { group_buckets_ = buckets; }
    void set_group_partitions(std::size_t partitions) {
        group_partitions_ = partitions;
    }
    void set_group_budget(std::size_t budget) { group_budget_ = budget; }

    // The table's schema and catalog indexes (loading the catalog on
    // first use). Null with a named error for an unknown table.
    const schema::TableSchema* table(const std::string& name,
                                     std::vector<schema::TableIndex>& indexes,
                                     std::string& error);

    // The session file for `path`, bound to `table_name`: opened on
    // first use, its catalog indexes opened or lazily built with it.
    // The escape-hatch commands and the SQL path share this.
    heapfile::HeapFile* file_for(const std::string& table_name,
                                 const std::string& path,
                                 const schema::TableSchema& schema,
                                 bool create_when_missing, std::string& error);

    // quit/EOF: close every session file exactly once. False iff any
    // close (flush) failed.
    bool close_all(std::string& error);

    // The escape-hatch write path: inserts one tuple through the session
    // file, maintaining that file's indexes (heap first, then index).
    bool insert_into(const std::string& table_name, const std::string& path,
                     const tuple::Tuple& tuple, heapfile::RowId& row_id,
                     std::string& error);

    // The escape-hatch erase path: reads the row (recording the indexed
    // columns' values), erases the heap slot, then erases every index
    // entry whose key and row id match the erased record. Heap first,
    // then every index — the mutation order INSERT uses.
    bool erase_row(const std::string& table_name, const std::string& path,
                   heapfile::RowId row_id, std::string& error);

    // Counts the records of a session file by scanning it. With
    // create_when_missing, a missing data file counts as zero rows (and
    // is created) instead of failing — the bulk path's fresh-table case.
    bool count_rows(const std::string& table_name, const std::string& path,
                    std::size_t& count, std::string& error,
                    bool create_when_missing = false);

    // checkindex: verifies every catalog-declared index of `path`
    // against the data file; one report line per index, clean or named.
    bool check_indexes(const std::string& table_name, const std::string& path,
                       std::vector<std::string>& report, bool& clean,
                       std::string& error);

private:
    struct SessionIndex {
        std::size_t column_index = 0;
        std::string index_name;   // catalog name
        std::string column_name;
        sql::IndexKind kind = sql::IndexKind::Hash;
        std::unique_ptr<isam::IsamIndex> index;                // ISAM kind
        std::unique_ptr<hashindex::ExtensibleHashIndex> hash;  // hash kind
        std::unique_ptr<hashindex::StaticHashIndex> static_hash;
    };

    struct SessionFile {
        std::string table_name;
        std::unique_ptr<heapfile::HeapFile> file;
        std::vector<SessionIndex> indexes;
    };

    struct CatalogEntry {
        CatalogEntry() : schema("") {}
        schema::TableSchema schema;
        std::vector<schema::TableIndex> indexes;
    };

    bool handle_create_table(const sql::CreateTableStatement& statement,
                             StatementResult& result, std::string& error);
    bool handle_drop_table(const sql::DropTableStatement& statement,
                           StatementResult& result, std::string& error);
    bool handle_create_index(const sql::CreateIndexStatement& statement,
                             StatementResult& result, std::string& error);
    bool handle_drop_index(const sql::DropIndexStatement& statement,
                           StatementResult& result, std::string& error);
    bool drop_index_in(const std::string& table_name, CatalogEntry& entry,
                       std::size_t position, StatementResult& result,
                       std::string& error);
    bool handle_insert(const sql::InsertStatement& statement,
                       StatementResult& result, std::string& error);
    bool handle_select(const sql::SelectStatement& statement,
                       StatementResult& result, std::string& error);

    // The grouped SELECT branch: GROUP BY keys, aggregate select items,
    // and HAVING bind here (group context); execution runs through the
    // group module's static hash operator.
    bool handle_grouped_select(const sql::SelectStatement& statement,
                               StatementResult& result, std::string& error);

    // The scan spine shared by both SELECT paths: access-path selection
    // (with the oracle's kind gates) and candidate row-id collection.
    // `file` is null when the data file does not exist yet — no rows can
    // exist, so the caller answers from zero rows.
    bool collect_candidates(const sql::SelectStatement& statement,
                            const CatalogEntry& entry, BoundExprPtr where,
                            ScanPlan& plan, std::string& access_path,
                            heapfile::HeapFile*& file,
                            std::vector<heapfile::RowId>& candidates,
                            std::string& error);

    // The grouped aggregation over pre-evaluated records (key values
    // then one value per aggregate slot that takes an argument). Tries
    // the in-memory table first; on budget exhaustion restarts the
    // record stream into K partition runs one level deeper (design D6).
    // `level` selects the hash-bit slice (and, once the slices exhaust
    // the hash, caps the recursion); `spilled` rises once any level
    // spilled.
    bool aggregate_records(const std::vector<std::vector<Value>>& records,
                           const schema::TableSchema& schema,
                           const std::vector<group::AggregateSpec>& specs,
                           const std::vector<std::size_t>& key_columns,
                           const std::vector<BoundExprPtr>& items,
                           const BoundExprPtr& having, const std::string& table,
                           std::size_t level, bool& spilled,
                           std::vector<std::vector<Value>>& out_rows,
                           std::string& error);

    // The spill half of aggregate_records: writes the level's K runs,
    // re-aggregates each partition recursively, deletes the files.
    bool spill_records(const std::vector<std::vector<Value>>& records,
                       const schema::TableSchema& schema,
                       const std::vector<group::AggregateSpec>& specs,
                       const std::vector<std::size_t>& key_columns,
                       const std::vector<BoundExprPtr>& items,
                       const BoundExprPtr& having, const std::string& table,
                       std::size_t level, bool& spilled,
                       std::vector<std::vector<Value>>& out_rows,
                       std::string& error);

    // A spill run's path: beside the data file, run number r encoding
    // level and partition (design D7).
    std::string group_run_path(const std::string& table, std::size_t r) const;

    bool load_catalog(const std::string& name, std::string& error);
    std::string data_path(const std::string& table_name) const;
    std::string index_path(const std::string& data_file,
                           const std::string& column_name,
                           sql::IndexKind kind) const;
    bool save_catalog(const std::string& name, std::string& error);
    void forget_file(const std::string& path);

    // The one index-maintenance seam: inserts and erases fan out to the
    // session index of whatever kind it is (heap first happens at the
    // call sites).
    bool index_insert(const SessionIndex& session_index,
                      const tuple::Column& value, isam::Rid rid,
                      std::string& error);
    bool index_erase(const SessionIndex& session_index,
                     const tuple::Column& value, isam::Rid rid,
                     std::string& error);

    // The teardown half of the union: closes every session index's file
    // through the manager — flushing its dirty pages — before the data
    // file closes or an index file is removed. Index pages reach disk
    // only on eviction or close, so a session index must be closed for
    // its file to be complete on disk.
    bool close_session_indexes(SessionFile& entry, std::string& error);

    bufman::BufferManager& manager_;
    std::string catalog_directory_;
    std::unordered_map<std::string, CatalogEntry> catalogs_;
    std::unordered_map<std::string, SessionFile> files_;
    bool allow_index_ = true;
    bool allow_hash_ = true;
    bool allow_isam_ = true;
    bool allow_static_ = true;
    std::size_t group_buckets_ = 8;        // design D5: the static index's default
    std::size_t group_partitions_ = 8;     // design D6: the spill's fan-out
    std::size_t group_budget_ = 100000;    // design D6: "large" for teaching scale
};

}
