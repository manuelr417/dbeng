// ex14 — bulk loader checks: the bootstrap tool's contract against the
// HeapFile class's own. The differential oracle loads the same generated
// rows twice — once through tools::bulk_load, once through per-row
// HeapFile::insert — and requires identical scans (records and row ids);
// the refusal checks pin the bootstrap decision rules. Runs inside a
// sandbox directory so data files never touch the workspace.

#include "bufpool/BufferManager.h"
#include "common/Error.h"
#include "exec/Executor.h"
#include "heapfile/HeapFile.h"
#include "policy/LRUPolicy.h"
#include "sql/Parser.h"
#include "tools/BulkLoader.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <functional>
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

// Runs `op` expecting success. PASS with `label` when it returns; a FAIL
// naming the exception when it throws.
template <typename Op>
void run(const Op& op, const std::string& label) {
    try {
        op();
        check(true, label);
    } catch (const common::Error& e) {
        check(false, label + " (threw: " + e.what() + ")");
    }
}

// Runs `op` expecting a throw. Returns the thrown message, or "" when
// nothing was thrown.
template <typename Op>
std::string refused(const Op& op) {
    try {
        op();
        return "";
    } catch (const common::Error& e) {
        return e.what();
    }
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// Parses one statement and runs it through the executor.
void run_sql(exec::Executor& executor, const std::string& text,
             exec::StatementResult& result) {
    sql::ParseResult parsed = sql::parse(text);
    if (parsed.statements.size() != 1) {
        throw common::Error("expected exactly one statement");
    }
    executor.execute(*parsed.statements.front(), result);
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
    char pattern[] = "/tmp/ex14-loader-XXXXXX";
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

    bufman::BufferManager manager;
    manager.init(16, std::make_unique<bufman::LRUPolicy>());
    exec::Executor executor(manager);

    // The table schema, through the engine so the String widths are
    // declared: record size 4 + 8 + 8 = 20, capacity (4096-12)/21 = 194.
    exec::StatementResult result;
    run([&] {
        run_sql(executor,
                "CREATE TABLE loader_t (id INTEGER, tag VARCHAR(8), "
                "score DOUBLE)",
                result);
    }, "loader_t table created");
    std::vector<schema::TableIndex> indexes;
    const schema::TableSchema& schema = *executor.table("loader_t", indexes);
    check(true, "loader_t schema available");

    const std::size_t capacity = heapfile::slot_capacity(
        schema::TupleSerializer(schema).record_size());
    const std::size_t count = 2 * capacity + 5;
    const std::string loaded_path = "data/loader_t.tbl";

    // --- 1. Bulk fill into a missing file, then engine-side read-back ---

    tools::BulkLoadStats stats;
    run([&] {
        tools::bulk_load(loaded_path, schema, count, generator, stats);
    }, "bulk load into a missing file succeeds");
    check(stats.records == count && stats.pages == 3 &&
              stats.per_page_capacity == capacity,
          "stats report " + std::to_string(count) + " records on 3 pages of " +
              std::to_string(capacity));

    {
        heapfile::HeapFile file(manager);
        run([&] { file.open(loaded_path, schema); },
            "HeapFile::open accepts the bulk-loaded file");
        bool in_order = true;
        bool ids_match = true;
        std::size_t seen = 0;
        heapfile::HeapFileIterator it = file.scan();
        tuple::Tuple tuple;
        while (it.next(tuple)) {
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
        check(seen == count,
              "scan yields all " + std::to_string(count) + " records");
        check(in_order, "scan order equals generation order");
        check(ids_match, "row ids follow the sequential-insert pattern");

        bool finds_ok = true;
        tuple::Tuple fetched;
        for (std::size_t i = 0; i < count; ++i) {
            try {
                file.find(heapfile::RowId{
                    static_cast<std::uint32_t>(i / capacity + 1),
                    static_cast<std::uint32_t>(i % capacity)}, fetched);
                if (!(fetched == oracle_row(i))) {
                    finds_ok = false;
                    break;
                }
            } catch (const common::Error&) {
                finds_ok = false;
                break;
            }
        }
        check(finds_ok, "find resolves every row id to its generated record");
        run([&] { file.close(); }, "file closes");
    }

    // --- 2. The differential oracle: tool vs per-row insert ---

    {
        const std::string twin_path = "data/loader_t_twin.tbl";
        heapfile::HeapFile twin(manager);
        run([&] {
            twin.create(twin_path, schema);
            twin.open(twin_path, schema);
        }, "per-row twin file created");
        bool inserts_ok = true;
        for (std::size_t i = 0; i < count; ++i) {
            heapfile::RowId row_id;
            try {
                twin.insert(oracle_row(i), row_id);
            } catch (const common::Error&) {
                inserts_ok = false;
                break;
            }
        }
        check(inserts_ok, "twin filled by per-row inserts");

        heapfile::HeapFile loaded(manager);
        run([&] { loaded.open(loaded_path, schema); },
            "bulk-loaded file reopened for the oracle");
        bool same_records = true;
        bool same_ids = true;
        heapfile::HeapFileIterator a = loaded.scan();
        heapfile::HeapFileIterator b = twin.scan();
        tuple::Tuple ta;
        tuple::Tuple tb;
        while (a.next(ta) && b.next(tb)) {
            if (!(ta == tb)) {
                same_records = false;
            }
            if (!(a.row_id() == b.row_id())) {
                same_ids = false;
            }
        }
        check(!a.next(ta) && !b.next(tb), "both scans exhaust together");
        check(same_records, "oracle: identical record sequences");
        check(same_ids, "oracle: identical row-id sequences");
        run([&] {
            loaded.close();
            twin.close();
        }, "oracle files close");
    }

    // --- 3. Refusals ---

    {
        tools::BulkLoadStats again;
        check(contains(refused([&] {
                           tools::bulk_load(loaded_path, schema, count,
                                            generator, again);
                       }),
                       "already"),
              "non-empty file refused");
        std::size_t still_there = 0;
        heapfile::HeapFile file(manager);
        try {
            file.open(loaded_path, schema);
            heapfile::HeapFileIterator it = file.scan();
            tuple::Tuple tuple;
            while (it.next(tuple)) {
                ++still_there;
            }
            check(still_there == count,
                  "refused load left the file unchanged");
            file.close();
        } catch (const common::Error&) {
            check(false, "refused file still opens");
        }
    }

    {
        exec::StatementResult u;
        run([&] {
            run_sql(executor,
                    "CREATE TABLE loader_u (id INTEGER, tag VARCHAR(8), "
                    "score DOUBLE)",
                    u);
        }, "loader_u table created");
        std::vector<schema::TableIndex> u_indexes;
        const schema::TableSchema& u_schema =
            *executor.table("loader_u", u_indexes);
        // A one-block file for loader_u: the identification check is what
        // must refuse loader_t's schema (block-count refusals fire first).
        const std::string u_path = "data/loader_u_only.tbl";
        tools::BulkLoadStats stats_u0;
        run([&] {
            tools::bulk_load(u_path, u_schema, 0, generator, stats_u0);
        }, "one-block loader_u file created");
        tools::BulkLoadStats stats_u;
        check(contains(refused([&] {
                           tools::bulk_load(u_path, schema, count, generator,
                                            stats_u);
                       }),
                       "loader_u"),
              "foreign table name refused");
    }

    {
        // Corrupt block 0 in a one-block copy, then refuse.
        const std::string corrupt_path = "data/corrupt.tbl";
        std::ifstream src("data/loader_empty.tbl", std::ios::binary);
        std::ofstream dst(corrupt_path, std::ios::binary);
        dst << src.rdbuf();
        dst.close();
        std::array<char, bufman::kBlockSize> junk{};
        junk.fill('x');
        run([&] {
            bufman::File raw = bufman::open_for_append(corrupt_path);
            bufman::write_block(raw, 0, junk.data(), junk.size());
            bufman::close(raw);
        }, "corrupt header written");
        tools::BulkLoadStats stats_c;
        check(contains(refused([&] {
                           tools::bulk_load(corrupt_path, schema, count,
                                            generator, stats_c);
                       }),
                       "not a tuple heap file"),
              "corrupt header refused");
    }

    {
        schema::TableSchema empty("no_cols");
        tools::BulkLoadStats stats_e;
        check(contains(refused([&] {
                           tools::bulk_load("data/empty.tbl", empty, 10,
                                            generator, stats_e);
                       }),
                       "no record layout"),
              "record-layout-less schema refused");
    }

    {
        exec::StatementResult big;
        run([&] {
            run_sql(executor, "CREATE TABLE loader_big (x VARCHAR(4090))",
                    big);
        }, "oversized table created");
        std::vector<schema::TableIndex> big_indexes;
        const schema::TableSchema& big_schema =
            *executor.table("loader_big", big_indexes);
        tools::BulkLoadStats stats_b;
        check(contains(refused([&] {
                           tools::bulk_load("data/loader_big.tbl", big_schema,
                                            10, generator, stats_b);
                       }),
                       "page fits records"),
              "oversized record refused");
    }

    // --- 4. Edge shapes: empty load, generator failure ---

    {
        const std::string empty_path = "data/loader_empty.tbl";
        tools::BulkLoadStats stats_z;
        run([&] {
            tools::bulk_load(empty_path, schema, 0, generator, stats_z);
            if (stats_z.records != 0 || stats_z.pages != 0) {
                throw common::Error("stats not empty");
            }
        }, "zero-record load succeeds with no pages");
        heapfile::HeapFile file(manager);
        run([&] { file.open(empty_path, schema); },
            "empty bulk-loaded file opens");
        heapfile::HeapFileIterator it = file.scan();
        tuple::Tuple tuple;
        check(!it.next(tuple), "empty file scans to exhaustion immediately");
        file.close();
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
        check(contains(refused([&] {
                           tools::bulk_load("data/loader_fail.tbl", schema,
                                            100, failing, stats_f);
                       }),
                       "index 5"),
              "generator failure names the index");
    }

    chdir(original.c_str());
    std::cout << "\nloader checks: " << failures << " failure(s)\n";
    return failures;
}
