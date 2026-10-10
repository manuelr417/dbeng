// ex13 — bulk loader checks: the bootstrap tool's contract against the
// HeapFile class's own. The differential oracle loads the same generated
// rows twice — once through tools::bulk_load, once through per-row
// HeapFile::insert — and requires identical scans (records and row ids);
// the refusal checks pin the bootstrap decision rules. Runs inside a
// sandbox directory so data files never touch the workspace.

#include "bufpool/BufferManager.h"
#include "exec/Executor.h"
#include "heapfile/HeapFile.h"
#include "policy/LRUPolicy.h"
#include "sql/Parser.h"
#include "tools/BulkLoader.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
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

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// Parses one statement and runs it through the executor.
bool run_sql(exec::Executor& executor, const std::string& text,
             exec::StatementResult& result, std::string& error) {
    sql::ParseResult parsed;
    if (!sql::parse(text, parsed, error) || parsed.statements.size() != 1) {
        if (error.empty()) {
            error = "expected exactly one statement";
        }
        return false;
    }
    return executor.execute(*parsed.statements.front(), result, error);
}

// The deterministic oracle rows: id = position + 1, an 8-character tag,
// a double score.
tuple::Tuple oracle_row(std::size_t index) {
    tuple::Tuple tuple;
    tuple.add(static_cast<int>(index) + 1);
    std::string tag = "tag_" + std::to_string(index);
    while (tag.size() < 8) {
        tag += "0";
    }
    tuple.add(tag.substr(0, 8));
    tuple.add(static_cast<double>(index) + 0.5);
    return tuple;
}

bool generator(std::size_t index, tuple::Tuple& out, std::string&) {
    out = oracle_row(index);
    return true;
}

}  // namespace

