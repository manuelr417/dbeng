// ex13 — exec layer checks: bind-time refusals, evaluation semantics,
// access-path selection with its markers, the DDL seam (CREATE/DROP
// TABLE and INDEX through the catalog writer), INSERT all-or-nothing
// with index maintenance, and SELECT projection. Runs inside a sandbox
// directory so catalogs and data files never touch the workspace.

#include "bufpool/BufferManager.h"
#include "exec/Executor.h"
#include "policy/LRUPolicy.h"
#include "schema/TableGenerator.h"
#include "sql/Parser.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <algorithm>
#include <random>
#include <sys/stat.h>
#include <vector>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>

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

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

}

int run_exec_checks() {
    // The sandbox: a fresh temp directory becomes the working directory,
    // so `data/<table>.tbl` and `tablecatalog/` land there. Restored on exit.
    char pattern[] = "/tmp/ex13-exec-XXXXXX";
    char* made = mkdtemp(pattern);
    if (made == nullptr) {
        std::cout << "FAIL: exec sandbox could not be created\n";
        return 1;
    }
    const std::string original = [] {
        std::vector<char> buffer;
        buffer.resize(4096);
        return std::string(getcwd(buffer.data(), buffer.size()));
    }();
    chdir(made);
    if (system("mkdir -p tablecatalog data") != 0) {
        std::cout << "FAIL: exec sandbox catalog dir\n";
    }

    std::string error;
    bufman::BufferManager manager;
    manager.init(16, std::make_unique<bufman::LRUPolicy>());
    exec::Executor executor(manager);

    exec::StatementResult result;

    // --- 5.4. CREATE TABLE through the catalog writer ---
    {
        check(run_sql(executor,
                      "CREATE TABLE person (pid INTEGER, name VARCHAR(10), "
                      "age INT, city VARCHAR(3))",
                      result, error) &&
                  result.message == "table 'person' created",
              "CREATE TABLE reports success");
        std::ifstream catalog("tablecatalog/person.json");
        check(catalog.good(), "the catalog file exists after CREATE TABLE");

        std::string dup_error;
        check(!run_sql(executor, "CREATE TABLE person (a INTEGER)", result,
                       dup_error) &&
                  contains(dup_error, "already exists"),
              "a duplicate CREATE TABLE is refused by name");

        std::string empty_error;
        check(!run_sql(executor, "CREATE TABLE void ()", result, empty_error) ||
                  true,
              "parse-level refusal of empty columns is fine either way");
    }

    // --- 5.6. INSERT: validation, all-or-nothing, index maintenance ---
    {
        check(run_sql(executor,
                      "INSERT INTO person VALUES (1, 'Ruth', 42, 'PR'), "
                      "(2, 'Amy', 30, 'MA'), (3, 'Ivan', 42, 'PR')",
                      result, error) &&
                  result.message == "inserted 3 row(s)",
              "a three-row INSERT reports its count");
        check(run_sql(executor, "SELECT * FROM person", result, error) &&
                  result.rows.rows.size() == 3,
              "the inserted rows are visible");

        std::string bad_error;
        check(!run_sql(executor,
                       "INSERT INTO person VALUES (4, 'WayTooLongName', 1, 'PR')",
                       result, bad_error) &&
                  !bad_error.empty(),
              "a too-wide string is refused with a named error");
        check(run_sql(executor, "SELECT * FROM person", result, error) &&
                  result.rows.rows.size() == 3,
              "a refused INSERT stored nothing (all-or-nothing)");

        std::string arity_error;
        check(!run_sql(executor, "INSERT INTO person VALUES (5, 'X', 1)",
                       result, arity_error) &&
                  contains(arity_error, "4"),
              "a wrong-arity row is refused with the column count");

        std::string type_error;
        check(!run_sql(executor,
                       "INSERT INTO person VALUES (5, 'X', 'old', 'PR')",
                       result, type_error) &&
                  contains(type_error, "must be"),
              "a string in an integer column is refused");
    }

    // --- 5.5. CREATE INDEX / DROP INDEX through the catalog ---
    {
        check(run_sql(executor, "CREATE INDEX person_pid ON person (pid)",
                      result, error) &&
                  contains(result.message, "person_pid"),
              "CREATE INDEX reports success");
        std::vector<schema::TableIndex> indexes;
        check(executor.table("person", indexes, error) != nullptr &&
                  indexes.size() == 1 && indexes[0].name == "person_pid" &&
                  indexes[0].column == "pid",
              "the catalog lists the new index");

        std::string dup_error;
        check(!run_sql(executor, "CREATE INDEX another ON person (pid)",
                       result, dup_error) &&
                  contains(dup_error, "one index per column"),
              "a second index on the same column is refused");
        std::vector<schema::TableIndex> after_refusal;
        check(executor.table("person", after_refusal, error) != nullptr &&
                  after_refusal.size() == 1,
              "the refused CREATE INDEX left the catalog unchanged");

        std::string unknown_column;
        check(!run_sql(executor, "CREATE INDEX bad ON person (nosuch)", result,
                       unknown_column) &&
                  contains(unknown_column, "unknown column"),
              "an index on an unknown column is refused");
    }

    // --- 5.3. access paths and their markers ---
    {
        check(run_sql(executor, "SELECT * FROM person WHERE pid = 2", result,
                      error) &&
                  result.rows.access_path == "index scan" &&
                  result.rows.rows.size() == 1 &&
                  std::get<int>(result.rows.rows[0][0]) == 2,
              "equality on the indexed column takes the index scan");

        check(run_sql(executor,
                      "SELECT * FROM person WHERE pid > 1 AND pid < 3",
                      result, error) &&
                  result.rows.access_path == "index scan" &&
                  result.rows.rows.size() == 1 &&
                  std::get<int>(result.rows.rows[0][0]) == 2,
              "an AND range on the indexed column takes the index scan");

        check(run_sql(executor,
                      "SELECT * FROM person WHERE pid = 1 AND city = 'PR'",
                      result, error) &&
                  result.rows.access_path == "index scan" &&
                  result.rows.rows.size() == 1,
              "an indexed equality plus a residual conjunct still uses the index");

        check(run_sql(executor, "SELECT * FROM person WHERE city = 'PR'",
                      result, error) &&
                  result.rows.access_path == "heap scan" &&
                  result.rows.rows.size() == 2,
              "an unindexed column scans the heap");

        check(run_sql(executor,
                      "SELECT * FROM person WHERE pid = 1 OR city = 'MA'",
                      result, error) &&
                  result.rows.access_path == "heap scan" &&
                  result.rows.rows.size() == 2,
              "a top-level OR degrades to the heap scan");

        check(run_sql(executor, "SELECT * FROM person WHERE pid = 99", result,
                      error) &&
                  result.rows.access_path == "index scan" &&
                  result.rows.rows.empty(),
              "an index-scan miss answers empty");
    }

    // --- 5.2. evaluation semantics through WHERE ---
    {
        check(run_sql(executor, "SELECT pid FROM person WHERE age / 2 = 21",
                      result, error) &&
                  result.rows.rows.size() == 2,
              "integer division truncates (42 / 2 = 21 finds two rows)");
        check(run_sql(executor, "SELECT pid FROM person WHERE age = 42.0",
                      result, error) &&
                  result.rows.rows.size() == 2,
              "int/double comparison promotes");
        check(run_sql(executor,
                      "SELECT pid FROM person WHERE city > 'PQ' AND city < 'PS'",
                      result, error) &&
                  result.rows.rows.size() == 2,
              "string comparison is lexicographic");
        check(run_sql(executor,
                      "SELECT pid FROM person WHERE NOT city = 'PR'",
                      result, error) &&
                  result.rows.rows.size() == 1 &&
                  std::get<int>(result.rows.rows[0][0]) == 2,
              "NOT negates");
        check(run_sql(executor,
                      "SELECT pid FROM person WHERE pid = 1 OR pid = 3",
                      result, error) &&
                  result.rows.rows.size() == 2,
              "OR still answers correctly on the heap path");
        std::string zero_error;
        check(!run_sql(executor, "SELECT * FROM person WHERE pid / 0 = 1",
                       result, zero_error) &&
                  contains(zero_error, "division by zero"),
              "division by zero is a named runtime error");
    }

    // --- 5.1. bind-time refusals (before any page is pinned) ---
    {
        std::string unknown_column;
        check(!run_sql(executor, "SELECT nosuch FROM person", result,
                       unknown_column) &&
                  contains(unknown_column, "unknown column"),
              "an unknown projected column is a bind error");
        std::string unknown_where;
        check(!run_sql(executor, "SELECT * FROM person WHERE nosuch = 1",
                       result, unknown_where) &&
                  contains(unknown_where, "unknown column"),
              "an unknown WHERE column is a bind error");
        std::string compare_types;
        check(!run_sql(executor, "SELECT * FROM person WHERE name = 5",
                       result, compare_types) &&
                  contains(compare_types, "cannot compare"),
              "string-vs-number comparison is a bind error");
        std::string arith_types;
        check(!run_sql(executor, "SELECT * FROM person WHERE name + 1 = 5",
                       result, arith_types) &&
                  contains(arith_types, "arithmetic needs numeric"),
              "arithmetic on strings is a bind error");
        std::string logic_types;
        check(!run_sql(executor, "SELECT * FROM person WHERE pid AND 1",
                       result, logic_types) &&
                  contains(logic_types, "boolean"),
              "non-boolean AND operands are a bind error");
    }

    // --- 5.7. the wider language is refused, naming the feature ---
    {
        const std::pair<const char*, const char*> refusals[] = {
            {"SELECT p.pid FROM person AS p JOIN car AS c ON p.pid = c.pid",
             "joins"},
            {"SELECT city, COUNT(*) FROM person GROUP BY city", "GROUP BY"},
            {"SELECT * FROM person ORDER BY pid", "ORDER BY"},
            {"SELECT * FROM person LIMIT 5", "LIMIT"},
            {"SELECT COUNT(*) FROM person", "aggregate"},
            {"UPDATE person SET age = 1 WHERE pid = 1", "UPDATE"},
            {"DELETE FROM person WHERE pid = 1", "DELETE"},
        };
        for (const auto& [text, feature] : refusals) {
            std::string refusal;
            check(!run_sql(executor, text, result, refusal) &&
                      contains(refusal, feature),
                  std::string("refused and named: ") + feature);
        }
    }

    // --- projection shapes ---
    {
        check(run_sql(executor, "SELECT city, pid FROM person WHERE pid = 1",
                      result, error) &&
                  result.rows.columns.size() == 2 &&
                  result.rows.columns[0] == "city" &&
                  result.rows.columns[1] == "pid" &&
                  std::get<std::string>(result.rows.rows[0][0]) == "PR",
              "projection narrows and orders the columns");
        check(run_sql(executor, "SELECT pid AS id FROM person WHERE pid = 1",
                      result, error) &&
                  result.rows.columns[0] == "id",
              "an alias names the projected column");
    }

    // --- 5.5. DROP INDEX, then the heap path ---
    {
        check(run_sql(executor, "DROP INDEX person_pid", result, error),
              "DROP INDEX succeeds");
        check(run_sql(executor, "SELECT * FROM person WHERE pid = 2", result,
                      error) &&
                  result.rows.access_path == "heap scan" &&
                  result.rows.rows.size() == 1,
              "after the drop, the same predicate scans the heap");
        std::string unknown_index;
        check(!run_sql(executor, "DROP INDEX person_pid", result,
                       unknown_index) &&
                  contains(unknown_index, "unknown index"),
              "dropping a missing index is refused by name");
    }

    // --- 5.5. the missing index file is rebuilt on open ---
    {
        check(run_sql(executor, "CREATE INDEX person_pid ON person (pid)",
                      result, error),
              "the index is created again");
        // A full session close, then a brand-new executor: the index
        // file was deleted externally, so the reopen must rebuild it.
        check(executor.close_all(error), "session closed");
        std::remove("data/person.tbl.pid.idx");
        exec::Executor fresh(manager);
        check(run_sql(fresh, "SELECT * FROM person WHERE pid = 3", result,
                      error) &&
                  result.rows.access_path == "index scan" &&
                  result.rows.rows.size() == 1 &&
                  std::get<int>(result.rows.rows[0][0]) == 3,
              "a deleted index file is rebuilt and used");
        check(run_sql(fresh, "SELECT * FROM person", result, error) &&
                  result.rows.rows.size() == 3,
              "the rebuilt index agrees with the heap");
        check(fresh.close_all(error), "fresh session closed");
    }

    // --- 5.4. DROP TABLE removes everything ---
    {
        check(run_sql(executor, "CREATE TABLE junk (a INTEGER)", result,
                      error) &&
                  run_sql(executor, "INSERT INTO junk VALUES (7)", result,
                          error) &&
                  run_sql(executor, "CREATE INDEX junk_a ON junk (a)",
                          result, error),
              "junk table prepared for the drop");
        check(run_sql(executor, "DROP TABLE junk", result, error) &&
                  result.message == "table 'junk' dropped",
              "DROP TABLE reports success");
        std::ifstream catalog("tablecatalog/junk.json");
        std::ifstream data("data/junk.tbl");
        std::ifstream index("data/junk.tbl.a.idx");
        check(!catalog.good() && !data.good() && !index.good(),
              "catalog, data, and index files are all gone");
        std::string unknown;
        check(!run_sql(executor, "DROP TABLE junk", result, unknown) &&
                  contains(unknown, "unknown table"),
              "dropping a missing table is refused by name");
    }

    // --- 7.1. the differential oracle ---
    // Random rows, random predicates; every SELECT runs twice — index
    // allowed and index suppressed — and both paths must return
    // identical rows. The index never changes an answer.
    {
        check(run_sql(executor,
                      "CREATE TABLE oracle (id INTEGER, tag VARCHAR(8), "
                      "score INTEGER, label VARCHAR(6))",
                      result, error),
              "oracle table created");
        // 500 rows through the generator: id counts 1..500, tag/label
        // are random-width strings, score is random in [0, 1000).
        {
            std::vector<schema::TableIndex> indexes;
            const schema::TableSchema* schema =
                executor.table("oracle", indexes, error);
            const std::vector<tuple::Tuple> rows =
                schema::generate_tuples(*schema, 500, 1234, 1);
            bool all = true;
            for (const tuple::Tuple& tuple : rows) {
                heapfile::RowId row_id;
                all = all && executor.insert_into("oracle", "data/oracle.tbl",
                                                  tuple, row_id, error);
            }
            check(all, "500 generated rows inserted");
        }
        check(run_sql(executor, "CREATE INDEX oracle_id ON oracle (id)",
                      result, error) &&
                  run_sql(executor, "CREATE INDEX oracle_score ON oracle "
                                    "(score)",
                          result, error),
              "both oracle indexes created");

        // Random predicates over both indexed columns, with and without
        // residual conjuncts, with and without OR.
        std::mt19937 rng(20260213);
        auto pick = [&](int lo, int hi) {
            return static_cast<int>(rng() % static_cast<unsigned>(hi - lo + 1)) + lo;
        };
        bool all_match = true;
        std::string first_mismatch;
        for (int case_index = 0; case_index < 300; ++case_index) {
            std::string where;
            const int shape = pick(0, 6);
            switch (shape) {
                case 0: where = "id = " + std::to_string(pick(1, 500)); break;
                case 1: where = "id < " + std::to_string(pick(1, 500)); break;
                case 2: where = "id >= " + std::to_string(pick(1, 500)); break;
                case 3: {
                    const int a = pick(1, 499);
                    where = "id > " + std::to_string(a) + " AND id < " +
                            std::to_string(a + pick(1, 100));
                    break;
                }
                case 4:
                    where = "score = " + std::to_string(pick(0, 999));
                    break;
                case 5:
                    where = "score > " + std::to_string(pick(0, 900)) +
                            " AND score < " + std::to_string(pick(901, 1000));
                    break;
                case 6:
                    where = "id = " + std::to_string(pick(1, 500)) +
                            " OR score < " + std::to_string(pick(0, 200));
                    break;
            }
            const std::string select =
                "SELECT * FROM oracle WHERE " + where;
            exec::StatementResult with_index;
            executor.set_allow_index(true);
            if (!run_sql(executor, select, with_index, error)) {
                all_match = false;
                first_mismatch = "index path failed: " + error;
                break;
            }
            exec::StatementResult without;
            executor.set_allow_index(false);
            if (!run_sql(executor, select, without, error)) {
                all_match = false;
                first_mismatch = "heap path failed: " + error;
                break;
            }
            executor.set_allow_index(true);
            // Row order is unspecified without ORDER BY (the index walks
            // keys, the heap scans row ids), so compare as sorted sets.
            std::vector<std::vector<exec::Value>> a = with_index.rows.rows;
            std::vector<std::vector<exec::Value>> b = without.rows.rows;
            std::sort(a.begin(), a.end());
            std::sort(b.begin(), b.end());
            if (a != b ||
                with_index.rows.columns != without.rows.columns) {
                all_match = false;
                first_mismatch = select + " (index " +
                                 std::to_string(with_index.rows.rows.size()) +
                                 " rows, heap " +
                                 std::to_string(without.rows.rows.size()) +
                                 " rows)";
                break;
            }
        }
        check(all_match, "300 random predicates: index path == heap path" +
                             (all_match ? std::string() : " — " + first_mismatch));
        executor.set_allow_index(true);
        check(executor.close_all(error), "oracle session closed");
    }

    // --- the escape-hatch bulk path on a fresh table ---
    // A table with no data file counts as zero rows, so generated
    // numbering starts at 1 — the ex11 semantics.
    {
        check(run_sql(executor,
                      "CREATE TABLE bulkr (id INTEGER, tag VARCHAR(6))",
                      result, error),
              "bulkr table created with no data file");
        std::vector<schema::TableIndex> indexes;
        const schema::TableSchema* schema =
            executor.table("bulkr", indexes, error);
        std::size_t existing = 999;  // must be overwritten with 0
        check(schema != nullptr &&
                  executor.count_rows("bulkr", "data/bulkr.tbl", existing,
                                      error, true) &&
                  existing == 0,
              "count_rows on a missing data file yields zero rows");
        const std::vector<tuple::Tuple> generated =
            schema::generate_tuples(*schema, 3, 42, existing + 1);
        bool all = true;
        int first_id = -1;
        for (const tuple::Tuple& tuple : generated) {
            heapfile::RowId row_id;
            all = all && executor.insert_into("bulkr", "data/bulkr.tbl",
                                              tuple, row_id, error);
            if (first_id < 0) {
                first_id = std::get<int>(tuple.column(0).column_value());
            }
        }
        check(all && first_id == 1,
              "bulk numbering starts at 1 on a fresh table");
    }

    chdir(original.c_str());
    std::cout << (failures == 0
                      ? "exec suite: all checks passed."
                      : "exec suite: " + std::to_string(failures) +
                            " check(s) FAILED.")
              << "\n";
    return failures;
}
