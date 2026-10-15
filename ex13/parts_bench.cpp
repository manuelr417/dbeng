// parts_bench — the parts-index-benchmark executable: one run measures
// the engine's search cost over generated parts rows, first on the heap
// path (no index), then on the index path, and prints the comparison.
//
// The run sequence: argument parsing (record count, default 1,000,000),
// clean slate (drops tolerating absence), CREATE TABLE, deterministic
// generation with a serializer round-trip self-check, load through the
// programmatic insert path, timed heap-scan baselines, one ISAM index
// per searched column with timed builds, timed index-scan re-runs, a
// result-set equality check between the two paths, and a plain-text
// comparison report. The engine is driven exactly as check_exec.cpp
// drives it: one shared BufferManager, one Executor, no engine changes.

#include "bufpool/BufferManager.h"
#include "exec/Executor.h"
#include "policy/LRUPolicy.h"
#include "schema/TableSchema.h"
#include "schema/TupleSerializer.h"
#include "sql/Parser.h"
#include "tools/BulkLoader.h"
#include "tuple/Tuple.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kDefaultRows = 1000000;
constexpr std::size_t kMaxRows = 9999999;
constexpr std::size_t kPoolFrames = 8;
constexpr std::uint32_t kSeed = 42;
const char* const kTable = "parts";
const char* const kDataFile = "data/parts.tbl";
const char* const kPartidIndex = "parts_partid";
const char* const kPpriceIndex = "parts_pprice";
// Q1's lookup key: the spec'd 54645 once the table is large enough to
// contain it, otherwise the largest generated partid — either way the
// equality query answers exactly one row.
constexpr std::size_t kLookupKey = 54645;

const char* const kColors[] = {"red", "blue",   "white", "beige",
                               "green", "black", "yellow"};
const char* const kMaterials[] = {"steel", "aluminum", "copper",
                                  "plastic", "rubber", "brass"};

struct Config {
    std::size_t rows = kDefaultRows;
    bool reuse = false;
};

struct QueryTiming {
    double ms = 0.0;
    std::string access_path;
    std::size_t row_count = 0;
};

struct Measurements {
    double load_ms = 0.0;
    double build_partid_ms = 0.0;
    double build_pprice_ms = 0.0;
    QueryTiming q1_heap;
    QueryTiming q1_index;
    QueryTiming q2_heap;
    QueryTiming q2_index;
    bool q1_agrees = false;
    bool q2_agrees = false;
    std::size_t q2_expected = 0;
    std::uint64_t fingerprint = 0;
    std::string q1_text;
    std::string q2_text;
};

double now_ms(std::chrono::steady_clock::time_point start) {
    const auto stop = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(stop - start).count();
}

// --- argument parsing ---------------------------------------------------------

// --rows N / --rows=N (default kDefaultRows) and the --reuse flag.
// Validation happens before anything else runs: non-numeric, <= 0, or
// beyond kMaxRows fails with a named error (the ceiling keeps every
// generated pname inside VARCHAR(10)); --rows and --reuse together fail
// (a reuse run's count is the existing data's, not an argument); unknown
// arguments fail the same way.
bool parse_args(int argc, char** argv, Config& config, std::string& error) {
    bool rows_seen = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--reuse") {
            config.reuse = true;
            continue;
        }
        std::string value;
        if (arg == "--rows") {
            if (i + 1 >= argc) {
                error = "--rows needs a value";
                return false;
            }
            value = argv[++i];
        } else if (arg.rfind("--rows=", 0) == 0) {
            value = arg.substr(std::string("--rows=").size());
        } else {
            error = "unknown argument '" + arg +
                    "' (expected --rows N or --reuse)";
            return false;
        }
        if (value.empty() ||
            value.find_first_not_of("0123456789") != std::string::npos) {
            error = "--rows must be a positive integer, got '" + value + "'";
            return false;
        }
        const std::size_t parsed = std::strtoull(value.c_str(), nullptr, 10);
        if (parsed < 1 || parsed > kMaxRows) {
            error = "--rows must be between 1 and " +
                    std::to_string(kMaxRows) + ", got " + value;
            return false;
        }
        config.rows = parsed;
        rows_seen = true;
    }
    if (config.reuse && rows_seen) {
        error = "--reuse and --rows cannot be combined: a reuse run's "
                "record count is the existing data's";
        return false;
    }
    return true;
}