int run_loader_checks() {
    // The sandbox: a fresh temp directory becomes the working directory,
    // so the data files land there. Restored on exit.
    char pattern[] = "/tmp/ex13-loader-XXXXXX";
    char* made = mkdtemp(pattern);
    if (made == nullptr) {
        std::cout << "FAIL: loader sandbox could not be created\n";
        return 1;
    }
    const std::string original = [] {
        std::vector<char> buffer;
        buffer.resize(4096);
        return std::string(getcwd(buffer.data(), buffer.size()));
    }();
    chdir(made);
    if (system("mkdir -p tablecatalog data") != 0) {
        std::cout << "FAIL: loader sandbox data dir\n";
    }

    std::string error;
    bufman::BufferManager manager;
    manager.init(16, std::make_unique<bufman::LRUPolicy>());
    exec::Executor executor(manager);

    // The table schema, through the engine so the String widths are
    // declared: record size 4 + 8 + 8 = 20, capacity (4096-12)/21 = 194.
    exec::StatementResult result;
    check(run_sql(executor,
                  "CREATE TABLE loader_t (id INTEGER, tag VARCHAR(8), "
                  "score DOUBLE)",
                  result, error) &&
              error.empty(),
          "loader_t table created");
    std::vector<schema::TableIndex> indexes;
    const schema::TableSchema* schema =
        executor.table("loader_t", indexes, error);
    check(schema != nullptr, "loader_t schema available");

    const std::size_t capacity = heapfile::slot_capacity(
        schema::TupleSerializer(*schema).record_size());
    const std::size_t count = 2 * capacity + 5;
    const std::string loaded_path = "data/loader_t.tbl";

    // --- 1. Bulk fill into a missing file, then engine-side read-back ---

    tools::BulkLoadStats stats;
    check(tools::bulk_load(loaded_path, *schema, count, generator, stats,
                           error) &&
              error.empty(),
          "bulk load into a missing file succeeds");
    check(stats.records == count && stats.pages == 3 &&
              stats.per_page_capacity == capacity,
          "stats report " + std::to_string(count) + " records on 3 pages of " +
              std::to_string(capacity));

    {
        heapfile::HeapFile file(manager);
        check(file.open(loaded_path, *schema, error), "HeapFile::open accepts the bulk-loaded file");
        bool in_order = true;
        bool ids_match = true;
        std::size_t seen = 0;
        heapfile::HeapFileIterator it = file.scan();
        tuple::Tuple tuple;
        while (it.next(tuple, error)) {
            if (!(tuple == oracle_row(seen))) {
                in_order = false;
            }
            const heapfile::RowId row_id = it.row_id();
            const std::size_t expected_page = seen / capacity + 1;
            const std::size_t expected_slot = seen % capacity;
            if (row_id.page_id != expected_page ||
                row_id.slot != expected_slot) {
                ids_match = false;
            }
            ++seen;
        }
        check(error.empty() && seen == count,
              "scan yields all " + std::to_string(count) + " records");
        check(in_order, "scan order equals generation order");
        check(ids_match, "row ids follow the sequential-insert pattern");

        bool finds_ok = true;
        tuple::Tuple fetched;
        for (std::size_t i = 0; i < count; ++i) {
            if (!file.find(heapfile::RowId{
                    static_cast<std::uint32_t>(i / capacity + 1),
                    static_cast<std::uint32_t>(i % capacity)},
                    fetched, error) ||
                !(fetched == oracle_row(i))) {
                finds_ok = false;
                break;
            }
        }
        check(finds_ok, "find resolves every row id to its generated record");
        check(file.close(error), "file closes");
    }

    // --- 2. The differential oracle: tool vs per-row insert ---

    {
        const std::string twin_path = "data/loader_t_twin.tbl";
        heapfile::HeapFile twin(manager);
        check(twin.create(twin_path, *schema, error) &&
                  twin.open(twin_path, *schema, error),
              "per-row twin file created");
        bool inserts_ok = true;
        for (std::size_t i = 0; i < count; ++i) {
            heapfile::RowId row_id;
            if (!twin.insert(oracle_row(i), row_id, error)) {
                inserts_ok = false;
                break;
            }
        }
        check(inserts_ok, "twin filled by per-row inserts");

        heapfile::HeapFile loaded(manager);
        check(loaded.open(loaded_path, *schema, error),
              "bulk-loaded file reopened for the oracle");
        bool same_records = true;
        bool same_ids = true;
        heapfile::HeapFileIterator a = loaded.scan();
        heapfile::HeapFileIterator b = twin.scan();
        tuple::Tuple ta;
        tuple::Tuple tb;
        while (a.next(ta, error) && b.next(tb, error)) {
            if (!(ta == tb)) {
                same_records = false;
            }
            if (!(a.row_id() == b.row_id())) {
                same_ids = false;
            }
        }
        check(error.empty() && !a.next(ta, error) && !b.next(tb, error),
              "both scans exhaust together");
        check(same_records, "oracle: identical record sequences");
        check(same_ids, "oracle: identical row-id sequences");
        check(loaded.close(error) && twin.close(error), "oracle files close");
    }

    // --- 3. Refusals ---

    {
        tools::BulkLoadStats again;
        check(!tools::bulk_load(loaded_path, *schema, count, generator,
                                again, error) &&
                  contains(error, "already"),
              "non-empty file refused");
        std::size_t still_there = 0;
        heapfile::HeapFile file(manager);
        if (file.open(loaded_path, *schema, error)) {
            heapfile::HeapFileIterator it = file.scan();
            tuple::Tuple tuple;
            while (it.next(tuple, error)) {
                ++still_there;
            }
            check(still_there == count,
                  "refused load left the file unchanged");
            file.close(error);
        } else {
            check(false, "refused file still opens");
        }
    }

    {
        exec::StatementResult u;
        check(run_sql(executor,
                      "CREATE TABLE loader_u (id INTEGER, tag VARCHAR(8), "
                      "score DOUBLE)",
                      u, error) &&
                  error.empty(),
              "loader_u table created");
        std::vector<schema::TableIndex> u_indexes;
        const schema::TableSchema* u_schema =
            executor.table("loader_u", u_indexes, error);
        // A one-block file for loader_u: the identification check is what
        // must refuse loader_t's schema (block-count refusals fire first).
        const std::string u_path = "data/loader_u_only.tbl";
        tools::BulkLoadStats stats_u0;
        check(tools::bulk_load(u_path, *u_schema, 0, generator, stats_u0,
                               error),
              "one-block loader_u file created");
        tools::BulkLoadStats stats_u;
        check(!tools::bulk_load(u_path, *schema, count, generator, stats_u,
                                error) &&
                  contains(error, "loader_u"),
              "foreign table name refused");
    }

    {
        // Corrupt block 0 in a one-block copy, then refuse.
        const std::string corrupt_path = "data/corrupt.tbl";
        std::ifstream src("data/loader_empty.tbl", std::ios::binary);
        std::ofstream dst(corrupt_path, std::ios::binary);
        dst << src.rdbuf();
        dst.close();
        bufman::File raw;
        std::array<char, bufman::kBlockSize> junk{};
        junk.fill('x');
        check(bufman::open_for_append(corrupt_path, raw, error) &&
                  bufman::write_block(raw, 0, junk.data(), junk.size(),
                                      error),
              "corrupt header written");
        bufman::close(raw);
        tools::BulkLoadStats stats_c;
        check(!tools::bulk_load(corrupt_path, *schema, count, generator,
                                stats_c, error) &&
                  contains(error, "not a tuple heap file"),
              "corrupt header refused");
    }

    {
        schema::TableSchema empty("no_cols");
        tools::BulkLoadStats stats_e;
        check(!tools::bulk_load("data/empty.tbl", empty, 10, generator,
                                stats_e, error) &&
                  contains(error, "no record layout"),
              "record-layout-less schema refused");
    }

    {
        exec::StatementResult big;
        check(run_sql(executor, "CREATE TABLE loader_big (x VARCHAR(4090))",
                      big, error) &&
                  error.empty(),
              "oversized table created");
        std::vector<schema::TableIndex> big_indexes;
        const schema::TableSchema* big_schema =
            executor.table("loader_big", big_indexes, error);
        tools::BulkLoadStats stats_b;
        check(!tools::bulk_load("data/loader_big.tbl", *big_schema, 10,
                                generator, stats_b, error) &&
                  contains(error, "page fits records"),
              "oversized record refused");
    }

    // --- 4. Edge shapes: empty load, generator failure ---

    {
        const std::string empty_path = "data/loader_empty.tbl";
        tools::BulkLoadStats stats_z;
        check(tools::bulk_load(empty_path, *schema, 0, generator, stats_z,
                               error) &&
                  error.empty() && stats_z.records == 0 &&
                  stats_z.pages == 0,
              "zero-record load succeeds with no pages");
        heapfile::HeapFile file(manager);
        check(file.open(empty_path, *schema, error),
              "empty bulk-loaded file opens");
        heapfile::HeapFileIterator it = file.scan();
        tuple::Tuple tuple;
        check(!it.next(tuple, error) && error.empty(),
              "empty file scans to exhaustion immediately");
        file.close(error);
    }

    {
        tools::BulkLoadStats stats_f;
        const tools::RowGenerator failing =
            [](std::size_t index, tuple::Tuple& out, std::string& err) {
                if (index == 5) {
                    err = "boom";
                    return false;
                }
                out = oracle_row(index);
                return true;
            };
        check(!tools::bulk_load("data/loader_fail.tbl", *schema, 100,
                                failing, stats_f, error) &&
                  contains(error, "index 5"),
              "generator failure names the index");
    }

    chdir(original.c_str());
    std::cout << "\nloader checks: " << failures << " failure(s)\n";
    return failures;
}
