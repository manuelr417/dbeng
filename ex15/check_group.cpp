// ex15 — group-by checks: the binder's aggregate typing and inventory,
// the aggregate placement refusals, then (as they land) the static-hash
// group operator, the restart spill, and the differential oracle.
// Binding is checked against an in-code person schema; no files.

#include "bufpool/BufferManager.h"
#include "exec/Binder.h"
#include "exec/Executor.h"
#include "group/GroupBy.h"
#include "group/PartitionRun.h"
#include "heapfile/HeapFile.h"
#include "policy/LRUPolicy.h"
#include "sql/Parser.h"

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <utility>
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

// Parses one statement and runs it through the executor (the spill
// checks' sandbox path).
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

// Spill runs left behind in data/ — must be zero after a clean run.
int count_spill_files() {
    DIR* dir = opendir("data");
    if (dir == nullptr) {
        return -1;
    }
    int count = 0;
    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.find(".gspill") != std::string::npos) {
            ++count;
        }
    }
    closedir(dir);
    return count;
}

// The check schema: person(pid INTEGER, name VARCHAR(10), age INT,
// city VARCHAR(3)) — pid=0, name=1, age=2, city=3.
schema::TableSchema person_schema() {
    schema::TableSchema schema("person");
    std::string error;
    schema.add_column("pid", tuple::ColumnType::Integer, error);
    schema.add_column("name", tuple::ColumnType::String, error);
    schema.add_column("age", tuple::ColumnType::Integer, error);
    schema.add_column("city", tuple::ColumnType::String, error);
    return schema;
}

// Parses one select item expression (the first item of
// "SELECT <expr> FROM person") and hands the AST node over; the holder
// keeps the tree alive until the next call.
const sql::Expr* item_expr(const std::string& text) {
    static sql::ParseResult holder;
    sql::ParseResult parsed;
    std::string error;
    if (!sql::parse("SELECT " + text + " FROM person", parsed, error) ||
        parsed.statements.size() != 1) {
        holder.statements.clear();
        return nullptr;
    }
    holder = std::move(parsed);
    const auto* select =
        static_cast<const sql::SelectStatement*>(holder.statements[0].get());
    return select->items[0].expr.get();
}

// Same, for a WHERE expression.
const sql::Expr* where_expr(const std::string& text) {
    static sql::ParseResult holder;
    sql::ParseResult parsed;
    std::string error;
    if (!sql::parse("SELECT name FROM person WHERE " + text, parsed, error) ||
        parsed.statements.size() != 1) {
        holder.statements.clear();
        return nullptr;
    }
    holder = std::move(parsed);
    const auto* select =
        static_cast<const sql::SelectStatement*>(holder.statements[0].get());
    return select->where.get();
}

// The group binding whose only key is `city` (column 3), the suite's
// default grouping.
exec::GroupBinding city_grouping() {
    exec::GroupBinding group;
    group.key_columns.push_back(3);
    return group;
}

}

