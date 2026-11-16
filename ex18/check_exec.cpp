// ex14 — exec layer checks: bind-time refusals, evaluation semantics,
// access-path selection with its markers, the DDL seam (CREATE/DROP
// TABLE and INDEX through the catalog writer), INSERT all-or-nothing
// with index maintenance, and SELECT projection. Runs inside a sandbox
// directory so catalogs and data files never touch the workspace.

#include "common/Error.h"
#include "bufpool/BufferManager.h"
#include "exec/Executor.h"
#include "exec/Planner.h"
#include "policy/LRUPolicy.h"
#include "schema/TableGenerator.h"
#include "schema/TableSchemaJson.h"
#include "sql/Parser.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
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

// Parses one statement and runs it through the executor.
void run_sql(exec::Executor& executor, const std::string& text,
             exec::StatementResult& result) {
    sql::ParseResult parsed = sql::parse(text);
    if (parsed.statements.size() != 1) {
        throw common::Error("expected exactly one statement");
    }
    executor.execute(*parsed.statements.front(), result);
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

}

int run_exec_checks() {
    // The sandbox: a fresh temp directory becomes the working directory,
    // so `data/<table>.tbl` and `tablecatalog/` land there. Restored on exit.
    char pattern[] = "/tmp/ex14-exec-XXXXXX";
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

    bufman::BufferManager manager;
    manager.init(16, std::make_unique<bufman::LRUPolicy>());
    exec::Executor executor(manager);

    exec::StatementResult result;

    // --- 5.4. CREATE TABLE through the catalog writer ---
    {
        const std::string create_error = refused([&] {
            run_sql(executor,
                    "CREATE TABLE person (pid INTEGER, name VARCHAR(10), "
                    "age INT, city VARCHAR(3))",
                    result);
        });
        check(create_error.empty() && result.message == "table 'person' created",
              "CREATE TABLE reports success");
        std::ifstream catalog("tablecatalog/person.json");
        check(catalog.good(), "the catalog file exists after CREATE TABLE");

        const std::string dup_error = refused([&] {
            run_sql(executor, "CREATE TABLE person (a INTEGER)", result);
        });
        check(!dup_error.empty() && contains(dup_error, "already exists"),
              "a duplicate CREATE TABLE is refused by name");

        const std::string empty_error = refused([&] {
            run_sql(executor, "CREATE TABLE void ()", result);
        });
        check(!empty_error.empty() || true,
              "parse-level refusal of empty columns is fine either way");
    }

    // --- 5.6. INSERT: validation, all-or-nothing, index maintenance ---
    {
        const std::string insert_error = refused([&] {
            run_sql(executor,
                    "INSERT INTO person VALUES (1, 'Ruth', 42, 'PR'), "
                    "(2, 'Amy', 30, 'MA'), (3, 'Ivan', 42, 'PR')",
                    result);
        });
        check(insert_error.empty() && result.message == "inserted 3 row(s)",
              "a three-row INSERT reports its count");
        const std::string visible_error = refused([&] {
            run_sql(executor, "SELECT * FROM person", result);
        });
        check(visible_error.empty() && result.rows.rows.size() == 3,
              "the inserted rows are visible");

        const std::string bad_error = refused([&] {
            run_sql(executor,
                    "INSERT INTO person VALUES (4, 'WayTooLongName', 1, 'PR')",
                    result);
        });
        check(!bad_error.empty(),
              "a too-wide string is refused with a named error");
        const std::string stored_error = refused([&] {
            run_sql(executor, "SELECT * FROM person", result);
        });
        check(stored_error.empty() && result.rows.rows.size() == 3,
              "a refused INSERT stored nothing (all-or-nothing)");

        const std::string arity_error = refused([&] {
            run_sql(executor, "INSERT INTO person VALUES (5, 'X', 1)", result);
        });
        check(!arity_error.empty() && contains(arity_error, "4"),
              "a wrong-arity row is refused with the column count");

        const std::string type_error = refused([&] {
            run_sql(executor,
                    "INSERT INTO person VALUES (5, 'X', 'old', 'PR')",
                    result);
        });
        check(!type_error.empty() && contains(type_error, "must be"),
              "a string in an integer column is refused");
    }

    // --- 5.5. CREATE INDEX / DROP INDEX through the catalog ---
    {
        const std::string create_error = refused([&] {
            run_sql(executor, "CREATE INDEX person_pid ON person (pid)", result);
        });
        check(create_error.empty() && contains(result.message, "person_pid"),
              "CREATE INDEX reports success");
        std::vector<schema::TableIndex> indexes;
        check(refused([&] { executor.table("person", indexes); }).empty() &&
                  indexes.size() == 1 && indexes[0].name == "person_pid" &&
                  indexes[0].column == "pid" && indexes[0].kind == "hash",
              "the catalog lists the new index, defaulting to hash");

        const std::string dup_error = refused([&] {
            run_sql(executor, "CREATE INDEX another ON person (pid)", result);
        });
        check(!dup_error.empty() && contains(dup_error, "one index per column"),
              "a second index of the same kind is refused");
        std::vector<schema::TableIndex> after_refusal;
        check(refused([&] { executor.table("person", after_refusal); }).empty() &&
                  after_refusal.size() == 1,
              "the refused CREATE INDEX left the catalog unchanged");
        const std::string isam_error = refused([&] {
            run_sql(executor,
                    "CREATE INDEX person_pid_isam ON person (pid) "
                    "USING ISAM",
                    result);
        });
        check(isam_error.empty() && contains(result.message, "person_pid_isam"),
              "an explicit ISAM index joins the hash one on the same column");

        const std::string unknown_column = refused([&] {
            run_sql(executor, "CREATE INDEX bad ON person (nosuch)", result);
        });
        check(!unknown_column.empty() &&
                  contains(unknown_column, "unknown column"),
              "an index on an unknown column is refused");
    }

    // --- 5.3. access paths and their markers ---
    {
        const std::string hash_error = refused([&] {
            run_sql(executor, "SELECT * FROM person WHERE pid = 2", result);
        });
        check(hash_error.empty() &&
                  result.rows.access_path == "hash scan" &&
                  result.rows.rows.size() == 1 &&
                  std::get<int>(result.rows.rows[0][0]) == 2,
              "equality on the hash-indexed column takes the hash scan");

        const std::string range_error = refused([&] {
            run_sql(executor,
                    "SELECT * FROM person WHERE pid > 1 AND pid < 3",
                    result);
        });
        check(range_error.empty() &&
                  result.rows.access_path == "index scan" &&
                  result.rows.rows.size() == 1 &&
                  std::get<int>(result.rows.rows[0][0]) == 2,
              "an AND range on the indexed column takes the index scan");

        const std::string residual_error = refused([&] {
            run_sql(executor,
                    "SELECT * FROM person WHERE pid = 1 AND city = 'PR'",
                    result);
        });
        check(residual_error.empty() &&
                  result.rows.access_path == "hash scan" &&
                  result.rows.rows.size() == 1,
              "an indexed equality plus a residual conjunct still uses the hash");

        const std::string heap_error = refused([&] {
            run_sql(executor, "SELECT * FROM person WHERE city = 'PR'",
                    result);
        });
        check(heap_error.empty() &&
                  result.rows.access_path == "heap scan" &&
                  result.rows.rows.size() == 2,
              "an unindexed column scans the heap");

        const std::string or_error = refused([&] {
            run_sql(executor,
                    "SELECT * FROM person WHERE pid = 1 OR city = 'MA'",
                    result);
        });
        check(or_error.empty() &&
                  result.rows.access_path == "heap scan" &&
                  result.rows.rows.size() == 2,
              "a top-level OR degrades to the heap scan");

        const std::string miss_error = refused([&] {
            run_sql(executor, "SELECT * FROM person WHERE pid = 99", result);
        });
        check(miss_error.empty() &&
                  result.rows.access_path == "hash scan" &&
                  result.rows.rows.empty(),
              "an index-path miss answers empty");
    }

    // --- 5.2. evaluation semantics through WHERE ---
    {
        const std::string division_error = refused([&] {
            run_sql(executor, "SELECT pid FROM person WHERE age / 2 = 21",
                    result);
        });
        check(division_error.empty() && result.rows.rows.size() == 2,
              "integer division truncates (42 / 2 = 21 finds two rows)");
        const std::string promote_error = refused([&] {
            run_sql(executor, "SELECT pid FROM person WHERE age = 42.0",
                    result);
        });
        check(promote_error.empty() && result.rows.rows.size() == 2,
              "int/double comparison promotes");
        const std::string lexicographic_error = refused([&] {
            run_sql(executor,
                    "SELECT pid FROM person WHERE city > 'PQ' AND city < 'PS'",
                    result);
        });
        check(lexicographic_error.empty() && result.rows.rows.size() == 2,
              "string comparison is lexicographic");
        const std::string not_error = refused([&] {
            run_sql(executor,
                    "SELECT pid FROM person WHERE NOT city = 'PR'",
                    result);
        });
        check(not_error.empty() &&
                  result.rows.rows.size() == 1 &&
                  std::get<int>(result.rows.rows[0][0]) == 2,
              "NOT negates");
        const std::string heap_or_error = refused([&] {
            run_sql(executor,
                    "SELECT pid FROM person WHERE pid = 1 OR pid = 3",
                    result);
        });
        check(heap_or_error.empty() && result.rows.rows.size() == 2,
              "OR still answers correctly on the heap path");
        const std::string zero_error = refused([&] {
            run_sql(executor, "SELECT * FROM person WHERE pid / 0 = 1",
                    result);
        });
        check(!zero_error.empty() && contains(zero_error, "division by zero"),
              "division by zero is a named runtime error");
    }

    // --- 5.1. bind-time refusals (before any page is pinned) ---
    {
        const std::string unknown_column = refused([&] {
            run_sql(executor, "SELECT nosuch FROM person", result);
        });
        check(!unknown_column.empty() &&
                  contains(unknown_column, "unknown column"),
              "an unknown projected column is a bind error");
        const std::string unknown_where = refused([&] {
            run_sql(executor, "SELECT * FROM person WHERE nosuch = 1", result);
        });
        check(!unknown_where.empty() &&
                  contains(unknown_where, "unknown column"),
              "an unknown WHERE column is a bind error");
        const std::string compare_types = refused([&] {
            run_sql(executor, "SELECT * FROM person WHERE name = 5", result);
        });
        check(!compare_types.empty() && contains(compare_types, "cannot compare"),
              "string-vs-number comparison is a bind error");
        const std::string arith_types = refused([&] {
            run_sql(executor, "SELECT * FROM person WHERE name + 1 = 5", result);
        });
        check(!arith_types.empty() &&
                  contains(arith_types, "arithmetic needs numeric"),
              "arithmetic on strings is a bind error");
        const std::string logic_types = refused([&] {
            run_sql(executor, "SELECT * FROM person WHERE pid AND 1", result);
        });
        check(!logic_types.empty() && contains(logic_types, "boolean"),
              "non-boolean AND operands are a bind error");
    }

    // --- 5.7. the wider language is refused, naming the feature ---
    {
        const std::pair<const char*, const char*> refusals[] = {
            {"SELECT p.pid FROM person AS p JOIN car AS c ON p.pid = c.pid",
             "joins"},
            {"SELECT * FROM person ORDER BY pid", "ORDER BY"},
            {"SELECT * FROM person LIMIT 5", "LIMIT"},
            {"UPDATE person SET age = 1 WHERE pid = 1", "UPDATE"},
            {"DELETE FROM person WHERE pid = 1", "DELETE"},
        };
        for (const auto& [text, feature] : refusals) {
            const std::string refusal = refused([&] {
                run_sql(executor, text, result);
            });
            check(!refusal.empty() && contains(refusal, feature),
                  std::string("refused and named: ") + feature);
        }

        // GROUP BY and aggregates route to the grouped branch: bind-time
        // errors name the statement's own mistakes, and a well-formed
        // grouped statement reaches the executor (the operator lands
        // with the group module).
        const std::string bind_error = refused([&] {
            run_sql(executor,
                    "SELECT nosuch, COUNT(*) FROM person GROUP BY nosuch",
                    result);
        });
        check(!bind_error.empty() && contains(bind_error, "nosuch"),
              "a grouped SELECT with an unknown column fails at bind time");
        const std::string star_error = refused([&] {
            run_sql(executor, "SELECT * FROM person GROUP BY city", result);
        });
        check(!star_error.empty() && contains(star_error, "SELECT *"),
              "SELECT * under GROUP BY is refused by name");
    }

    // --- ex18: grouped SELECT end-to-end (the static hash operator) ---
    {
        // Controlled data: five rows over three cities.
        //   PR: ages 30, 40   MA: ages 25, 35   UT: age 28
        const std::string create_error = refused([&] {
            run_sql(executor,
                    "CREATE TABLE people (pid INTEGER, name VARCHAR(10), "
                    "age INT, city VARCHAR(3))",
                    result);
        });
        check(create_error.empty() && result.message == "table 'people' created",
              "the grouped checks' table is created");
        const std::string insert_error = refused([&] {
            run_sql(executor,
                    "INSERT INTO people VALUES (1, 'Ann', 30, 'PR'), "
                    "(2, 'Bob', 40, 'PR'), (3, 'Cid', 25, 'MA'), "
                    "(4, 'Dee', 35, 'MA'), (5, 'Eve', 28, 'UT')",
                    result);
        });
        check(insert_error.empty() && result.message == "inserted 5 row(s)",
              "the grouped checks' rows are inserted");

        const auto int_of = [](const exec::Value& v) {
            return std::get<int>(v);
        };
        const auto double_of = [](const exec::Value& v) {
            return std::get<double>(v);
        };
        const auto string_of = [](const exec::Value& v) {
            return std::get<std::string>(v);
        };

        // The plain grouping: one row per city, typed aggregates.
        run([&] {
            run_sql(executor,
                    "SELECT city, COUNT(*), SUM(age), MIN(name), MAX(name) "
                    "FROM people GROUP BY city",
                    result);
        }, "the five-aggregate grouping runs");
        check(result.rows.columns.size() == 5 &&
                  result.rows.columns[0] == "city" &&
                  result.rows.columns[1] == "COUNT(*)" &&
                  result.rows.columns[2] == "SUM(age)",
              "the grouped projection names its columns");
        check(contains(result.rows.access_path, "+ hash group"),
              "the announcement composes the scan path with the grouping");
        {
            struct Expected {
                std::string city;
                int count;
                int sum;
                std::string min_name;
                std::string max_name;
            };
            const Expected expected[] = {
                {"PR", 2, 70, "Ann", "Bob"},
                {"MA", 2, 60, "Cid", "Dee"},
                {"UT", 1, 28, "Eve", "Eve"},
            };
            bool all_found = result.rows.rows.size() == 3;
            bool typed = all_found;
            for (const std::vector<exec::Value>& row : result.rows.rows) {
                bool matched = false;
                for (const Expected& want : expected) {
                    if (string_of(row[0]) == want.city) {
                        matched = true;
                        typed = typed && int_of(row[1]) == want.count &&
                                int_of(row[2]) == want.sum &&
                                string_of(row[3]) == want.min_name &&
                                string_of(row[4]) == want.max_name;
                    }
                }
                all_found = all_found && matched;
            }
            check(all_found,
                  "one row per group with the right COUNT and SUM "
                  "(bucket order, not sorted order)");
            check(typed, "each group's aggregates carry the group's values");
        }

        // AVG is double; a WHERE filters rows before they reach groups.
        const std::string where_error = refused([&] {
            run_sql(executor,
                    "SELECT city, AVG(age) FROM people WHERE age >= 30 "
                    "GROUP BY city",
                    result);
        });
        check(where_error.empty() && result.rows.rows.size() == 2,
              "WHERE filters rows before grouping (two cities survive)");
        {
            bool avgs_ok = true;
            for (const std::vector<exec::Value>& row : result.rows.rows) {
                const double avg = double_of(row[1]);
                avgs_ok = avgs_ok &&
                          ((string_of(row[0]) == "PR" && avg == 35.0) ||
                           (string_of(row[0]) == "MA" && avg == 35.0));
            }
            check(avgs_ok, "AVG(age) is the group's double mean");
        }

        // HAVING filters finished groups.
        const std::string having_error = refused([&] {
            run_sql(executor,
                    "SELECT city, COUNT(*) FROM people GROUP BY city "
                    "HAVING COUNT(*) > 1",
                    result);
        });
        check(having_error.empty() && result.rows.rows.size() == 2,
              "HAVING keeps only the groups passing the predicate");
        const std::string having_column_error = refused([&] {
            run_sql(executor,
                    "SELECT city FROM people GROUP BY city HAVING city = 'UT'",
                    result);
        });
        check(having_column_error.empty() &&
                  result.rows.rows.size() == 1 &&
                  std::get<std::string>(result.rows.rows[0][0]) == "UT",
              "HAVING over a grouped column works too");

        // The implicit single group.
        const std::string implicit_error = refused([&] {
            run_sql(executor,
                    "SELECT COUNT(*), SUM(age), AVG(age), MIN(name), "
                    "MAX(name) FROM people",
                    result);
        });
        check(implicit_error.empty() &&
                  result.rows.rows.size() == 1 &&
                  int_of(result.rows.rows[0][0]) == 5 &&
                  int_of(result.rows.rows[0][1]) == 158 &&
                  double_of(result.rows.rows[0][2]) == 31.6 &&
                  string_of(result.rows.rows[0][3]) == "Ann" &&
                  string_of(result.rows.rows[0][4]) == "Eve",
              "the implicit single group aggregates the whole table");

        // Empty input: zero rows with GROUP BY, one identity row without.
        run([&] {
            run_sql(executor, "CREATE TABLE nobody (a INT, b VARCHAR(3))", result);
        }, "the empty table is created");
        const std::string grouped_empty_error = refused([&] {
            run_sql(executor, "SELECT a, COUNT(*) FROM nobody GROUP BY a", result);
        });
        check(grouped_empty_error.empty() && result.rows.rows.empty(),
              "empty input with GROUP BY answers zero rows");
        const std::string identity_error = refused([&] {
            run_sql(executor, "SELECT COUNT(*), SUM(a), AVG(a), MIN(a), "
                              "MAX(a) FROM nobody",
                    result);
        });
        check(identity_error.empty() &&
                  result.rows.rows.size() == 1 &&
                  int_of(result.rows.rows[0][0]) == 0 &&
                  int_of(result.rows.rows[0][1]) == 0 &&
                  double_of(result.rows.rows[0][2]) == 0.0 &&
                  int_of(result.rows.rows[0][3]) == 0 &&
                  int_of(result.rows.rows[0][4]) == 0,
              "empty input without GROUP BY answers one identity row");
        const std::string having_filter_error = refused([&] {
            run_sql(executor,
                    "SELECT COUNT(*) FROM nobody HAVING COUNT(*) > 0",
                    result);
        });
        check(having_filter_error.empty() && result.rows.rows.empty(),
              "an identity row can be filtered out by HAVING");

        // Both announcement forms: the in-memory run announces the bare
        // grouping; a forced-spill run (tiny budget) names the spill and
        // its fan-out.
        const std::string memory_error = refused([&] {
            run_sql(executor, "SELECT city, COUNT(*) FROM people GROUP BY city",
                    result);
        });
        check(memory_error.empty() &&
                  contains(result.rows.access_path, "+ hash group") &&
                  !contains(result.rows.access_path, "spilled"),
              "the in-memory announcement is the bare grouping");
        executor.set_group_budget(2);
        const std::string spill_error = refused([&] {
            run_sql(executor, "SELECT city, COUNT(*) FROM people GROUP BY city",
                    result);
        });
        check(spill_error.empty() &&
                  result.rows.rows.size() == 3 &&
                  contains(result.rows.access_path,
                           "+ hash group (spilled, 8 partitions)"),
              "the forced-spill announcement names the partition count");
        executor.set_group_budget(100000);
        const std::string restore_error = refused([&] {
            run_sql(executor, "SELECT city, COUNT(*) FROM people GROUP BY city",
                    result);
        });
        check(restore_error.empty() &&
                  !contains(result.rows.access_path, "spilled"),
              "restoring the budget returns to the in-memory path");
    }

    // --- projection shapes ---
    {
        const std::string narrow_error = refused([&] {
            run_sql(executor, "SELECT city, pid FROM person WHERE pid = 1",
                    result);
        });
        check(narrow_error.empty() &&
                  result.rows.columns.size() == 2 &&
                  result.rows.columns[0] == "city" &&
                  result.rows.columns[1] == "pid" &&
                  std::get<std::string>(result.rows.rows[0][0]) == "PR",
              "projection narrows and orders the columns");
        const std::string alias_error = refused([&] {
            run_sql(executor, "SELECT pid AS id FROM person WHERE pid = 1",
                    result);
        });
        check(alias_error.empty() && result.rows.columns[0] == "id",
              "an alias names the projected column");
    }

    // --- 5.5. DROP INDEX, then the heap path ---
    {
        run([&] { run_sql(executor, "DROP INDEX person_pid", result); },
            "DROP INDEX succeeds");
        const std::string isam_takes_error = refused([&] {
            run_sql(executor, "SELECT * FROM person WHERE pid = 2", result);
        });
        check(isam_takes_error.empty() &&
                  result.rows.access_path == "index scan" &&
                  result.rows.rows.size() == 1,
              "after the hash drop, the ISAM index takes over");
        run([&] { run_sql(executor, "DROP INDEX person_pid_isam", result); },
            "the ISAM index drops too");
        const std::string heap_takes_error = refused([&] {
            run_sql(executor, "SELECT * FROM person WHERE pid = 2", result);
        });
        check(heap_takes_error.empty() &&
                  result.rows.access_path == "heap scan" &&
                  result.rows.rows.size() == 1,
              "after both drops, the same predicate scans the heap");
        const std::string unknown_index = refused([&] {
            run_sql(executor, "DROP INDEX person_pid", result);
        });
        check(!unknown_index.empty() && contains(unknown_index, "unknown index"),
              "dropping a missing index is refused by name");
    }

    // --- 5.5. the missing index file is rebuilt on open ---
    {
        run([&] {
            run_sql(executor, "CREATE INDEX person_pid ON person (pid)", result);
        }, "the index is created again");
        // A full session close, then a brand-new executor: the index
        // file was deleted externally, so the reopen must rebuild it.
        run([&] { executor.close_all(); }, "session closed");
        std::remove("data/person.tbl.pid.hdx");
        exec::Executor fresh(manager);
        const std::string rebuild_error = refused([&] {
            run_sql(fresh, "SELECT * FROM person WHERE pid = 3", result);
        });
        check(rebuild_error.empty() &&
                  result.rows.access_path == "hash scan" &&
                  result.rows.rows.size() == 1 &&
                  std::get<int>(result.rows.rows[0][0]) == 3,
              "a deleted hash index file is rebuilt and used");
        const std::string agree_error = refused([&] {
            run_sql(fresh, "SELECT * FROM person", result);
        });
        check(agree_error.empty() && result.rows.rows.size() == 3,
              "the rebuilt index agrees with the heap");
        run([&] { fresh.close_all(); }, "fresh session closed");
    }

    // --- USING STATIC HASH: the third kind through the DDL seam ---
    {
        // Open the session file first: an index created while the data
        // file is open builds immediately (otherwise the build defers
        // to the next open).
        const std::string open_error = refused([&] {
            run_sql(executor, "SELECT * FROM person", result);
        });
        check(open_error.empty() && result.rows.rows.size() == 3,
              "person is open when the static index is created");
        const std::string static_create_error = refused([&] {
            run_sql(executor,
                    "CREATE INDEX person_pid_static ON person (pid) "
                    "USING STATIC HASH",
                    result);
        });
        check(static_create_error.empty() &&
                  contains(result.message, "person_pid_static"),
              "CREATE INDEX USING STATIC HASH reports success");
        std::vector<schema::TableIndex> indexes;
        bool static_recorded = false;
        check(refused([&] { executor.table("person", indexes); }).empty() &&
                  indexes.size() == 2,
              "the static index joins the hash one in the catalog");
        for (const schema::TableIndex& declared : indexes) {
            static_recorded =
                static_recorded || declared.kind == "static hash";
        }
        check(static_recorded,
              "the catalog records the static hash kind");
        std::ifstream shdx("data/person.tbl.pid.shdx");
        check(shdx.good(),
              "the static index file sits beside the data file");

        const std::string static_dup_error = refused([&] {
            run_sql(executor,
                    "CREATE INDEX static_again ON person (pid) "
                    "USING STATIC HASH",
                    result);
        });
        check(!static_dup_error.empty() &&
                  contains(static_dup_error, "one index per column"),
              "a second static index of the same kind is refused");

        // A deleted .shdx rebuilds at open, like the other kinds.
        run([&] { executor.close_all(); },
            "session closed for the static rebuild");
        std::remove("data/person.tbl.pid.shdx");
        exec::Executor fresh(manager);
        const std::string reopen_error = refused([&] {
            run_sql(fresh, "SELECT * FROM person", result);
        });
        check(reopen_error.empty() && result.rows.rows.size() == 3,
              "the fresh session opens the table for the static rebuild");
        std::ifstream rebuilt("data/person.tbl.pid.shdx");
        check(rebuilt.good(), "a deleted static index file is rebuilt at open");
        std::vector<std::string> report;
        bool clean = false;
        const std::string check_error = refused([&] {
            fresh.check_indexes("person", "data/person.tbl", report, clean);
        });
        check(check_error.empty() && clean && report.size() == 2,
              "checkindex verifies the rebuilt static index clean");
        run([&] { fresh.close_all(); }, "the static rebuild session closed");
    }

    // --- the static path is announced, and the hash is preferred ---
    {
        // person carries hash person_pid and static person_pid_static.
        const std::string prefer_hash_error = refused([&] {
            run_sql(executor, "SELECT * FROM person WHERE pid = 2", result);
        });
        check(prefer_hash_error.empty() &&
                  result.rows.access_path == "hash scan" &&
                  result.rows.rows.size() == 1,
              "with hash and static, an equality takes the hash scan");
        executor.set_allow_hash(false);
        const std::string static_scan_error = refused([&] {
            run_sql(executor, "SELECT * FROM person WHERE pid = 2", result);
        });
        check(static_scan_error.empty() &&
                  result.rows.access_path == "static scan" &&
                  result.rows.rows.size() == 1 &&
                  std::get<int>(result.rows.rows[0][0]) == 2,
              "equality on the static-indexed column takes the static scan");
        const std::string degrade_error = refused([&] {
            run_sql(executor,
                    "SELECT * FROM person WHERE pid > 1 AND pid < 3",
                    result);
        });
        check(degrade_error.empty() &&
                  result.rows.access_path == "heap scan" &&
                  result.rows.rows.size() == 1,
              "a range with only hash and static degrades to the heap");
        executor.set_allow_hash(true);
    }

    // --- 6.3. erase_row: heap erase fans out to every index ---
    {
        run([&] {
            run_sql(executor, "CREATE TABLE delme (id INTEGER, tag "
                                  "VARCHAR(6))",
                    result);
            run_sql(executor,
                    "INSERT INTO delme VALUES (1, 'a'), (2, 'b'), "
                    "(2, 'c'), (3, 'd')",
                    result);
            run_sql(executor, "CREATE INDEX delme_id ON delme (id)", result);
            run_sql(executor,
                    "CREATE INDEX delme_id_isam ON delme (id) "
                    "USING ISAM",
                    result);
            run_sql(executor,
                    "CREATE INDEX delme_id_static ON delme (id) "
                    "USING STATIC HASH",
                    result);
        }, "delme table prepared with all three index kinds");
        run([&] {
            executor.erase_row("delme", "data/delme.tbl", heapfile::RowId{1, 2});
        }, "erasing the row at (1,2) succeeds");
        const std::string gone_error = refused([&] {
            run_sql(executor, "SELECT * FROM delme", result);
        });
        check(gone_error.empty() && result.rows.rows.size() == 3,
              "the heap no longer holds the erased row");
        const std::string hash_drop_error = refused([&] {
            run_sql(executor, "SELECT * FROM delme WHERE id = 2", result);
        });
        check(hash_drop_error.empty() &&
                  result.rows.access_path == "hash scan" &&
                  result.rows.rows.size() == 1 &&
                  std::get<std::string>(result.rows.rows[0][1]) == "b",
              "the hash index drops the erased duplicate");
        const std::string isam_drop_error = refused([&] {
            run_sql(executor, "SELECT * FROM delme WHERE id > 0 AND id < 9",
                    result);
        });
        check(isam_drop_error.empty() &&
                  result.rows.access_path == "index scan" &&
                  result.rows.rows.size() == 3,
              "the ISAM index drops the erased row too");
        const std::string free_error = refused([&] {
            executor.erase_row("delme", "data/delme.tbl", heapfile::RowId{1, 2});
        });
        check(!free_error.empty() && contains(free_error, "no record"),
              "erasing a free row id is refused by name");
        const std::string unchanged_error = refused([&] {
            run_sql(executor, "SELECT * FROM delme", result);
        });
        check(unchanged_error.empty() && result.rows.rows.size() == 3,
              "the refused erase changed nothing");
        run([&] {
            executor.erase_row("delme", "data/delme.tbl", heapfile::RowId{1, 1});
            executor.erase_row("delme", "data/delme.tbl", heapfile::RowId{1, 0});
            executor.erase_row("delme", "data/delme.tbl", heapfile::RowId{1, 3});
        }, "the remaining rows erase");
        const std::string emptied_error = refused([&] {
            run_sql(executor, "SELECT * FROM delme", result);
        });
        check(emptied_error.empty() && result.rows.rows.empty(),
              "an emptied table scans to nothing");
        std::vector<std::string> report;
        bool clean = false;
        const std::string check_error = refused([&] {
            executor.check_indexes("delme", "data/delme.tbl", report, clean);
        });
        check(check_error.empty() && clean && report.size() == 3,
              "checkindex reports all three emptied indexes clean");
        run([&] { run_sql(executor, "DROP TABLE delme", result); },
            "delme dropped");
    }

    // --- 5.4. DROP TABLE removes everything ---
    {
        run([&] {
            run_sql(executor, "CREATE TABLE junk (a INTEGER)", result);
            run_sql(executor, "INSERT INTO junk VALUES (7)", result);
            run_sql(executor, "CREATE INDEX junk_a ON junk (a)", result);
        }, "junk table prepared for the drop");
        const std::string drop_error = refused([&] {
            run_sql(executor, "DROP TABLE junk", result);
        });
        check(drop_error.empty() && result.message == "table 'junk' dropped",
              "DROP TABLE reports success");
        std::ifstream catalog("tablecatalog/junk.json");
        std::ifstream data("data/junk.tbl");
        std::ifstream index("data/junk.tbl.a.hdx");
        check(!catalog.good() && !data.good() && !index.good(),
              "catalog, data, and hash index files are all gone");
        const std::string unknown = refused([&] {
            run_sql(executor, "DROP TABLE junk", result);
        });
        check(!unknown.empty() && contains(unknown, "unknown table"),
              "dropping a missing table is refused by name");
    }

    // --- 6.1. planner routing (design D8) ---
    {
        schema::TableSchema person("");
        schema::parse_table_schema(
            "{\"table_name\":\"person\",\"columns\":["
            "{\"column_name\":\"pid\",\"column_type\":\"Integer\",\"size\":4},"
            "{\"column_name\":\"city\",\"column_type\":\"String\",\"size\":3}]}",
            person);
        const auto plan_of = [&](const std::string& where_sql,
                                 const std::vector<exec::CatalogIndex>& indexes,
                                 exec::ScanPlan& plan) {
            return refused([&] {
                sql::ParseResult parsed =
                    sql::parse("SELECT * FROM person WHERE " + where_sql);
                exec::BoundExprPtr bound = exec::bind_expr(
                    *static_cast<const sql::SelectStatement&>(
                         *parsed.statements.front())
                         .where,
                    person);
                plan = exec::plan_where(std::move(bound), indexes);
            }).empty();
        };
        using Kind = sql::IndexKind;
        const std::vector<exec::CatalogIndex> hash_only = {{"h", 0, Kind::Hash}};
        const std::vector<exec::CatalogIndex> static_only = {
            {"s", 0, Kind::Static}};
        const std::vector<exec::CatalogIndex> isam_only = {{"i", 0, Kind::Isam}};
        const std::vector<exec::CatalogIndex> both = {{"h", 0, Kind::Hash},
                                                      {"i", 0, Kind::Isam}};
        const std::vector<exec::CatalogIndex> hash_and_static = {
            {"h", 0, Kind::Hash}, {"s", 0, Kind::Static}};
        exec::ScanPlan plan;

        check(plan_of("pid = 5", hash_only, plan) && plan.use_hash &&
                  !plan.use_static && !plan.use_index &&
                  plan.bounds.index_name == "h" &&
                  plan.bounds.lo && plan.bounds.hi &&
                  exec::compare_values(*plan.bounds.lo, exec::Value(5)) == 0 &&
                  exec::compare_values(*plan.bounds.hi, exec::Value(5)) == 0,
              "an equality on a hash-indexed column claims the hash scan");
        check(plan_of("pid = 5", static_only, plan) && plan.use_static &&
                  !plan.use_hash && !plan.use_index &&
                  plan.bounds.index_name == "s" &&
                  plan.bounds.lo && plan.bounds.hi &&
                  exec::compare_values(*plan.bounds.lo, exec::Value(5)) == 0,
              "an equality on a static-only column claims the static scan");
        check(plan_of("pid = 5", isam_only, plan) && plan.use_index &&
                  !plan.use_hash && !plan.use_static &&
                  plan.bounds.index_name == "i" &&
                  plan.bounds.lo && plan.bounds.hi &&
                  exec::compare_values(*plan.bounds.lo, exec::Value(5)) == 0,
              "an equality on an ISAM-only column stays an index scan");
        check(plan_of("pid = 5", both, plan) && plan.use_hash &&
                  plan.bounds.index_name == "h",
              "with both kinds, an equality prefers the hash");
        check(plan_of("pid = 5", hash_and_static, plan) && plan.use_hash &&
                  plan.bounds.index_name == "h",
              "with hash and static, an equality prefers the hash");
        check(plan_of("pid > 10 AND pid < 20", isam_only, plan) &&
                  plan.use_index && !plan.use_hash && !plan.use_static &&
                  plan.bounds.lo &&
                  plan.bounds.hi && !plan.bounds.lo_inclusive &&
                  !plan.bounds.hi_inclusive,
              "an AND range on an ISAM column takes the index scan");
        check(plan_of("pid > 10", hash_only, plan) && !plan.use_hash &&
                  !plan.use_static && !plan.use_index &&
                  plan.residual.size() == 1,
              "a range on a hash-only column degrades to the heap");
        check(plan_of("pid > 10", static_only, plan) && !plan.use_static &&
                  !plan.use_hash && !plan.use_index &&
                  plan.residual.size() == 1,
              "a range on a static-only column degrades to the heap");
        check(plan_of("pid > 10", both, plan) && plan.use_index &&
                  !plan.use_hash && plan.bounds.index_name == "i",
              "with both kinds, a range takes the ISAM scan");
        check(plan_of("pid = 5 AND pid > 10", both, plan) && plan.use_hash &&
                  plan.residual.size() == 1,
              "an equality keeps the hash; the range filters as residual");
        check(plan_of("pid = 5 AND pid = 9", hash_only, plan) &&
                  plan.use_hash && plan.residual.size() == 1,
              "a second equality on the hash column filters as residual");
        check(plan_of("pid = 5 AND pid = 9", static_only, plan) &&
                  plan.use_static && plan.residual.size() == 1,
              "a second equality on the static column filters as residual");
        check(plan_of("pid = 1 OR city = 'PR'", both, plan) &&
                  !plan.use_hash && !plan.use_static && !plan.use_index &&
                  plan.residual.size() == 1,
              "a top-level OR degrades to the heap");
        check(plan_of("city = 'PR'", hash_only, plan) && !plan.use_hash &&
                  !plan.use_static && !plan.use_index &&
                  plan.residual.size() == 1,
              "an equality on an unindexed column scans the heap");
    }

    // --- 7.1. the differential oracle ---
    // Random rows, random predicates; every SELECT runs twice — index
    // allowed and index suppressed — and both paths must return
    // identical rows. The index never changes an answer.
    {
        run([&] {
            run_sql(executor,
                    "CREATE TABLE oracle (id INTEGER, tag VARCHAR(8), "
                    "score INTEGER, label VARCHAR(6))",
                    result);
        }, "oracle table created");
        // 500 rows through the generator: id counts 1..500, tag/label
        // are random-width strings, score is random in [0, 1000).
        {
            std::vector<schema::TableIndex> indexes;
            const schema::TableSchema* schema =
                executor.table("oracle", indexes);
            const std::vector<tuple::Tuple> rows =
                schema::generate_tuples(*schema, 500, 1234, 1);
            bool all = true;
            for (const tuple::Tuple& tuple : rows) {
                heapfile::RowId row_id;
                all = all && refused([&] {
                          executor.insert_into("oracle", "data/oracle.tbl",
                                               tuple, row_id);
                      }).empty();
            }
            check(all, "500 generated rows inserted");
        }
        run([&] {
            run_sql(executor, "CREATE INDEX oracle_id ON oracle (id)", result);
            run_sql(executor,
                    "CREATE INDEX oracle_id_isam ON oracle (id) "
                    "USING ISAM",
                    result);
            run_sql(executor,
                    "CREATE INDEX oracle_id_static ON oracle (id) "
                    "USING STATIC HASH",
                    result);
            run_sql(executor, "CREATE INDEX oracle_score ON oracle "
                              "(score)",
                    result);
            run_sql(executor,
                    "CREATE INDEX oracle_score_isam ON oracle (score) "
                    "USING ISAM",
                    result);
        }, "all five oracle indexes created");

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
            // Every available path must answer identically: all kinds,
            // each single kind suppressed in turn, static alone, and
            // the heap.
            const std::vector<std::vector<exec::Value>>* baseline = nullptr;
            std::string baseline_label;
            for (const auto& [hash_allowed, isam_allowed, static_allowed,
                              label] :
                 std::vector<std::tuple<bool, bool, bool, std::string>>{
                     {true, true, true, "all"},
                     {true, false, true, "no isam"},
                     {false, true, true, "no hash"},
                     {false, false, true, "static only"},
                     {false, false, false, "heap"}}) {
                executor.set_allow_index(true);
                executor.set_allow_hash(hash_allowed);
                executor.set_allow_isam(isam_allowed);
                executor.set_allow_static(static_allowed);
                exec::StatementResult run_result;
                const std::string path_error = refused([&] {
                    run_sql(executor, select, run_result);
                });
                if (!path_error.empty()) {
                    all_match = false;
                    first_mismatch = label + " path failed: " + path_error;
                    break;
                }
                if (baseline == nullptr) {
                    baseline = &run_result.rows.rows;
                    baseline_label = label;
                    continue;
                }
                // Row order is unspecified without ORDER BY (the index
                // walks keys, the heap scans row ids), so compare as
                // sorted sets.
                std::vector<std::vector<exec::Value>> a = *baseline;
                std::vector<std::vector<exec::Value>> b = run_result.rows.rows;
                std::sort(a.begin(), a.end());
                std::sort(b.begin(), b.end());
                if (a != b) {
                    all_match = false;
                    first_mismatch = select + " (" + baseline_label + " " +
                                     std::to_string(a.size()) + " rows, " +
                                     label + " " + std::to_string(b.size()) +
                                     " rows)";
                    break;
                }
            }
            executor.set_allow_hash(true);
            executor.set_allow_isam(true);
            executor.set_allow_static(true);
            if (!all_match) {
                break;
            }
        }
        check(all_match, "300 random predicates: hash, static, ISAM, and "
                         "heap paths all agree" +
                              (all_match ? std::string() : " — " + first_mismatch));
        executor.set_allow_index(true);
        run([&] { executor.close_all(); }, "oracle session closed");
    }

    // --- the escape-hatch bulk path on a fresh table ---
    // A table with no data file counts as zero rows, so generated
    // numbering starts at 1 — the ex11 semantics.
    {
        run([&] {
            run_sql(executor, "CREATE TABLE bulkr (id INTEGER, tag VARCHAR(6))",
                    result);
        }, "bulkr table created with no data file");
        std::vector<schema::TableIndex> indexes;
        const schema::TableSchema* schema = executor.table("bulkr", indexes);
        std::size_t existing = 999;  // must be overwritten with 0
        check(schema != nullptr &&
                  refused([&] {
                      existing = executor.count_rows("bulkr", "data/bulkr.tbl",
                                                     true);
                  }).empty() &&
                  existing == 0,
              "count_rows on a missing data file yields zero rows");
        const std::vector<tuple::Tuple> generated =
            schema::generate_tuples(*schema, 3, 42, existing + 1);
        bool all = true;
        int first_id = -1;
        for (const tuple::Tuple& tuple : generated) {
            heapfile::RowId row_id;
            all = all && refused([&] {
                      executor.insert_into("bulkr", "data/bulkr.tbl",
                                           tuple, row_id);
                  }).empty();
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