// --- statement plumbing -------------------------------------------------------

// Executes one statement's text through the shared executor. SELECT
// fills `result`; the caller times around this for the measured steps.
bool run_sql(exec::Executor& executor, const std::string& text,
             exec::StatementResult& result, std::string& error) {
    sql::ParseResult parsed;
    if (!sql::parse(text, parsed, error)) {
        return false;
    }
    if (parsed.statements.size() != 1) {
        error = "expected exactly one statement, got " +
                std::to_string(parsed.statements.size());
        return false;
    }
    return executor.execute(*parsed.statements.front(), result, error);
}

// The clean-slate phase, split by mode: the index drops always run (a
// genuine heap-scan baseline needs no indexes, and their absence is
// tolerated), while DROP TABLE runs only in default mode — a reuse run
// keeps the catalog and data file.
void clean_slate(exec::Executor& executor, bool keep_table) {
    std::vector<std::string> drops = {
        std::string("DROP INDEX ") + kPartidIndex + ";",
        std::string("DROP INDEX ") + kPpriceIndex + ";",
    };
    if (!keep_table) {
        drops.push_back(std::string("DROP TABLE ") + kTable + ";");
    }
    for (const std::string& text : drops) {
        exec::StatementResult result;
        std::string error;
        if (run_sql(executor, text, result, error)) {
            std::cout << "clean slate: " << result.message << "\n";
        } else {
            std::cout << "clean slate: absent (" << error << ")\n";
        }
    }
}

// --- the parts generator ------------------------------------------------------

// "p" + the row number, zero-padded to 6 digits: 7 characters through
// 999,999 and 8 beyond, so pname stays inside VARCHAR(10) for every
// allowed record count.
std::string part_name(std::size_t row_number) {
    std::string digits = std::to_string(row_number);
    while (digits.size() < 6) {
        digits.insert(digits.begin(), '0');
    }
    return "p" + digits;
}

double clamped_normal(std::mt19937& engine, std::normal_distribution<>& dist,
                      double lo, double hi) {
    const double value = dist(engine);
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

// FNV-1a over every row's values: two runs with the same record count
// must print the same fingerprint, which is how determinism is checked
// from outside the process.
std::uint64_t fingerprint(const std::vector<tuple::Tuple>& rows) {
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](const char* bytes, std::size_t length) {
        for (std::size_t i = 0; i < length; ++i) {
            hash ^= static_cast<unsigned char>(bytes[i]);
            hash *= 1099511628211ull;
        }
    };
    for (const tuple::Tuple& tuple : rows) {
        for (std::size_t c = 0; c < tuple.column_count(); ++c) {
            const char column_tag = static_cast<char>('0' + c);
            mix(&column_tag, 1);
            const tuple::Column& column = tuple.column(c);
            switch (column.column_type()) {
                case tuple::ColumnType::Integer: {
                    const int value = std::get<int>(column.column_value());
                    mix(reinterpret_cast<const char*>(&value), sizeof(value));
                    break;
                }
                case tuple::ColumnType::String: {
                    const std::string& value =
                        std::get<std::string>(column.column_value());
                    mix(value.data(), value.size());
                    break;
                }
                case tuple::ColumnType::Double: {
                    const double value =
                        std::get<double>(column.column_value());
                    mix(reinterpret_cast<const char*>(&value), sizeof(value));
                    break;
                }
            }
        }
    }
    return hash;
}

// Deterministic generation (fixed seed): sequential partid, a color from
// the seven-color list, clamped-normal price and weight, reproducible
// strings. Every row is validated as it is built, so a generator
// regression fails the run loudly instead of loading bad data.
std::vector<tuple::Tuple> generate_parts(std::size_t count,
                                         std::string& error) {
    std::mt19937 engine(kSeed);
    std::uniform_int_distribution<std::size_t> color_dist(0, 6);
    std::uniform_int_distribution<std::size_t> material_dist(0, 5);
    std::normal_distribution<double> price_dist(1500.25, 2999.5 / 6.0);
    std::normal_distribution<double> weight_dist(25.5, 49.0 / 6.0);

    std::vector<tuple::Tuple> rows;
    rows.reserve(count);
    for (std::size_t i = 1; i <= count; ++i) {
        const double price =
            clamped_normal(engine, price_dist, 0.5, 3000.0);
        const double weight =
            clamped_normal(engine, weight_dist, 1.0, 50.0);
        const std::string name = part_name(i);
        if (price < 0.5 || price > 3000.0 || weight < 1.0 || weight > 50.0 ||
            name.size() > 10) {
            error = "generated row " + std::to_string(i) +
                    " violates its column bounds";
            return {};
        }
        tuple::Tuple tuple;
        tuple.add(static_cast<int>(i));
        tuple.add(name);
        tuple.add(std::string(kColors[color_dist(engine)]));
        tuple.add(weight);
        tuple.add(price);
        tuple.add(std::string(kMaterials[material_dist(engine)]));
        rows.push_back(std::move(tuple));
    }
    return rows;
}

