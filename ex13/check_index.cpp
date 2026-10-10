// ex13 — index module checks: the key codec, the three page views, the
// block-0 identification record, bulk build, lookup and range (with
// duplicates spanning leaves), insert with overflow chaining, the verify
// pass, and close/reopen persistence.

#include "bufpool/BufferManager.h"
#include "index/IndexPages.h"
#include "index/IsamIndex.h"
#include "index/KeyCodec.h"
#include "policy/LRUPolicy.h"
#include "schema/TableSchema.h"
#include "schema/TableSchemaJson.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <unistd.h>
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

std::string temp_path(const std::string& tag) {
    return "/tmp/ex13-index-" + std::to_string(getpid()) + "-" + tag + ".idx";
}

schema::TableSchema load_schema(const std::string& json, std::string& error) {
    schema::TableSchema schema("");
    schema::parse_table_schema(json, schema, error);
    return schema;
}

const char* kPersonJson =
    "{\"table_name\":\"person\",\"columns\":["
    "{\"column_name\":\"pid\",\"column_type\":\"Integer\",\"size\":4},"
    "{\"column_name\":\"name\",\"column_type\":\"String\",\"size\":10},"
    "{\"column_name\":\"age\",\"column_type\":\"Integer\",\"size\":4},"
    "{\"column_name\":\"city\",\"column_type\":\"String\",\"size\":3}]}";

std::vector<std::pair<tuple::Column, isam::Rid>> int_entries(int first,
                                                              int last) {
    std::vector<std::pair<tuple::Column, isam::Rid>> entries;
    for (int value = first; value <= last; ++value) {
        entries.emplace_back(tuple::Column(value),
                             isam::Rid{static_cast<std::uint32_t>(value),
                                        static_cast<std::uint32_t>(value)});
    }
    return entries;
}

std::vector<std::pair<tuple::Column, isam::Rid>> repeated_int_entries(
    int value, std::size_t count) {
    std::vector<std::pair<tuple::Column, isam::Rid>> entries;
    for (std::size_t i = 0; i < count; ++i) {
        entries.emplace_back(tuple::Column(value),
                             isam::Rid{static_cast<std::uint32_t>(i),
                                        static_cast<std::uint32_t>(i)});
    }
    return entries;
}

// Encodes one key value and inserts it through the entry-page API.
bool insert_encoded(isam::EntryPage& page, const isam::KeyCodec& codec,
                    const tuple::Column& value, const isam::Rid& rid,
                    std::string& error) {
    std::string bytes(codec.width(), '\0');
    if (!codec.encode(value, bytes.data(), error)) {
        return false;
    }
    return page.insert_entry(isam::DataEntry{bytes, rid}, codec, error);
}

// Reads one block straight from disk through a short-lived file handle,
// so before/after comparisons see flushed bytes, not pooled ones.
std::string disk_block(bufman::BufferManager& manager,
                       const std::string& path, std::uint32_t block,
                       std::string& error) {
    bufman::File file;
    if (!manager.open_file(path, file, error)) {
        return "";
    }
    std::size_t frame = 0;
    if (!manager.pin(file, block, frame, error)) {
        std::string close_error;
        manager.close_file(file, close_error);
        return "";
    }
    std::string bytes(manager.frame(frame).buffer, bufman::kPageSize);
    std::string unpin_error;
    manager.unpin(frame, false, unpin_error);
    std::string close_error;
    manager.close_file(file, close_error);
    return bytes;
}

}