int run_group_checks() {
    const schema::TableSchema schema = person_schema();

    // --- binder: aggregate typing and the inventory ---
    {
        exec::GroupBinding group = city_grouping();
        exec::BoundExprPtr bound;
        std::string error;

        const sql::Expr* count_star = item_expr("COUNT(*)");
        check(exec::bind_group_expr(*count_star, schema, group, bound, error) &&
                  bound != nullptr && bound->type == exec::Type::Int &&
                  group.aggregates.size() == 1 &&
                  group.aggregates[0].arg == nullptr,
              "COUNT(*) binds to an int aggregate with no argument");

        const sql::Expr* sum_age = item_expr("SUM(age)");
        check(exec::bind_group_expr(*sum_age, schema, group, bound, error) &&
                  bound != nullptr && bound->type == exec::Type::Int &&
                  group.aggregates.size() == 2,
              "SUM(integer) stays int and joins the inventory");

        const sql::Expr* avg_age = item_expr("AVG(age)");
        check(exec::bind_group_expr(*avg_age, schema, group, bound, error) &&
                  bound != nullptr && bound->type == exec::Type::Double,
              "AVG binds to double");

        const sql::Expr* min_name = item_expr("MIN(name)");
        check(exec::bind_group_expr(*min_name, schema, group, bound, error) &&
                  bound != nullptr && bound->type == exec::Type::String,
              "MIN over strings keeps the string type");

        const sql::Expr* max_name = item_expr("MAX(name)");
        check(exec::bind_group_expr(*max_name, schema, group, bound, error) &&
                  bound != nullptr && bound->type == exec::Type::String,
              "MAX over strings keeps the string type");

        const sql::Expr* sum_mixed = item_expr("SUM(age + 0.5)");
        check(exec::bind_group_expr(*sum_mixed, schema, group, bound, error) &&
                  bound != nullptr && bound->type == exec::Type::Double &&
                  group.aggregates.back().arg != nullptr &&
                  group.aggregates.back().arg->type == exec::Type::Double,
              "SUM over a double expression is double, argument bound");

        const sql::Expr* sum_name = item_expr("SUM(name)");
        std::string sum_error;
        check(!exec::bind_group_expr(*sum_name, schema, group, bound,
                                     sum_error) &&
                  contains(sum_error, "SUM"),
              "SUM over strings is a named bind error");
        const sql::Expr* avg_city = item_expr("AVG(city)");
        std::string avg_error;
        check(!exec::bind_group_expr(*avg_city, schema, group, bound,
                                     avg_error) &&
                  contains(avg_error, "AVG"),
              "AVG over strings is a named bind error");

        // inventory dedup: the same call spelled twice shares one slot
        exec::GroupBinding dedup = city_grouping();
        sql::ParseResult trio;
        std::string trio_error;
        const bool parsed_trio =
            sql::parse("SELECT COUNT(*) FROM person; "
                       "SELECT count(*) FROM person; "
                       "SELECT COUNT(pid) FROM person",
                       trio, trio_error) &&
            trio.statements.size() == 3;
        check(parsed_trio, "the dedup statements parse");
        if (parsed_trio) {
            const auto* spelled_upper = static_cast<const sql::SelectStatement*>(
                trio.statements[0].get());
            const auto* spelled_lower = static_cast<const sql::SelectStatement*>(
                trio.statements[1].get());
            const auto* other_column = static_cast<const sql::SelectStatement*>(
                trio.statements[2].get());
            exec::BoundExprPtr first;
            exec::BoundExprPtr second;
            exec::BoundExprPtr third;
            const bool bound_first = exec::bind_group_expr(
                *spelled_upper->items[0].expr, schema, dedup, first, error);
            const bool bound_second = exec::bind_group_expr(
                *spelled_lower->items[0].expr, schema, dedup, second, error);
            const bool bound_third = exec::bind_group_expr(
                *other_column->items[0].expr, schema, dedup, third, error);
            check(bound_first && bound_second && bound_third &&
                      dedup.aggregates.size() == 2 &&
                      first->aggregate_index == second->aggregate_index &&
                      third->aggregate_index == 1,
                  "identical aggregates share one slot, distinct ones take the next");
            check(exec::bound_equal(*first, *second) &&
                      !exec::bound_equal(*first, *third),
                  "bound_equal separates aggregates by identity");
        }
    }

    // --- binder: aggregate placement refusals ---
    {
        exec::GroupBinding group = city_grouping();
        exec::BoundExprPtr bound;
        std::string error;

        const sql::Expr* aggregate_in_where = where_expr("COUNT(*) > 1");
        check(!exec::bind_expr(*aggregate_in_where, schema, bound, error) &&
                  contains(error, "WHERE"),
              "an aggregate in WHERE is refused by name");

        const sql::Expr* nested = item_expr("SUM(COUNT(*))");
        check(!exec::bind_group_expr(*nested, schema, group, bound, error) &&
                  contains(error, "nest"),
              "a nested aggregate is refused by name");

        const sql::Expr* stray_column = item_expr("age");
        check(!exec::bind_group_expr(*stray_column, schema, group, bound,
                                     error) &&
                  contains(error, "must appear in GROUP BY"),
              "a bare non-key column in group context is refused by name");

        const sql::Expr* key_column = item_expr("city");
        check(exec::bind_group_expr(*key_column, schema, group, bound, error) &&
                  bound != nullptr && bound->kind == exec::BoundExpr::Kind::Column &&
                  bound->column_index == 3,
              "the grouped column binds in group context");

        const sql::Expr* having_tree =
            item_expr("COUNT(*) > 2 AND city = 'PR'");
        check(exec::bind_group_expr(*having_tree, schema, group, bound,
                                    error) &&
                  bound != nullptr && bound->type == exec::Type::Bool,
              "a HAVING-shaped tree over aggregates and keys binds");

        // COUNT(DISTINCT x) is refused at parse time, by name
        sql::ParseResult parsed;
        std::string parse_error;
        check(!sql::parse("SELECT COUNT(DISTINCT pid) FROM person", parsed,
                          parse_error) &&
                  contains(parse_error, "DISTINCT"),
              "COUNT(DISTINCT ...) is refused by name at parse time");
    }

    // --- the static hash table: routing, chains, fold, drain ---
    {
        // The ex14 hash convention, unit-checked.
        check(group::hash_group_key({exec::Value(4)}) == 4u &&
                  group::hash_group_key({exec::Value(20)}) == 20u,
              "a lone integer key hashes to its own value");
        check(group::hash_group_key({exec::Value(1)}) !=
                  group::hash_group_key({exec::Value(std::string("1"))}),
              "the integer 1 and the string \"1\" hash differently");
        check(group::hash_group_key({exec::Value(1), exec::Value(std::string("a"))}) ==
                  group::hash_group_key({exec::Value(1), exec::Value(std::string("a"))}) &&
              group::hash_group_key({exec::Value(1), exec::Value(std::string("a"))}) !=
                  group::hash_group_key({exec::Value(1), exec::Value(std::string("b"))}),
              "multi-column keys hash deterministically and distinctly");

        // Hand-computed placement with N = 8: keys 4, 12, 20 route to
        // bucket 4 (three-deep chain), keys 1 and 9 to bucket 1. Fold
        // order 4, 12, 20, 1, 9 — drain order is bucket order: 1, 9,
        // then 4, 12, 20.
        std::vector<group::AggregateSpec> specs(1);
        specs[0].fn = sql::FunctionCode::Count;
        specs[0].takes_arg = false;
        group::GroupTable table(specs, 8, 100);
        std::string table_error;
        const int fold_order[] = {4, 12, 20, 1, 9};
        bool folded = true;
        for (int key : fold_order) {
            folded = folded && table.fold({exec::Value(key)}, {}, table_error);
        }
        check(folded && table.entry_count() == 5,
              "five distinct keys fold into five entries");
        std::vector<group::GroupEntry> drained = table.take_all();
        bool order_ok = drained.size() == 5;
        if (order_ok) {
            const int expected_order[] = {1, 9, 4, 12, 20};
            for (std::size_t i = 0; i < 5; ++i) {
                order_ok = order_ok &&
                           std::get<int>(drained[i].key[0]) == expected_order[i];
            }
        }
        check(order_ok,
              "routing is h(key) mod N: drain order proves the buckets");
        check(table.entry_count() == 0, "draining empties the table");

        // A repeated key folds into the same entry (no duplicates).
        group::GroupTable merge(specs, 8, 100);
        check(merge.fold({exec::Value(4)}, {}, table_error) &&
                  merge.fold({exec::Value(4)}, {}, table_error) &&
                  merge.entry_count() == 1,
              "a repeated key folds into one entry");

        // The budget is the spill signal, named.
        group::GroupTable tiny(specs, 8, 2);
        std::string budget_error;
        check(tiny.fold({exec::Value(1)}, {}, budget_error) &&
                  tiny.fold({exec::Value(2)}, {}, budget_error) &&
                  !tiny.fold({exec::Value(3)}, {}, budget_error) &&
                  contains(budget_error, "budget"),
              "exhausting the entry budget is a named error");
    }

    // --- fold and drain: typed aggregate values ---
    {
        // Slots: COUNT(*), SUM(int), AVG(int), MIN(string), MAX(string),
        // SUM(double) — the full typing surface.
        std::vector<group::AggregateSpec> specs(6);
        specs[0].fn = sql::FunctionCode::Count;
        specs[0].takes_arg = false;
        specs[1].fn = sql::FunctionCode::Sum;
        specs[1].result_type = exec::Type::Int;
        specs[1].arg_type = exec::Type::Int;
        specs[1].takes_arg = true;
        specs[2].fn = sql::FunctionCode::Avg;
        specs[2].result_type = exec::Type::Double;
        specs[2].arg_type = exec::Type::Int;
        specs[2].takes_arg = true;
        specs[3].fn = sql::FunctionCode::Min;
        specs[3].result_type = exec::Type::String;
        specs[3].arg_type = exec::Type::String;
        specs[3].takes_arg = true;
        specs[4].fn = sql::FunctionCode::Max;
        specs[4].result_type = exec::Type::String;
        specs[4].arg_type = exec::Type::String;
        specs[4].takes_arg = true;
        specs[5].fn = sql::FunctionCode::Sum;
        specs[5].result_type = exec::Type::Double;
        specs[5].arg_type = exec::Type::Double;
        specs[5].takes_arg = true;

        group::GroupTable table(specs, 8, 100);
        std::string error;
        // Two rows in group 1, two in group 2, one in group 3.
        const int ages[] = {30, 40, 25, 35, 28};
        const std::string names[] = {"Ann", "Bob", "Cid", "Dee", "Eve"};
        const int group_of[] = {1, 1, 2, 2, 3};
        const double weights[] = {0.5, 1.5, 2.5, 3.5, 4.5};
        for (int i = 0; i < 5; ++i) {
            std::vector<exec::Value> args(6);
            args[1] = exec::Value(ages[i]);
            args[2] = exec::Value(ages[i]);
            args[3] = exec::Value(names[i]);
            args[4] = exec::Value(names[i]);
            args[5] = exec::Value(weights[i]);
            check(table.fold({exec::Value(group_of[i])}, args, error),
                  "row folds");
        }
        check(table.entry_count() == 3, "three groups formed");

        std::vector<group::GroupEntry> groups = table.take_all();
        check(groups.size() == 3, "drain hands back all three groups");
        for (const group::GroupEntry& entry : groups) {
            const int key = std::get<int>(entry.key[0]);
            const int count =
                std::get<int>(group::final_value(specs[0], entry.states[0]));
            const int sum =
                std::get<int>(group::final_value(specs[1], entry.states[1]));
            const double avg =
                std::get<double>(group::final_value(specs[2], entry.states[2]));
            const std::string min_name =
                std::get<std::string>(group::final_value(specs[3], entry.states[3]));
            const std::string max_name =
                std::get<std::string>(group::final_value(specs[4], entry.states[4]));
            const double weight_sum =
                std::get<double>(group::final_value(specs[5], entry.states[5]));
            if (key == 1) {
                check(count == 2 && sum == 70 && avg == 35.0 &&
                          min_name == "Ann" && max_name == "Bob" &&
                          weight_sum == 2.0,
                      "group 1: COUNT 2, SUM 70, AVG 35.0, MIN Ann, MAX Bob, "
                      "SUM(double) 2.0");
            } else if (key == 2) {
                check(count == 2 && sum == 60 && avg == 30.0 &&
                          min_name == "Cid" && max_name == "Dee" &&
                          weight_sum == 6.0,
                      "group 2: the same typed shapes over its rows");
            } else {
                check(count == 1 && sum == 28 && avg == 28.0 &&
                          min_name == "Eve" && max_name == "Eve" &&
                          weight_sum == 4.5,
                      "group 3: a single-row group");
            }
        }

        // Empty-group identities over pristine states.
        std::vector<group::AggregateState> empty_states(6);
        check(std::get<int>(group::final_value(specs[0], empty_states[0])) == 0 &&
                  std::get<int>(group::final_value(specs[1], empty_states[1])) == 0 &&
                  std::get<double>(group::final_value(specs[2], empty_states[2])) == 0.0 &&
                  std::get<std::string>(group::final_value(specs[3], empty_states[3])).empty() &&
                  std::get<std::string>(group::final_value(specs[4], empty_states[4])).empty() &&
                  std::get<double>(group::final_value(specs[5], empty_states[5])) == 0.0,
              "empty-group identities: 0, 0, 0.0, \"\", \"\", 0.0");
    }

    // --- parameter invariance: the answer set does not depend on N ---
    {
        std::vector<group::AggregateSpec> specs(2);
        specs[0].fn = sql::FunctionCode::Count;
        specs[0].takes_arg = false;
        specs[1].fn = sql::FunctionCode::Sum;
        specs[1].result_type = exec::Type::Int;
        specs[1].arg_type = exec::Type::Int;
        specs[1].takes_arg = true;

        // Rows: keys 0..29, five rows each, arg = key.
        std::vector<std::pair<int, int>> rows;
        for (int key = 0; key < 30; ++key) {
            for (int i = 0; i < 5; ++i) {
                rows.emplace_back(key, key);
            }
        }

        std::vector<std::vector<std::pair<int, int>>> answers;
        for (const std::size_t buckets : {std::size_t{1}, std::size_t{3},
                                          std::size_t{8}, std::size_t{17}}) {
            group::GroupTable table(specs, buckets, 1000);
            std::string error;
            for (const auto& [key, arg] : rows) {
                if (!table.fold({exec::Value(key)},
                                {exec::Value(0), exec::Value(arg)}, error)) {
                    break;
                }
            }
            std::vector<std::pair<int, int>> answer;
            for (group::GroupEntry& entry : table.take_all()) {
                answer.emplace_back(
                    std::get<int>(entry.key[0]),
                    std::get<int>(group::final_value(specs[1], entry.states[1])));
            }
            std::sort(answer.begin(), answer.end());
            answers.push_back(std::move(answer));
        }
        bool invariant = answers.size() == 4;
        for (std::size_t i = 1; i < answers.size(); ++i) {
            invariant = invariant && answers[i] == answers[0];
        }
        check(invariant && answers[0].size() == 30,
              "bucket counts 1, 3, 8, 17 give identical answer sets "
              "(150 rows, 30 groups, SUM = 5·key)");

        // The budget does not change answers while it holds.
        group::GroupTable tight(specs, 8, 30);
        std::string error;
        bool fits = true;
        for (const auto& [key, arg] : rows) {
            fits = fits && tight.fold({exec::Value(key)},
                                      {exec::Value(0), exec::Value(arg)},
                                      error);
        }
        check(fits && tight.entry_count() == 30,
              "a budget exactly at the group count holds");
    }

    // --- the partition runs: engine-equivalent heap files (D7) ---
    {
        // The mini-schema: key int, then a string arg and a double arg.
        const schema::TableSchema mini = group::run_schema(
            "spillt",
            {exec::Type::Int, exec::Type::String, exec::Type::Double}, 10);
        check(mini.column_count() == 3 &&
                  mini.column_info(1).column_type() == tuple::ColumnType::String &&
                  mini.column_info(1).column_size() == 10,
              "run_schema synthesizes the typed mini-schema");
        const std::string path = "gspill-probe.tmp";
        group::PartitionRunWriter writer;
        std::string error;
        check(writer.open(path, mini, error),
              "the run writer opens a fresh file");
        const std::vector<std::vector<exec::Value>> rows = {
            {exec::Value(1), exec::Value(std::string("Ann")),
             exec::Value(0.5)},
            {exec::Value(2), exec::Value(std::string("Bo")),
             exec::Value(1.5)},
            {exec::Value(3), exec::Value(std::string("Cid")),
             exec::Value(2.5)},
        };
        bool wrote = true;
        for (const std::vector<exec::Value>& row : rows) {
            wrote = wrote && writer.append(row, error);
        }
        check(wrote && writer.close(error),
              "three records append and the run closes");

        group::PartitionRunReader reader;
        check(reader.open(path, mini, error), "the run reader opens the file");
        bool same = true;
        std::vector<exec::Value> record;
        std::size_t count = 0;
        while (reader.next(record, error)) {
            same = same && count < rows.size() && record == rows[count];
            ++count;
        }
        check(same && error.empty() && count == 3,
              "the run scans back record-for-record");

        // Engine equivalence: with the four identification bytes patched
        // to the heap magic, the run IS a heap file for the mini-schema —
        // the loader's tool-vs-insert twin test, reapplied.
        {
            std::ifstream in(path, std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
            in.close();
            const std::uint32_t heap_magic = 0x31505554u;  // "TUP1"
            std::memcpy(bytes.data(), &heap_magic, sizeof(heap_magic));
            const std::string twin = path + ".twin.tbl";
            std::ofstream out(twin, std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            out.close();
            bufman::BufferManager manager;
            manager.init(8, std::make_unique<bufman::LRUPolicy>());
            heapfile::HeapFile heap(manager);
            check(heap.open(twin, mini, error),
                  "the patched run opens as a HeapFile");
            tuple::Tuple expected;
            expected.add(2);
            expected.add(std::string("Bo"));
            expected.add(1.5);
            heapfile::HeapFileIterator it = heap.scan();
            tuple::Tuple tuple;
            std::string scan_error;
            std::size_t scanned = 0;
            bool middle_ok = false;
            while (it.next(tuple, scan_error)) {
                if (scanned == 1) {
                    middle_ok = tuple == expected;
                }
                ++scanned;
            }
            check(scan_error.empty() && scanned == 3 && middle_ok,
                  "the heap view scans back the same three records");
            std::remove(twin.c_str());
        }
        std::remove(path.c_str());
    }

    // --- restart-on-overflow: the same answers through the spill path ---
    {
        // The sandbox: a fresh temp working directory, like the exec
        // suite; restored on exit.
        char pattern[] = "/tmp/ex15-group-XXXXXX";
        char* made = mkdtemp(pattern);
        if (made == nullptr) {
            check(false, "group spill sandbox could not be created");
        } else {
            const std::string original = [] {
                std::vector<char> buffer;
                buffer.resize(4096);
                return std::string(getcwd(buffer.data(), buffer.size()));
            }();
            chdir(made);
            if (system("mkdir -p tablecatalog data") != 0) {
                check(false, "group spill sandbox catalog dir");
            }

            bufman::BufferManager manager;
            manager.init(16, std::make_unique<bufman::LRUPolicy>());
            exec::Executor executor(manager);
            exec::StatementResult result;
            std::string error;

            check(run_sql(executor, "CREATE TABLE spillt (k INTEGER, v INT)",
                          result, error),
                  "the spill table is created");
            std::string insert = "INSERT INTO spillt VALUES ";
            for (int key = 0; key < 50; ++key) {
                for (int i = 0; i < 5; ++i) {
                    insert += (key > 0 || i > 0 ? ", " : "");
                    insert += "(" + std::to_string(key) + ", " +
                              std::to_string(key) + ")";
                }
            }
            check(run_sql(executor, insert, result, error) &&
                      result.message == "inserted 250 row(s)",
                  "the spill table's 250 rows are inserted");

            const std::string query =
                "SELECT k, COUNT(*), SUM(v) FROM spillt GROUP BY k";
            const auto answer_of = [&](exec::Executor& run_executor,
                                       std::vector<std::pair<int, int>>& answer,
                                       std::string& announce) {
                if (!run_sql(run_executor, query, result, error)) {
                    return false;
                }
                announce = result.rows.access_path;
                for (const std::vector<exec::Value>& row : result.rows.rows) {
                    answer.emplace_back(std::get<int>(row[0]),
                                        std::get<int>(row[2]));
                }
                std::sort(answer.begin(), answer.end());
                return true;
            };

            // The in-memory answer: 50 groups, key k sums 5·k.
            std::vector<std::pair<int, int>> memory_answer;
            std::string memory_announce;
            const bool memory_ok =
                answer_of(executor, memory_answer, memory_announce);
            check(memory_ok && memory_answer.size() == 50 &&
                      memory_answer[7] == std::make_pair(7, 35) &&
                      !contains(memory_announce, "spilled"),
                  "the in-memory path answers 50 groups unspilled");

            // Forced spill: a tiny budget restarts into 8 runs; the
            // answers must match and no run file may leak.
            executor.set_group_budget(3);
            std::vector<std::pair<int, int>> spilled_answer;
            std::string spilled_announce;
            const bool spilled_ok =
                answer_of(executor, spilled_answer, spilled_announce);
            check(spilled_ok && spilled_answer == memory_answer,
                  "the spilled path answers identically (budget 3, 8 runs)");
            check(contains(spilled_announce, "(spilled, 8 partitions)"),
                  "the spilled announcement names the partition count");
            check(count_spill_files() == 0,
                  "no run files survive the spilled run");

            // Multi-level recursion: 2 partitions split one bit per
            // level, so 50 keys over a budget of 2 pushes the recursion
            // several levels deep.
            executor.set_group_partitions(2);
            std::vector<std::pair<int, int>> deep_answer;
            std::string deep_announce;
            const bool deep_ok = answer_of(executor, deep_answer, deep_announce);
            check(deep_ok && deep_answer == memory_answer,
                  "multi-level spilling answers identically (2 runs, "
                  "budget 2)");
            check(contains(deep_announce, "(spilled, 2 partitions)"),
                  "the multi-level announcement names its fan-out");
            check(count_spill_files() == 0,
                  "no run files survive the multi-level run");

            // Restore the production defaults.
            executor.set_group_budget(100000);
            executor.set_group_partitions(8);
            check(chdir(original.c_str()) == 0, "sandbox cwd restored");
        }
    }

    // --- the differential oracle: memory vs spill vs brute force ---
    {
        // A fresh sandbox with 300 random rows over 12 group keys.
        char pattern[] = "/tmp/ex15-oracle-XXXXXX";
        char* made = mkdtemp(pattern);
        if (made == nullptr) {
            check(false, "oracle sandbox could not be created");
        } else {
            const std::string original = [] {
                std::vector<char> buffer;
                buffer.resize(4096);
                return std::string(getcwd(buffer.data(), buffer.size()));
            }();
            chdir(made);
            if (system("mkdir -p tablecatalog data") != 0) {
                check(false, "oracle sandbox catalog dir");
            }

            bufman::BufferManager manager;
            manager.init(16, std::make_unique<bufman::LRUPolicy>());
            exec::Executor executor(manager);
            exec::StatementResult result;
            std::string error;

            check(run_sql(executor,
                          "CREATE TABLE oraclet (g INTEGER, v INT, "
                          "w DOUBLE, s VARCHAR(8))",
                          result, error),
                  "the oracle table is created");
            std::mt19937 rng(20260515u);  // fixed seed: deterministic suite
            std::uniform_int_distribution<int> group_roll(0, 11);
            std::uniform_int_distribution<int> value_roll(0, 49);
            std::string insert = "INSERT INTO oraclet VALUES ";
            std::vector<std::pair<int, int>> rows;  // (g, v)
            for (int i = 0; i < 300; ++i) {
                const int g = group_roll(rng);
                const int v = value_roll(rng);
                rows.emplace_back(g, v);
                const double w = v / 4.0;
                insert += (i > 0 ? ", " : "");
                insert += "(" + std::to_string(g) + ", " +
                          std::to_string(v) + ", " + std::to_string(w) +
                          ", 'r" + std::to_string(g % 4) + "')";
            }
            check(run_sql(executor, insert, result, error) &&
                      result.message == "inserted 300 row(s)",
                  "the oracle table's 300 random rows are inserted");

            const auto engine_answer_of = [&](const std::string& query)
                -> std::vector<std::vector<exec::Value>> {
                std::vector<std::vector<exec::Value>> answer;
                if (!run_sql(executor, query, result, error)) {
                    answer.push_back({exec::Value(-999)});  // failure marker
                    return answer;
                }
                for (const std::vector<exec::Value>& row : result.rows.rows) {
                    answer.push_back(row);
                }
                std::sort(answer.begin(), answer.end());
                return answer;
            };

            const std::string base_query =
                "SELECT g, COUNT(*), SUM(v), MIN(v), MAX(v) FROM oraclet";
            const std::string where_query =
                "SELECT g, COUNT(*), SUM(v), MIN(v), MAX(v) FROM oraclet "
                "WHERE v > 10";
            const std::string having_query =
                "SELECT g, COUNT(*), SUM(v), MIN(v), MAX(v) FROM oraclet "
                "GROUP BY g HAVING COUNT(*) > 10";

            // The brute-force reference: no hashing anywhere — a map
            // from group key to accumulated aggregates over the inserted
            // rows (optionally WHERE-filtered), visited in sorted key
            // order. HAVING is applied as a minimum group count.
            const auto clean_reference_of =
                [&](bool where_v_gt_10,
                    std::size_t min_count) -> std::vector<std::vector<exec::Value>> {
                struct State {
                    int count = 0;
                    int sum = 0;
                    int min_v = -1;
                    int max_v = -1;
                };
                std::map<int, State> groups;
                for (const auto& [g, v] : rows) {
                    if (where_v_gt_10 && v <= 10) {
                        continue;
                    }
                    State& state = groups[g];
                    state.count += 1;
                    state.sum += v;
                    state.min_v = state.count == 1 ? v : std::min(state.min_v, v);
                    state.max_v = std::max(state.max_v, v);
                }
                std::vector<std::vector<exec::Value>> answer;
                for (const auto& [g, state] : groups) {
                    if (static_cast<std::size_t>(state.count) < min_count) {
                        continue;
                    }
                    answer.push_back({exec::Value(g), exec::Value(state.count),
                                      exec::Value(state.sum),
                                      exec::Value(state.min_v),
                                      exec::Value(state.max_v)});
                }
                return answer;
            };

            bool all_match = true;
            std::string first_mismatch;
            const auto expect = [&](const std::string& query, bool where,
                                    std::size_t min_count, std::size_t buckets,
                                    std::size_t partitions,
                                    std::size_t budget) {
                executor.set_group_buckets(buckets);
                executor.set_group_partitions(partitions);
                executor.set_group_budget(budget);
                const std::vector<std::vector<exec::Value>> want =
                    clean_reference_of(where, min_count);
                const std::vector<std::vector<exec::Value>> got =
                    engine_answer_of(query);
                if (got != want && all_match) {
                    all_match = false;
                    first_mismatch = "N=" + std::to_string(buckets) +
                                     " K=" + std::to_string(partitions) +
                                     " budget=" + std::to_string(budget);
                }
            };

            // Varied N, K, budget — including forced spills at several
            // depths — against the same brute-force answers.
            expect(base_query + " GROUP BY g", false, 0, 8, 8, 100000);
            expect(base_query + " GROUP BY g", false, 0, 1, 8, 100000);
            expect(base_query + " GROUP BY g", false, 0, 23, 2, 100000);
            expect(base_query + " GROUP BY g", false, 0, 8, 8, 5);
            expect(base_query + " GROUP BY g", false, 0, 3, 2, 2);
            expect(where_query + " GROUP BY g", true, 0, 8, 8, 100000);
            expect(where_query + " GROUP BY g", true, 0, 8, 8, 3);
            expect(where_query + " GROUP BY g", true, 0, 5, 4, 1);
            expect(having_query, false, 11, 8, 8, 100000);
            expect(having_query, false, 11, 8, 8, 4);
            executor.set_group_buckets(8);
            executor.set_group_partitions(8);
            executor.set_group_budget(100000);

            check(all_match,
                  "in-memory, spilled, and parameter-varied runs all match "
                  "the brute-force reference" +
                      (all_match ? std::string() : " (first mismatch: " +
                                                       first_mismatch + ")"));
            check(count_spill_files() == 0, "the oracle left no run files");

            check(chdir(original.c_str()) == 0, "oracle cwd restored");
        }
    }

    std::cout << (failures == 0
                      ? "group suite: all checks passed."
                      : "group suite: " + std::to_string(failures) +
                            " check(s) FAILED.")
              << "\n";
    return failures;
}