// The width contract, checked where it bites: a sample of generated rows
// must survive serialize/deserialize against the created schema
// unchanged (first and last row — the last carries the widest pname).
bool round_trip_check(const schema::TableSchema& schema,
                      const std::vector<tuple::Tuple>& rows,
                      std::string& error) {
    schema::TupleSerializer serializer(schema);
    std::vector<char> buffer(serializer.record_size());
    const std::size_t samples[] = {0, rows.size() - 1};
    for (const std::size_t index : samples) {
        tuple::Tuple back;
        if (!serializer.serialize(rows[index], buffer.data(), buffer.size(),
                                  error) ||
            !serializer.deserialize(buffer.data(), buffer.size(), back,
                                    error)) {
            return false;
        }
        if (!(back == rows[index])) {
            error = "round-trip mismatch at row " + std::to_string(index + 1);
            return false;
        }
    }
    return true;
}

// --- result-set plumbing ------------------------------------------------------

// One stable key per row: int/double/string tagged and rendered with
// full double precision, so set equality cannot be fooled by display
// rounding. Rows are sorted before comparison — row order is
// unspecified without ORDER BY.
std::string value_key(const exec::Value& value) {
    if (const int* v = std::get_if<int>(&value)) {
        return "i" + std::to_string(*v);
    }
    if (const double* v = std::get_if<double>(&value)) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.17g", *v);
        return std::string("d") + buffer;
    }
    return "s" + std::get<std::string>(value);
}