int run_index_checks() {
    std::string error;
    const schema::TableSchema person = load_schema(kPersonJson, error);
    check(error.empty() && person.column_count() == 4,
          "person schema loads for the index checks");
    bufman::BufferManager manager;
    manager.init(16, std::make_unique<bufman::LRUPolicy>());

    // --- 4.1. the key codec ---
    {
        const isam::KeyCodec int_codec(tuple::ColumnType::Integer, 0);
        check(int_codec.width() == 4, "int key width is 4");
        const isam::KeyCodec double_codec(tuple::ColumnType::Double, 0);
        check(double_codec.width() == 8, "double key width is 8");
        const isam::KeyCodec string_codec(tuple::ColumnType::String, 10);
        check(string_codec.width() == 10,
              "string key width is the declared width");

        char bytes[16] = {};
        check(int_codec.encode(tuple::Column(-42), bytes, error) &&
                  int_codec.decode(bytes) == tuple::Column(-42),
              "int key encodes and decodes");
        check(double_codec.encode(tuple::Column(2.5), bytes, error) &&
                  std::get<double>(double_codec.decode(bytes).column_value()) ==
                      2.5,
              "double key encodes and decodes");
        check(string_codec.encode(tuple::Column(std::string("Ruth")), bytes,
                                  error),
              "string key encodes");
        check(std::memcmp(bytes, "Ruth\0\0\0\0\0\0", 10) == 0,
              "string key is NUL-padded to the declared width");
        check(std::get<std::string>(string_codec.decode(bytes).column_value()) ==
                  "Ruth",
              "string key decodes");

        check(!string_codec.encode(tuple::Column(42), bytes, error),
              "a non-string value refuses to encode as a string key");
        check(!string_codec.encode(tuple::Column(std::string("0123456789A")),
                                   bytes, error),
              "a too-wide string refuses to encode as a key");
        check(!int_codec.encode(tuple::Column(2.5), bytes, error),
              "a double refuses to encode as an int key");

        char a[4] = {};
        char b[4] = {};
        const auto encoded_int = [&](int value, char* out) {
            int_codec.encode(tuple::Column(value), out, error);
        };
        encoded_int(-5, a);
        encoded_int(3, b);
        check(int_codec.compare(a, b) < 0,
              "int key order is numeric across zero");
        encoded_int(3, a);
        encoded_int(3, b);
        check(int_codec.compare(a, b) == 0, "equal int keys compare equal");

        char sa[10] = {};
        char sb[10] = {};
        const auto encoded_str = [&](const std::string& value, char* out) {
            string_codec.encode(tuple::Column(value), out, error);
        };
        encoded_str("ab", sa);
        encoded_str("abc", sb);
        check(string_codec.compare(sa, sb) < 0, "shorter prefix sorts first");
        encoded_str("b", sa);
        check(string_codec.compare(sa, sb) > 0, "lexicographic byte order");
        encoded_str("abc", sa);
        check(string_codec.compare(sa, sb) == 0,
              "NUL padding does not disturb equality");
    }

    // --- 4.1. page view capacities and entry machinery ---
    {
        check(isam::leaf_capacity(4) == 340, "int-key leaf capacity is 340");
        check(isam::separator_capacity(4) == 511,
              "int-key separator capacity is 511");
        check(isam::overflow_capacity(4) == 340,
              "int-key overflow capacity is 340");
        check(isam::leaf_capacity(10) == (4096 - 12) / 18,
              "10-byte string key leaf capacity follows the formula");

        static char page[bufman::kPageSize];
        const isam::KeyCodec codec(tuple::ColumnType::Integer, 0);
        isam::LeafPage leaf;
        leaf.bind(page, 4);
        leaf.format(isam::kNoPage, isam::kNoPage);
        check(leaf.count() == 0 && leaf.attach(error),
              "a formatted leaf page attaches");
        const std::vector<int> shuffled = {5, 1, 9, 1, 7, 3};
        bool all_inserted = true;
        for (const int value : shuffled) {
            all_inserted = all_inserted &&
                           insert_encoded(leaf, codec, tuple::Column(value),
                                          isam::Rid{9, 9}, error);
        }
        check(all_inserted && leaf.count() == 6,
              "six out-of-order inserts all land");
        bool sorted = true;
        isam::DataEntry previous_entry = leaf.get_entry(0);
        for (std::size_t i = 1; i < leaf.count(); ++i) {
            const isam::DataEntry entry = leaf.get_entry(i);
            sorted = sorted &&
                     codec.compare(previous_entry.key.data(),
                                   entry.key.data()) <= 0;
            previous_entry = entry;
        }
        check(sorted, "entry inserts keep the page sorted");
        const auto encode_int = [&](int value) {
            std::string bytes4(4, '\0');
            codec.encode(tuple::Column(value), bytes4.data(), error);
            return bytes4;
        };
        check(leaf.lower_bound(encode_int(4), codec) == 3,
              "lower_bound finds the first key >= 4");
        check(leaf.lower_bound(encode_int(0), codec) == 0,
              "lower_bound at the front");
        check(leaf.lower_bound(encode_int(9), codec) == 5,
              "lower_bound on an existing key lands on it");
        check(leaf.lower_bound(encode_int(10), codec) == 6,
              "lower_bound past the end is the count");

        while (leaf.count() < leaf.capacity()) {
            check(leaf.insert_entry(
                      isam::DataEntry{encode_int(2000), isam::Rid{1, 1}},
                      codec, error),
                  "filling the leaf succeeds");
        }
        check(!leaf.insert_entry(
                  isam::DataEntry{encode_int(3000), isam::Rid{1, 1}}, codec,
                  error) &&
                  error.find("full") != std::string::npos,
              "a full leaf refuses another entry by name");

        isam::OverflowPage overflow;
        overflow.bind(page, 4);
        overflow.format(77);
        check(overflow.attach(error) && overflow.next_overflow() == 77 &&
                  overflow.count() == 0,
              "a formatted overflow page carries its chain link");

        isam::SeparatorPage separator;
        separator.bind(page, 4);
        separator.format();
        check(separator.attach(error) && separator.count() == 0,
              "a formatted separator page attaches");
        check(separator.append(encode_int(10).data(), 11, error) &&
                  separator.append(encode_int(20).data(), 21, error) &&
                  separator.append(encode_int(30).data(), 31, error),
              "separator appends succeed");
        separator.set_first_child(1);
        check(separator.first_child() == 1, "first child is settable");
        check(separator.entry(0).child == 11 &&
                  separator.entry(0).key == encode_int(10),
              "entry(0) reads back the first index entry");
        check(separator.entry(2).child == 31 &&
                  separator.entry(2).key == encode_int(30),
              "entry(2) reads back the last index entry");
        check(separator.descend_step(encode_int(5).data(), codec) == 1,
              "below every separator routes to the first child");
        check(separator.descend_step(encode_int(10).data(), codec) == 1,
              "equal to a separator still routes left of it");
        check(separator.descend_step(encode_int(15).data(), codec) == 11,
              "between separators routes past the smaller one");
        check(separator.descend_step(encode_int(35).data(), codec) == 31,
              "past every separator routes to the last child");
        isam::SeparatorPage fresh;
        fresh.bind(page, 4);
        fresh.format();
        check(fresh.first_child() == isam::kNoPage,
              "a formatted separator's first child is the no-page marker");
    }

    // --- 4.2. the header block: identification on open ---
    {
        const std::string path = temp_path("header");
        std::remove(path.c_str());
        isam::IsamIndex idx(manager, person, 0);
        check(idx.create(path, error) && idx.is_open() &&
                  idx.entry_count() == 0,
              "create yields an open, empty index");
        check(idx.close(error) && idx.open(path, error),
              "an index file reopens after close");

        const char* kCarJson =
            "{\"table_name\":\"car\",\"columns\":["
            "{\"column_name\":\"pid\",\"column_type\":\"Integer\",\"size\":4},"
            "{\"column_name\":\"brand\",\"column_type\":\"String\",\"size\":10}]}";
        const schema::TableSchema car = load_schema(kCarJson, error);
        isam::IsamIndex wrong_table(manager, car, 0);
        check(!wrong_table.open(path, error) &&
                  error.find("table 'person'") != std::string::npos,
              "opening under a different table is refused by name");

        // Same key type and width, different column name: the type and
        // width checks pass, so the refusal must come from the name.
        const char* kTwoIntJson =
            "{\"table_name\":\"person\",\"columns\":["
            "{\"column_name\":\"pid\",\"column_type\":\"Integer\",\"size\":4},"
            "{\"column_name\":\"age\",\"column_type\":\"Integer\",\"size\":4}]}";
        const schema::TableSchema two_int = load_schema(kTwoIntJson, error);
        isam::IsamIndex wrong_column(manager, two_int, 1);
        check(!wrong_column.open(path, error) &&
                  error.find("column 'pid'") != std::string::npos,
              "opening under a different column is refused by name");

        const char* kStringPidJson =
            "{\"table_name\":\"person\",\"columns\":["
            "{\"column_name\":\"pid\",\"column_type\":\"String\",\"size\":4},"
            "{\"column_name\":\"name\",\"column_type\":\"String\",\"size\":10}]}";
        const schema::TableSchema string_pid = load_schema(kStringPidJson, error);
        isam::IsamIndex wrong_type(manager, string_pid, 0);
        check(!wrong_type.open(path, error) &&
                  error.find("key type differs") != std::string::npos,
              "opening under a different key type is refused");

        const std::string foreign = temp_path("foreign");
        {
            std::ofstream out(foreign, std::ios::binary | std::ios::trunc);
            // Exactly one block of text: the header block exists, its
            // magic does not.
            out << std::string(4096, 'x');
        }
        isam::IsamIndex foreign_idx(manager, person, 0);
        check(!foreign_idx.open(foreign, error) &&
                  error.find("not an index file") != std::string::npos,
              "a foreign file is refused by magic");
        std::remove(foreign.c_str());
        check(idx.close(error), "header-test index closed");
        std::remove(path.c_str());
    }

    // --- 4.3. bulk build ---
    {
        const std::string path = temp_path("build");
        std::remove(path.c_str());
        isam::IsamIndex idx(manager, person, 0);
        std::vector<std::pair<tuple::Column, isam::Rid>> entries =
            int_entries(1, 1000);
        std::reverse(entries.begin(), entries.end());  // build must sort
        check(idx.build(path, entries, error), "build over 1000 reversed entries");
        check(idx.entry_count() == 1000, "the header reports 1000 entries");
        bool all_found = true;
        for (int value = 1; value <= 1000; ++value) {
            std::vector<isam::Rid> out;
            if (!idx.lookup(tuple::Column(value), out, error) ||
                out.size() != 1 || !(out[0] == isam::Rid{
                                            static_cast<std::uint32_t>(value),
                                            static_cast<std::uint32_t>(value)})) {
                all_found = false;
                break;
            }
        }
        check(all_found, "every built key is found with its exact rid");
        std::vector<isam::Rid> missing;
        check(idx.lookup(tuple::Column(5000), missing, error) &&
                  missing.empty(),
              "a missing key answers empty");
        check(idx.close(error), "build index closed");

        const std::string empty_path = temp_path("empty");
        std::remove(empty_path.c_str());
        isam::IsamIndex empty_idx(manager, person, 0);
        check(empty_idx.build(empty_path, {}, error),
              "building an empty index succeeds");
        check(empty_idx.entry_count() == 0, "the empty index holds no entries");
        std::vector<isam::Rid> none;
        check(empty_idx.lookup(tuple::Column(1), none, error) && none.empty(),
              "lookups on an empty index answer empty");
        check(empty_idx.insert(tuple::Column(42), isam::Rid{1, 1}, error),
              "the first insert grows the empty index's first leaf");
        check(empty_idx.lookup(tuple::Column(42), none, error) &&
                  none.size() == 1,
              "the inserted key is findable");
        check(empty_idx.close(error), "empty index closed");
        std::remove(empty_path.c_str());

        const std::string string_path = temp_path("strings");
        std::remove(string_path.c_str());
        isam::IsamIndex name_idx(manager, person, 1);
        std::vector<std::pair<tuple::Column, isam::Rid>> names;
        for (int i = 0; i < 50; ++i) {
            char text[16];
            std::snprintf(text, sizeof(text), "name%03d", i);
            names.emplace_back(tuple::Column(std::string(text)),
                               isam::Rid{static_cast<std::uint32_t>(i),
                                          static_cast<std::uint32_t>(i)});
        }
        check(name_idx.build(string_path, names, error),
              "a string-key index builds");
        std::vector<isam::Rid> found;
        check(name_idx.lookup(tuple::Column(std::string("name007")), found,
                              error) &&
                  found.size() == 1 && found[0] == isam::Rid{7, 7},
              "a string key is found");
        check(name_idx.close(error), "string index closed");
        std::remove(string_path.c_str());
        std::remove(path.c_str());
    }

    // --- 4.4. duplicates spanning leaves, and ranges ---
    {
        const std::string path = temp_path("dups");
        std::remove(path.c_str());
        isam::IsamIndex idx(manager, person, 0);
        // 400 equal keys: 340 fill the first leaf, 60 spill into the
        // second — duplicates spanning a leaf boundary.
        check(idx.build(path, repeated_int_entries(5, 400), error),
              "a build of 400 equal keys succeeds");
        std::vector<isam::Rid> out;
        check(idx.lookup(tuple::Column(5), out, error) && out.size() == 400,
              "a lookup returns every duplicate across both leaves");
        std::vector<isam::Rid> ranged;
        check(idx.range(isam::RangeBound{tuple::Column(5), true},
                        isam::RangeBound{tuple::Column(5), true}, ranged,
                        error) && ranged.size() == 400,
              "a point range returns every duplicate");

        check(idx.insert(tuple::Column(5), isam::Rid{777, 0}, error),
              "another equal key inserts");
        check(idx.lookup(tuple::Column(5), out, error) && out.size() == 401,
              "the inserted duplicate is found with the others");
        check(idx.close(error), "duplicate index closed");
        std::remove(path.c_str());

        const std::string range_path = temp_path("range");
        std::remove(range_path.c_str());
        isam::IsamIndex ridx(manager, person, 0);
        check(ridx.build(range_path, int_entries(1, 1000), error),
              "range index built");
        const auto range_of = [&](int lo, bool lo_incl, int hi, bool hi_incl) {
            std::vector<isam::Rid> out;
            std::optional<isam::RangeBound> low;
            std::optional<isam::RangeBound> high;
            if (lo >= 0) {
                low = isam::RangeBound{tuple::Column(lo), lo_incl};
            }
            if (hi >= 0) {
                high = isam::RangeBound{tuple::Column(hi), hi_incl};
            }
            ridx.range(low, high, out, error);
            return out;
        };
        {
            const auto out = range_of(10, true, 19, true);
            bool ok = out.size() == 10;
            for (std::size_t i = 0; ok && i < out.size(); ++i) {
                ok = out[i].page_id == 10 + static_cast<int>(i);
            }
            check(ok, "an inclusive range is ascending and complete");
        }
        {
            const auto out = range_of(10, false, 19, true);
            check(out.size() == 9 && out.front().page_id == 11,
                  "an exclusive lower bound skips its key");
        }
        {
            const auto out = range_of(10, true, 19, false);
            check(out.size() == 9 && out.back().page_id == 18,
                  "an exclusive upper bound skips its key");
        }
        {
            const auto out = range_of(2000, true, 3000, true);
            check(out.empty(), "a range past every key answers empty");
        }
        {
            std::vector<isam::Rid> out;
            check(ridx.range(isam::RangeBound{tuple::Column(-1), true},
                             isam::RangeBound{tuple::Column(-1), true}, out,
                             error) && out.empty(),
                  "a range below every key answers empty");
        }
        check(ridx.close(error), "range index closed");
        std::remove(range_path.c_str());
    }

    // --- 4.5. insert, overflow chaining, frozen separators ---
    {
        const std::string path = temp_path("ovf");
        std::remove(path.c_str());
        isam::IsamIndex idx(manager, person, 0);
        check(idx.build(path, int_entries(0, 339), error),
              "a build filling exactly one leaf succeeds");
        check(idx.entry_count() == 340, "the full leaf holds 340 entries");
        check(idx.insert(tuple::Column(-1), isam::Rid{400, 0}, error),
              "an insert into the full leaf spills to overflow");
        check(idx.insert(tuple::Column(-2), isam::Rid{401, 0}, error),
              "a second spill joins the overflow page");
        std::vector<isam::Rid> out;
        check(idx.lookup(tuple::Column(-1), out, error) &&
                  out.size() == 1 && out[0] == isam::Rid{400, 0},
              "a spilled key is findable");
        check(idx.lookup(tuple::Column(339), out, error) && out.size() == 1,
              "leaf keys still answer after spills");
        check(idx.range(isam::RangeBound{tuple::Column(-2), true},
                        isam::RangeBound{tuple::Column(339), true}, out,
                        error) && out.size() == 342,
              "a range covering leaf and overflow returns all 342 in order");
        check(idx.entry_count() == 342, "the header counts spilled entries");
        check(idx.close(error), "overflow index closed");
        std::remove(path.c_str());

        // Separator pages never change: same index shape, byte compare.
        const std::string sep_path = temp_path("sep");
        std::remove(sep_path.c_str());
        isam::IsamIndex sidx(manager, person, 0);
        check(sidx.build(sep_path, int_entries(1, 400), error),
              "a two-leaf build with one separator level succeeds");
        check(manager.flush_all(error), "pool flushed before the snapshot");
        const std::string before = disk_block(manager, sep_path, 3, error);
        check(before.size() == bufman::kPageSize,
              "the separator block is readable from disk");
        check(sidx.insert(tuple::Column(-1), isam::Rid{500, 0}, error) &&
                  sidx.insert(tuple::Column(341), isam::Rid{501, 0}, error),
              "two inserts land (one spilling, one in the open leaf)");
        check(manager.flush_all(error), "pool flushed after the inserts");
        const std::string after = disk_block(manager, sep_path, 3, error);
        check(before == after,
              "separator pages are byte-identical across inserts");
        check(sidx.close(error), "separator index closed");
        std::remove(sep_path.c_str());
    }

    // --- 4.6. the verify pass ---
    {
        const std::string path = temp_path("verify");
        std::remove(path.c_str());
        isam::IsamIndex idx(manager, person, 0);
        check(idx.build(path, int_entries(0, 339), error), "verify index built");
        std::size_t walked = 0;
        std::vector<std::string> findings;
        const auto all_occupied = [](const isam::Rid&) { return true; };
        check(idx.verify(all_occupied, walked, findings, error) &&
                  findings.empty() && walked == 340,
              "a clean index verifies with no findings");
        check(idx.close(error), "verify index closed for corruption");

        // Corrupt entry 0's rid on disk: it must be named, key and rid.
        check(manager.flush_all(error), "pool flushed before corruption");
        {
            bufman::File file;
            check(manager.open_file(path, file, error), "corruption file opened");
            std::size_t frame = 0;
            check(manager.pin(file, 1, frame, error), "leaf block pinned");
            char* buffer = manager.frame(frame).buffer;
            const std::uint32_t bogus_page = 9999;
            const std::uint32_t bogus_slot = 9999;
            std::memcpy(buffer + 12 + 4, &bogus_page, 4);
            std::memcpy(buffer + 12 + 8, &bogus_slot, 4);
            std::string unpin_error;
            manager.unpin(frame, true, unpin_error);
            std::string close_error;
            manager.close_file(file, close_error);
        }
        check(idx.open(path, error), "the corrupted index reopens");
        const auto below_1000 = [](const isam::Rid& rid) {
            return rid.page_id < 1000;
        };
        check(idx.verify(below_1000, walked, findings, error) &&
                  findings.size() == 1 &&
                  findings[0].find("dangling") != std::string::npos &&
                  findings[0].find("(9999,9999)") != std::string::npos,
              "a dangling entry is named by rid");
        check(idx.close(error), "corrupted index closed");
        std::remove(path.c_str());
    }

    // --- 4.7. persistence across close and reopen ---
    {
        const std::string path = temp_path("persist");
        std::remove(path.c_str());
        isam::IsamIndex idx(manager, person, 0);
        check(idx.build(path, int_entries(1, 1000), error), "persist index built");
        std::vector<isam::Rid> before;
        check(idx.lookup(tuple::Column(500), before, error) &&
                  before.size() == 1,
              "key 500 found before the close");
        std::vector<isam::Rid> range_before;
        check(idx.range(isam::RangeBound{tuple::Column(10), true},
                        isam::RangeBound{tuple::Column(19), true},
                        range_before, error) && range_before.size() == 10,
              "range answered before the close");
        check(idx.close(error) && idx.open(path, error),
              "the index closes and reopens");
        std::vector<isam::Rid> after;
        check(idx.lookup(tuple::Column(500), after, error) && after == before,
              "lookups answer identically after reopen");
        std::vector<isam::Rid> range_after;
        check(idx.range(isam::RangeBound{tuple::Column(10), true},
                        isam::RangeBound{tuple::Column(19), true},
                        range_after, error) && range_after == range_before,
              "ranges answer identically after reopen");
        check(idx.entry_count() == 1000,
              "the reopened header still reports 1000 entries");
        check(idx.close(error), "persist index closed");
        std::remove(path.c_str());
    }

    std::cout << (failures == 0
                      ? "index suite: all checks passed."
                      : "index suite: " + std::to_string(failures) +
                            " check(s) FAILED.")
              << "\n";
    return failures;
}