std::vector<std::string> row_keys(const exec::ResultSet& rows) {
    std::vector<std::string> keys;
    keys.reserve(rows.rows.size());
    for (const std::vector<exec::Value>& row : rows.rows) {
        std::string key;
        for (std::size_t i = 0; i < row.size(); ++i) {
            if (i != 0) {
                key += "|";
            }
            key += value_key(row[i]);
        }
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

bool sets_agree(const exec::ResultSet& baseline,
                const exec::ResultSet& indexed) {
    return row_keys(baseline) == row_keys(indexed);
}

// The loaded data, spot-checked through the engine against the rows the
// generator produced: the lookup key answers exactly one row, and
// partid 1, the lookup key, and partid N match the generated tuples
// field for field.
bool spot_checks(exec::Executor& executor,
                 const std::vector<tuple::Tuple>& rows, std::size_t lookup_key,
                 exec::ResultSet& lookup_key_result, std::string& error) {
    const std::size_t probes[] = {1, lookup_key, rows.size()};
    for (const std::size_t partid : probes) {
        exec::StatementResult result;
        if (!run_sql(executor,
                     "SELECT * FROM parts WHERE partid = " +
                         std::to_string(partid) + ";",
                     result, error)) {
            return false;
        }
        const tuple::Tuple& expected = rows[partid - 1];
        if (result.rows.rows.size() != 1 ||
            result.rows.rows[0].size() != expected.column_count()) {
            error = "spot check for partid " + std::to_string(partid) +
                    " returned " +
                    std::to_string(result.rows.rows.size()) + " rows";
            return false;
        }
        for (std::size_t c = 0; c < result.rows.rows[0].size(); ++c) {
            tuple::Column fetched(0);
            const exec::Value& value = result.rows.rows[0][c];
            if (const int* v = std::get_if<int>(&value)) {
                fetched = tuple::Column(*v);
            } else if (const double* v = std::get_if<double>(&value)) {
                fetched = tuple::Column(*v);
            } else {
                fetched = tuple::Column(std::get<std::string>(value));
            }
            if (!(fetched == expected.column(c))) {
                error = "spot check for partid " + std::to_string(partid) +
                        " column " + std::to_string(c) + " disagrees";
                return false;
            }
        }
        if (partid == lookup_key) {
            lookup_key_result = std::move(result.rows);
        }
    }
    return true;
}

// Both index files must exist beside the data file, and the session's
// index verification must walk a nonzero entry count per index cleanly.
bool check_index_files(exec::Executor& executor, std::string& error) {
    const std::string files[] = {
        std::string(kDataFile) + ".partid.idx",
        std::string(kDataFile) + ".pprice.idx",
    };
    for (const std::string& file : files) {
        if (!std::filesystem::exists(file)) {
            error = "missing index file " + file;
            return false;
        }
    }
    std::vector<std::string> report;
    bool clean = true;
    if (!executor.check_indexes(kTable, kDataFile, report, clean, error)) {
        return false;
    }
    for (const std::string& line : report) {
        std::cout << "index check: " << line << "\n";
        const std::size_t colon = line.find(": ");
        const std::size_t entries = line.find(" entries");
        if (colon == std::string::npos || entries == std::string::npos) {
            continue;
        }
        const std::size_t count = std::strtoull(
            line.substr(colon + 2, entries - colon - 2).c_str(), nullptr, 10);
        if (count == 0) {
            error = "index walked zero entries: " + line;
            return false;
        }
    }
    if (!clean) {
        error = "index verification reported findings";
        return false;
    }
    return true;
}

void print_report(const Config& config, std::size_t n, const Measurements& m) {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "== parts_bench report ==\n";
    if (config.reuse) {
        std::cout << "data:                   reused (" << n
                  << " rows, load skipped)\n";
    } else {
        std::cout << "rows loaded:            " << n << "  (load "
                  << m.load_ms << " ms)\n";
    }
    std::cout << "data fingerprint:       0x" << std::hex << m.fingerprint
              << std::dec << "\n";
    std::cout << "index build partid:     " << m.build_partid_ms << " ms\n";
    std::cout << "index build pprice:     " << m.build_pprice_ms << " ms\n";
    const QueryTiming* pairs[][2] = {
        {&m.q1_heap, &m.q1_index},
        {&m.q2_heap, &m.q2_index},
    };
    const char* const names[] = {"Q1", "Q2"};
    const std::string* const texts[] = {&m.q1_text, &m.q2_text};
    const bool agreements[] = {m.q1_agrees, m.q2_agrees};
    for (std::size_t q = 0; q < 2; ++q) {
        const QueryTiming& heap = *pairs[q][0];
        const QueryTiming& index = *pairs[q][1];
        std::cout << names[q] << ": " << *texts[q] << "\n";
        std::cout << "  heap scan : " << heap.ms << " ms   rows: "
                  << heap.row_count << "  [" << heap.access_path << "]\n";
        std::cout << "  index scan: " << index.ms << " ms   rows: "
                  << index.row_count << "  [" << index.access_path << "]\n";
        if (index.ms > 0.0) {
            std::cout << "  speedup   : " << heap.ms / index.ms << "x\n";
        } else {
            std::cout << "  speedup   : n/a\n";
        }
        std::cout << "  answers identical on both paths: "
                  << (agreements[q] ? "yes" : "NO") << "\n";
    }
}

// --- the run ------------------------------------------------------------------

bool measure(exec::Executor& executor, const Config& config,
             Measurements& m, std::size_t& n_out, std::size_t& lookup_key,
             std::string& error) {
    // Reuse mode: the data must already be there. The precondition runs
    // before anything else — no measurement, no generation — and N is a
    // fact of the existing table, not an argument.
    std::size_t n = config.rows;
    if (config.reuse) {
        std::vector<schema::TableIndex> existing;
        if (executor.table(kTable, existing, error) == nullptr) {
            error = "--reuse: no existing parts table (" + error + ")";
            return false;
        }
        std::size_t counted = 0;
        if (!executor.count_rows(kTable, kDataFile, counted, error)) {
            error = "--reuse: cannot read the existing parts data (" +
                    error + ")";
            return false;
        }
        if (counted == 0) {
            error = "--reuse: the existing parts table is empty (run once "
                    "without --reuse to create it)";
            return false;
        }
        n = counted;
    } else {
        // Create the table (the requested CHAR(n) columns map to the
        // engine's only string type, VARCHAR(n)).
        exec::StatementResult result;
        if (!run_sql(executor,
                     "CREATE TABLE parts (partid INTEGER, pname VARCHAR(10), "
                     "pcolor VARCHAR(10), pweight DOUBLE, pprice DOUBLE, "
                     "pmaterial VARCHAR(20));",
                     result, error)) {
            return false;
        }
    }
    std::vector<schema::TableIndex> indexes;
    const schema::TableSchema* schema = executor.table(kTable, indexes, error);
    if (schema == nullptr) {
        return false;
    }
    exec::StatementResult result;

    // Generate, fingerprint, and prove the width contract — both modes.
    // In reuse mode this is verification data only: nothing is written.
    const std::vector<tuple::Tuple> rows = generate_parts(n, error);
    if (rows.size() != n) {
        if (error.empty()) {
            error = "generator produced " + std::to_string(rows.size()) +
                    " rows, expected " + std::to_string(n);
        }
        return false;
    }
    m.fingerprint = fingerprint(rows);
    if (!round_trip_check(*schema, rows, error)) {
        return false;
    }

    // Default mode loads through the bulk loader: raw block writes, no
    // buffer manager (the session has the file closed at this point —
    // the bootstrap-before-open contract), timed. Reuse mode skips the
    // load entirely; its spot checks below verify the existing file
    // against the regenerated rows.
    if (!config.reuse) {
        const auto load_start = std::chrono::steady_clock::now();
        tools::BulkLoadStats load_stats;
        const tools::RowGenerator row_source =
            [&rows](std::size_t index, tuple::Tuple& out, std::string&) {
                out = rows[index];
                return true;
            };
        if (!tools::bulk_load(kDataFile, *schema, n, row_source, load_stats,
                              error)) {
            return false;
        }
        m.load_ms = now_ms(load_start);
        std::cout << "  bulk-loaded " << load_stats.records << " rows onto "
                  << load_stats.pages << " pages (capacity "
                  << load_stats.per_page_capacity << ")\n";
    }
    std::size_t counted = 0;
    if (!executor.count_rows(kTable, kDataFile, counted, error)) {
        return false;
    }
    if (counted != n) {
        error = "count_rows returned " + std::to_string(counted) +
                ", expected " + std::to_string(n);
        return false;
    }

    // Spot checks against the generated data — in reuse mode these are
    // the verification that the reused file matches the generator.
    lookup_key = n >= kLookupKey ? kLookupKey : n;
    n_out = n;
    exec::ResultSet lookup_key_rows;
    if (!spot_checks(executor, rows, lookup_key, lookup_key_rows, error)) {
        return false;
    }

    // The two benchmark queries, parsed once: the same AST runs on both
    // access paths, so both timers measure identical work.
    const std::string q1_text = "SELECT * FROM parts WHERE partid = " +
                                std::to_string(lookup_key) + ";";
    const std::string q2_text =
        "SELECT * FROM parts WHERE pprice >= 1.0 AND pprice <= 10.0;";
    sql::ParseResult q1;
    sql::ParseResult q2;
    if (!sql::parse(q1_text, q1, error) || !sql::parse(q2_text, q2, error)) {
        return false;
    }
    m.q1_text = q1_text;
    m.q2_text = q2_text;

    // Baselines: no index exists, so both must announce heap scans.
    // Both result sets are retained for the differential check.
    exec::ResultSet q1_baseline;
    exec::ResultSet q2_baseline;
    const auto q1_start = std::chrono::steady_clock::now();
    if (!executor.execute(*q1.statements.front(), result, error)) {
        return false;
    }
    m.q1_heap = {now_ms(q1_start), result.rows.access_path,
                 result.rows.rows.size()};
    q1_baseline = std::move(result.rows);
    result = exec::StatementResult{};
    const auto q2_start = std::chrono::steady_clock::now();
    if (!executor.execute(*q2.statements.front(), result, error)) {
        return false;
    }
    m.q2_heap = {now_ms(q2_start), result.rows.access_path,
                 result.rows.rows.size()};
    q2_baseline = std::move(result.rows);
    result = exec::StatementResult{};
    if (m.q1_heap.access_path != "heap scan" ||
        m.q2_heap.access_path != "heap scan") {
        error = "baseline announced '" + m.q1_heap.access_path + "' / '" +
                m.q2_heap.access_path + "', expected heap scan";
        return false;
    }
    if (m.q1_heap.row_count != 1) {
        error = "Q1 baseline returned " +
                std::to_string(m.q1_heap.row_count) + " rows, expected 1";
        return false;
    }
    for (const tuple::Tuple& tuple : rows) {
        const double price = std::get<double>(tuple.column(4).column_value());
        if (price >= 1.0 && price <= 10.0) {
            ++m.q2_expected;
        }
    }
    if (m.q2_heap.row_count != m.q2_expected) {
        error = "Q2 baseline returned " +
                std::to_string(m.q2_heap.row_count) + " rows, generator says " +
                std::to_string(m.q2_expected);
        return false;
    }

    // Index phase: build on the searched column, re-run the same query.
    const auto build_start = std::chrono::steady_clock::now();
    if (!run_sql(executor,
                 std::string("CREATE INDEX ") + kPartidIndex + " ON " +
                     kTable + " (partid);",
                 result, error)) {
        return false;
    }
    m.build_partid_ms = now_ms(build_start);
    exec::ResultSet q1_indexed;
    const auto q1i_start = std::chrono::steady_clock::now();
    if (!executor.execute(*q1.statements.front(), result, error)) {
        return false;
    }
    m.q1_index = {now_ms(q1i_start), result.rows.access_path,
                  result.rows.rows.size()};
    q1_indexed = std::move(result.rows);
    result = exec::StatementResult{};

    const auto build2_start = std::chrono::steady_clock::now();
    if (!run_sql(executor,
                 std::string("CREATE INDEX ") + kPpriceIndex + " ON " +
                     kTable + " (pprice);",
                 result, error)) {
        return false;
    }
    m.build_pprice_ms = now_ms(build2_start);
    exec::ResultSet q2_indexed;
    const auto q2i_start = std::chrono::steady_clock::now();
    if (!executor.execute(*q2.statements.front(), result, error)) {
        return false;
    }
    m.q2_index = {now_ms(q2i_start), result.rows.access_path,
                  result.rows.rows.size()};
    q2_indexed = std::move(result.rows);
    result = exec::StatementResult{};
    if (m.q1_index.access_path != "index scan" ||
        m.q2_index.access_path != "index scan") {
        error = "indexed run announced '" + m.q1_index.access_path +
                "' / '" + m.q2_index.access_path + "', expected index scan";
        return false;
    }
    if (m.q1_index.row_count != m.q1_heap.row_count ||
        m.q2_index.row_count != m.q2_heap.row_count) {
        error = "indexed run row count differs from baseline";
        return false;
    }

    // The index files must exist, verify cleanly, and be nonempty.
    if (!check_index_files(executor, error)) {
        return false;
    }

    // The differential check: both paths must answer identically (as a
    // set), and a mismatch fails the run by name.
    m.q1_agrees = sets_agree(q1_baseline, q1_indexed);
    m.q2_agrees = sets_agree(q2_baseline, q2_indexed);
    if (!m.q1_agrees) {
        error = "Q1: the index scan answered a different row set than the "
                "heap scan";
        return false;
    }
    if (!m.q2_agrees) {
        error = "Q2: the index scan answered a different row set than the "
                "heap scan";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Config config;
    std::string error;
    if (!parse_args(argc, argv, config, error)) {
        std::cerr << "parts_bench: " << error << "\n";
        return 1;
    }
    std::cout << (config.reuse ? "parts_bench: reusing existing parts data\n"
                               : "parts_bench: loading " +
                                     std::to_string(config.rows) + " rows\n");

    bufman::BufferManager manager;
    manager.init(kPoolFrames, std::make_unique<bufman::LRUPolicy>());
    exec::Executor executor(manager);

    clean_slate(executor, config.reuse);

    Measurements measurements;
    std::size_t lookup_key = 0;
    std::size_t derived_rows = 0;
    if (!measure(executor, config, measurements, derived_rows, lookup_key,
                 error)) {
        std::cerr << "parts_bench: " << error << "\n";
        std::string close_error;
        if (!executor.close_all(close_error)) {
            std::cerr << "parts_bench: close failed: " << close_error << "\n";
        }
        return 1;
    }

    print_report(config, derived_rows, measurements);

    std::string close_error;
    if (!executor.close_all(close_error)) {
        std::cerr << "parts_bench: " << close_error << "\n";
        return 1;
    }
    return 0;
}
