// ex14 — hash-index module checks: the hash function, the three page
// views (directory, bucket, overflow), and — as the module lands — the
// ExtensibleHashIndex lifecycle, split/double behavior, erase, verify,
// and persistence.

#include "bufpool/BufferManager.h"
#include "common/Error.h"
#include "hashindex/ExtensibleHashIndex.h"
#include "hashindex/HashPages.h"
#include "index/KeyCodec.h"
#include "policy/LRUPolicy.h"
#include "schema/TableSchema.h"
#include "schema/TableSchemaJson.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
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

std::string temp_path(const std::string& tag) {
    return "/tmp/ex14-hash-" + std::to_string(getpid()) + "-" + tag + ".hdx";
}

schema::TableSchema load_schema(const std::string& json) {
    schema::TableSchema schema("");
    schema::parse_table_schema(json, schema);
    return schema;
}

const char* kPersonJson =
    "{\"table_name\":\"person\",\"columns\":["
    "{\"column_name\":\"pid\",\"column_type\":\"Integer\",\"size\":4},"
    "{\"column_name\":\"name\",\"column_type\":\"String\",\"size\":10},"
    "{\"column_name\":\"age\",\"column_type\":\"Integer\",\"size\":4},"
    "{\"column_name\":\"city\",\"column_type\":\"String\",\"size\":3}]}";

// The last `depth` bits of a hash, the lecture's bucket number.
std::uint32_t slot_of(std::uint32_t hash, std::uint32_t depth) {
    return depth == 0 ? 0 : (hash & ((1u << depth) - 1));
}

int run_hash_checks() {
    const schema::TableSchema person = load_schema(kPersonJson);
    check(person.column_count() == 4,
          "person schema loads for the hash checks");
    bufman::BufferManager manager;
    manager.init(16, std::make_unique<bufman::LRUPolicy>());

    // --- 5.1a. the hash function ---
    {
        const isam::KeyCodec int_codec(tuple::ColumnType::Integer, 0);
        char four[4] = {};
        char twenty[4] = {};
        char twenty_two[4] = {};
        check(refused([&] {
                  int_codec.encode(tuple::Column(4), four);
                  int_codec.encode(tuple::Column(20), twenty);
                  int_codec.encode(tuple::Column(22), twenty_two);
              }).empty(),
              "int keys encode for the hash checks");
        check(hashindex::hash_key(int_codec, four) == 4u,
              "hash(4) is its own bit pattern (100)");
        check(hashindex::hash_key(int_codec, twenty) == 20u,
              "hash(20) is its own bit pattern (10100)");
        check(slot_of(hashindex::hash_key(int_codec, four), 2) == 0u &&
                  slot_of(hashindex::hash_key(int_codec, twenty_two), 2) == 2u &&
                  slot_of(hashindex::hash_key(int_codec, twenty_two), 3) == 6u &&
                  slot_of(hashindex::hash_key(int_codec, twenty), 3) == 4u,
              "the last d bits pick the slot, exactly as the slides work it");
        char minus_one[4] = {};
        int_codec.encode(tuple::Column(-1), minus_one);
        check(hashindex::hash_key(int_codec, minus_one) == 0xFFFFFFFFu,
              "a negative int hashes by its two's-complement pattern");

        const isam::KeyCodec double_codec(tuple::ColumnType::Double, 0);
        char d1[8] = {};
        double_codec.encode(tuple::Column(2.5), d1);
        check(hashindex::hash_key(double_codec, d1) == hashindex::hash_key(double_codec, d1),
              "double keys hash deterministically");
        char d2[8] = {};
        double_codec.encode(tuple::Column(3.5), d2);
        check(hashindex::hash_key(double_codec, d1) != hashindex::hash_key(double_codec, d2),
              "distinct double keys hash apart");

        const isam::KeyCodec string_codec(tuple::ColumnType::String, 10);
        char s1[10] = {};
        string_codec.encode(tuple::Column(std::string("Ruth")), s1);
        check(hashindex::hash_key(string_codec, s1) == hashindex::hash_key(string_codec, s1),
              "string keys hash deterministically");
        char s2[10] = {};
        string_codec.encode(tuple::Column(std::string("Amy")), s2);
        check(hashindex::hash_key(string_codec, s1) != hashindex::hash_key(string_codec, s2),
              "distinct string keys hash apart");
    }

    // --- 5.1b. the page views ---
    {
        std::vector<char> page(bufman::kPageSize);

        isam::KeyCodec int_codec(tuple::ColumnType::Integer, 0);

        isam::KeyCodec codec(tuple::ColumnType::Integer, 0);
        check(hashindex::directory_capacity() == 1022 &&
                  hashindex::bucket_capacity(4) == 340 &&
                  hashindex::overflow_capacity(4) == 340,
              "int-key hash capacities follow the formulas");

        hashindex::DirectoryPage directory;
        directory.bind(page.data());
        directory.format();
        check(refused([&] { directory.attach(); }).empty() &&
                  directory.count() == 0 &&
                  directory.next_directory() == isam::kNoPage,
              "a formatted directory page attaches");
        directory.set_slot(0, 7);
        directory.set_slot(1021, 9);
        check(directory.slot(0) == 7 && directory.slot(1021) == 9 &&
                  directory.count() == 0,
              "directory slots read back at both ends");
        directory.set_next_directory(42);
        check(directory.next_directory() == 42,
              "a directory chain link reads back");

        hashindex::BucketPage bucket;
        bucket.bind(page.data(), 4);
        bucket.format(3, 55);
        check(refused([&] { bucket.attach(); }).empty() &&
                  bucket.count() == 0 &&
                  bucket.local_depth() == 3 && bucket.first_overflow() == 55,
              "a formatted bucket page carries depth and overflow link");
        char key_bytes[4] = {};
        const auto encoded = [&](int value, char* out) {
            int_codec.encode(tuple::Column(value), out);
        };
        encoded(10, key_bytes);
        check(refused([&] {
                  bucket.append_entry(isam::DataEntry{std::string(key_bytes, 4),
                                                      isam::Rid{1, 2}});
              }).empty(),
              "a bucket append succeeds");
        encoded(20, key_bytes);
        check(refused([&] {
                  bucket.append_entry(isam::DataEntry{std::string(key_bytes, 4),
                                                      isam::Rid{3, 4}});
              }).empty(),
              "a second bucket append succeeds");
        check(bucket.count() == 2 &&
                  bucket.get_entry(0).rid == isam::Rid{1, 2} &&
                  bucket.get_entry(1).rid == isam::Rid{3, 4},
              "bucket entries keep their arrival order");
        bucket.remove_entry(0);
        check(bucket.count() == 1 &&
                  bucket.get_entry(0).rid == isam::Rid{3, 4},
              "removing the first entry shifts the tail down");
        bool full_refused = false;
        bucket.set_local_depth(0);
        for (std::size_t i = bucket.count(); i < bucket.capacity(); ++i) {
            try {
                bucket.append_entry(
                    isam::DataEntry{std::string(key_bytes, 4),
                                    isam::Rid{0, 0}});
            } catch (const common::Error&) {
                full_refused = true;
                break;
            }
        }
        check(!full_refused, "filling the bucket succeeds");
        check(refused([&] {
                  bucket.append_entry(
                      isam::DataEntry{std::string(key_bytes, 4),
                                      isam::Rid{0, 0}});
              }).find("full") != std::string::npos,
              "a full bucket refuses another entry by name");

        hashindex::OverflowPage overflow;
        overflow.bind(page.data(), 4);
        overflow.format(77);
        check(refused([&] { overflow.attach(); }).empty() &&
                  overflow.next_overflow() == 77 && overflow.count() == 0,
              "a formatted overflow page carries its chain link");
        check(refused([&] {
                  overflow.append_entry(
                      isam::DataEntry{std::string(key_bytes, 4),
                                      isam::Rid{5, 6}});
              }).empty(),
              "an overflow append succeeds");
        check(overflow.count() == 1 &&
                  overflow.get_entry(0).rid == isam::Rid{5, 6},
              "overflow entries read back");
        overflow.remove_entry(0);
        check(overflow.count() == 0, "overflow removal empties the page");
    }

    // --- 5.2. lifecycle: create, reopen, self-identification refusals ---
    {
        const std::string path = temp_path("lifecycle");
        std::remove(path.c_str());
        hashindex::ExtensibleHashIndex idx(manager, person, 0);
        check(refused([&] { idx.create(path); }).empty(),
              "create yields an open, empty hash index");
        check(idx.is_open() && idx.entry_count() == 0 && idx.global_depth() == 1,
              "a fresh hash index is empty at global depth 1");
        run([&] { idx.close(); }, "the fresh hash index closes");
        check(!idx.is_open(), "a closed hash index reports closed");

        hashindex::ExtensibleHashIndex reopened(manager, person, 0);
        run([&] { reopened.open(path); },
            "a hash index file reopens after close");
        check(reopened.global_depth() == 1 && reopened.entry_count() == 0,
              "the reopened header reports the stored state");
        run([&] { reopened.close(); }, "the reopened index closes");

        hashindex::ExtensibleHashIndex wrong_table(manager, person, 0);
        const schema::TableSchema car = load_schema(
            "{\"table_name\":\"car\",\"columns\":["
            "{\"column_name\":\"pid\",\"column_type\":\"Integer\",\"size\":4}]}");
        hashindex::ExtensibleHashIndex for_car(manager, car, 0);
        check(refused([&] { for_car.open(path); })
                      .find("table") != std::string::npos,
              "opening under a different table is refused by name");
        const schema::TableSchema wrong_column = load_schema(
            "{\"table_name\":\"person\",\"columns\":["
            "{\"column_name\":\"age\",\"column_type\":\"Integer\",\"size\":4}]}");
        hashindex::ExtensibleHashIndex for_age(manager, wrong_column, 0);
        check(refused([&] { for_age.open(path); })
                      .find("column") != std::string::npos,
              "opening under a different column is refused by name");
        const schema::TableSchema wrong_type = load_schema(
            "{\"table_name\":\"person\",\"columns\":["
            "{\"column_name\":\"pid\",\"column_type\":\"Double\",\"size\":8}]}");
        hashindex::ExtensibleHashIndex for_double(manager, wrong_type, 0);
        check(refused([&] { for_double.open(path); })
                      .find("key type") != std::string::npos,
              "opening under a different key type is refused");
        const std::string foreign = temp_path("foreign");
        {
            std::ofstream junk(foreign, std::ios::binary | std::ios::trunc);
            junk << "not a hash index file at all, just text padding";
            junk.seekp(bufman::kPageSize - 1);
            junk.put('\0');
        }
        hashindex::ExtensibleHashIndex foreign_idx(manager, person, 0);
        check(refused([&] { foreign_idx.open(foreign); })
                      .find("not a hash index") != std::string::npos,
              "a foreign file is refused by magic");
        std::remove(foreign.c_str());
        hashindex::ExtensibleHashIndex missing(manager, person, 0);
        check(!refused([&] { missing.open(temp_path("missing")); }).empty(),
              "a missing file is refused");
        check(refused([&] { idx.create(path); })
                      .find("already holds") != std::string::npos,
              "create refuses an existing non-empty file by name");
        std::remove(path.c_str());
    }

    // --- 5.3a. the lecture's placements at the lazy phase ---
    {
        const std::string path = temp_path("placements");
        std::remove(path.c_str());
        hashindex::ExtensibleHashIndex idx(manager, person, 0);
        run([&] { idx.create(path); }, "placements index created");
        bool all_inserted = true;
        for (int value : {4, 12, 32, 16, 20, 22, 5}) {
            try {
                idx.insert(tuple::Column(value),
                           isam::Rid{static_cast<std::uint32_t>(value), 0});
            } catch (const common::Error&) {
                all_inserted = false;
            }
        }
        check(all_inserted, "the slides' keys all insert");
        check(idx.global_depth() == 1 && idx.entry_count() == 7,
              "the slides' keys share one bucket (all even but 5), depth 1");
        bool all_found = true;
        for (int value : {4, 12, 32, 16, 20, 22, 5}) {
            std::vector<isam::Rid> out;
            try {
                idx.lookup(tuple::Column(value), out);
                all_found = out.size() == 1 &&
                            out[0] ==
                                isam::Rid{static_cast<std::uint32_t>(value),
                                          0} &&
                            all_found;
            } catch (const common::Error&) {
                all_found = false;
            }
        }
        check(all_found, "every one of the slides' keys is found by hash");
        std::vector<isam::Rid> miss;
        check(refused([&] { idx.lookup(tuple::Column(6), miss); }).empty() &&
                  miss.empty(),
              "a key that never entered answers empty");
        run([&] { idx.close(); }, "placements index closed");
        std::remove(path.c_str());
    }

    // --- 5.3b. split at the global depth doubles; below it does not ---
    {
        const std::string path = temp_path("split");
        std::remove(path.c_str());
        hashindex::ExtensibleHashIndex idx(manager, person, 0);
        run([&] { idx.create(path); }, "split index created");
        // 340 even keys fill slot 0's lazy bucket; an odd key grows
        // slot 1's own bucket.
        bool inserted = true;
        for (int value = 0; value <= 678; value += 2) {
            try {
                idx.insert(tuple::Column(value),
                           isam::Rid{static_cast<std::uint32_t>(value), 0});
            } catch (const common::Error&) {
                inserted = false;
            }
        }
        check(inserted && idx.entry_count() == 340 &&
                  idx.global_depth() == 1,
              "340 even keys fill slot 0's bucket at depth 1");
        run([&] { idx.insert(tuple::Column(1), isam::Rid{1, 0}); },
            "an odd key grows slot 1's own bucket");

        // The 341st even key splits the full bucket: local depth 1 ==
        // global depth 1, so the directory doubles to depth 2.
        run([&] { idx.insert(tuple::Column(680), isam::Rid{680, 0}); },
            "the 341st even key splits the full bucket");
        check(idx.global_depth() == 2,
              "splitting at the global depth doubled the directory");
        check(idx.entry_count() == 342, "the header counts the split insert");
        bool evens_found = true;
        for (int value = 0; value <= 680; value += 2) {
            std::vector<isam::Rid> out;
            try {
                idx.lookup(tuple::Column(value), out);
                evens_found = out.size() == 1 &&
                              out[0] ==
                                  isam::Rid{static_cast<std::uint32_t>(value),
                                            0} &&
                              evens_found;
            } catch (const common::Error&) {
                evens_found = false;
            }
        }
        check(evens_found,
              "every even key answers after the split (both sides)");

        // The odd bucket predates the doubling: slots 1 and 3 are
        // corresponding elements pointing at it. Filling it with 340
        // odds and forcing one more splits at local depth 1 < global
        // depth 2 — no doubling, slot 3 re-points to the new bucket.
        bool odds_inserted = true;
        for (int value = 3; value <= 679; value += 2) {
            try {
                idx.insert(tuple::Column(value),
                           isam::Rid{static_cast<std::uint32_t>(value), 0});
            } catch (const common::Error&) {
                odds_inserted = false;
            }
        }
        check(odds_inserted && idx.entry_count() == 681,
              "340 odds fill the shared odd bucket");
        run([&] { idx.insert(tuple::Column(683), isam::Rid{683, 0}); },
            "the 341st odd key splits the shared bucket");
        check(idx.global_depth() == 2,
              "a split below the global depth keeps the directory size");
        check(idx.entry_count() == 682, "the header counts the odd split");
        bool odds_found = true;
        for (int value = 1; value <= 679; value += 2) {
            std::vector<isam::Rid> out;
            try {
                idx.lookup(tuple::Column(value), out);
                odds_found = out.size() == 1 &&
                             out[0] ==
                                 isam::Rid{static_cast<std::uint32_t>(value),
                                           0} &&
                             odds_found;
            } catch (const common::Error&) {
                odds_found = false;
            }
        }
        {
            std::vector<isam::Rid> out;
            try {
                idx.lookup(tuple::Column(683), out);
                odds_found = out.size() == 1 && out[0] == isam::Rid{683, 0} &&
                             odds_found;
            } catch (const common::Error&) {
                odds_found = false;
            }
        }
        check(odds_found, "every odd key answers after the below-depth split");
        run([&] { idx.close(); }, "split index closed");
        std::remove(path.c_str());
    }

    // --- 5.3c. a duplicate flood abandons the split to overflow ---
    {
        const std::string path = temp_path("flood");
        std::remove(path.c_str());
        hashindex::ExtensibleHashIndex idx(manager, person, 0);
        run([&] { idx.create(path); }, "flood index created");
        std::vector<isam::Rid> inserted;
        bool flood_ok = true;
        for (std::uint32_t i = 0; i < 350; ++i) {
            const isam::Rid rid{i, 0};
            try {
                idx.insert(tuple::Column(9), rid);
            } catch (const common::Error&) {
                flood_ok = false;
            }
            inserted.push_back(rid);
        }
        check(flood_ok, "350 duplicates of one key all insert");
        check(idx.global_depth() == 1,
              "a flood of one key never splits (all hashes agree)");
        check(idx.entry_count() == 350, "the header counts the flood");
        std::vector<isam::Rid> found;
        check(refused([&] { idx.lookup(tuple::Column(9), found); }).empty() &&
                  found.size() == 350,
              "the flood is findable across bucket and overflow chain");
        std::vector<isam::Rid> expected = inserted;
        std::vector<isam::Rid> actual = found;
        const auto rid_order = [](const isam::Rid& a, const isam::Rid& b) {
            return a.page_id != b.page_id ? a.page_id < b.page_id
                                          : a.slot < b.slot;
        };
        std::sort(expected.begin(), expected.end(), rid_order);
        std::sort(actual.begin(), actual.end(), rid_order);
        check(expected == actual, "the flood returns exactly the entered rids");
        check(found.front() == isam::Rid{0, 0},
              "arrival order survives the bucket-to-chain spill");
        std::vector<isam::Rid> miss;
        check(refused([&] { idx.lookup(tuple::Column(8), miss); }).empty() &&
                  miss.empty(),
              "a different key returns none of the flood");
        run([&] { idx.close(); }, "flood index closed");
        std::remove(path.c_str());
    }

    // --- 5.4a. erase: exact pair, gaps, no restructuring ---
    {
        const std::string path = temp_path("erase");
        std::remove(path.c_str());
        hashindex::ExtensibleHashIndex idx(manager, person, 0);
        run([&] { idx.create(path); }, "erase index created");
        bool inserted = true;
        for (int value = 0; value <= 99; ++value) {
            if (value == 50) continue;  // key 50 arrives only as duplicates
            try {
                idx.insert(tuple::Column(value),
                           isam::Rid{static_cast<std::uint32_t>(value),
                                     static_cast<std::uint32_t>(value)});
            } catch (const common::Error&) {
                inserted = false;
            }
        }
        check(inserted &&
                  refused([&] {
                      idx.insert(tuple::Column(50), isam::Rid{500, 0});
                      idx.insert(tuple::Column(50), isam::Rid{501, 0});
                  }).empty(),
              "the erase fixture holds 99 keys plus two duplicates of 50");
        std::vector<isam::Rid> out;
        run([&] { idx.erase(tuple::Column(50), isam::Rid{500, 0}); },
            "erasing one duplicate succeeds");
        check(idx.entry_count() == 100, "the header counts the erase");
        check(refused([&] { idx.lookup(tuple::Column(50), out); }).empty() &&
                  out.size() == 1 && out[0] == isam::Rid{501, 0},
              "the erased duplicate is gone, its sibling remains");
        run([&] { idx.erase(tuple::Column(50), isam::Rid{501, 0}); },
            "erasing the sibling succeeds");
        check(idx.entry_count() == 99, "the header counts the sibling erase");
        check(refused([&] { idx.lookup(tuple::Column(50), out); }).empty() &&
                  out.empty(),
              "key 50 answers empty after both erases");
        check(refused([&] {
                  idx.lookup(tuple::Column(0), out);
                  idx.lookup(tuple::Column(99), out);
              }).empty() &&
                  out.size() == 1,
              "the neighbors still answer after erases");
        check(refused([&] { idx.erase(tuple::Column(50), isam::Rid{501, 0}); })
                      .find("no entry") != std::string::npos,
              "erasing an absent pair is refused by name");
        check(idx.entry_count() == 99,
              "the refused erase changed nothing");
        check(idx.global_depth() == 1,
              "erases never restructure the directory");
        run([&] { idx.close(); }, "erase index closed");
        std::remove(path.c_str());
    }

    // --- 5.4b. erase reaches into overflow chains ---
    {
        const std::string path = temp_path("erase-ovf");
        std::remove(path.c_str());
        hashindex::ExtensibleHashIndex idx(manager, person, 0);
        run([&] { idx.create(path); }, "the overflow-erase index is created");
        bool inserted = true;
        for (std::uint32_t i = 0; i < 350; ++i) {
            try {
                idx.insert(tuple::Column(9), isam::Rid{i, 0});
            } catch (const common::Error&) {
                inserted = false;
            }
        }
        check(inserted && idx.entry_count() == 350,
              "the flood is in place (bucket plus overflow chain)");
        bool erased = true;
        for (std::uint32_t i = 0; i < 10; ++i) {
            try {
                idx.erase(tuple::Column(9), isam::Rid{i, 0});
            } catch (const common::Error&) {
                erased = false;
            }
        }
        check(erased, "ten bucket-resident duplicates erase");
        for (std::uint32_t i = 340; i < 350; ++i) {
            try {
                idx.erase(tuple::Column(9), isam::Rid{i, 0});
            } catch (const common::Error&) {
                erased = false;
            }
        }
        check(erased, "ten overflow-resident duplicates erase");
        check(idx.entry_count() == 330, "the header counts both erase bands");
        std::vector<isam::Rid> found;
        check(refused([&] { idx.lookup(tuple::Column(9), found); }).empty() &&
                  found.size() == 330,
              "the survivors answer across bucket and chain");
        bool contiguous = found.size() == 330;
        for (std::uint32_t i = 10; contiguous && i < 340; ++i) {
            contiguous = found[i - 10] == isam::Rid{i, 0};
        }
        check(contiguous,
              "exactly rids 10..339 remain, in arrival order");
        for (std::uint32_t i = 10; i < 340; ++i) {
            try {
                idx.erase(tuple::Column(9), isam::Rid{i, 0});
            } catch (const common::Error&) {
                erased = false;
            }
        }
        check(erased && idx.entry_count() == 0,
              "erasing everything leaves an empty index");
        std::vector<isam::Rid> empty;
        check(refused([&] { idx.lookup(tuple::Column(9), empty); }).empty() &&
                  empty.empty(),
              "the emptied key answers empty");
        std::size_t walked = 0;
        std::vector<std::string> findings;
        const auto all_occupied = [](const isam::Rid&) { return true; };
        check(refused([&] { idx.verify(all_occupied, walked, findings); })
                          .empty() &&
                  findings.empty() && walked == 0,
              "an index emptied by erases verifies clean");
        run([&] { idx.close(); }, "the overflow-erase index closes");
        std::remove(path.c_str());
    }

    // --- 5.4c. the verify pass, with seeded corruption ---
    {
        const std::string path = temp_path("verify");
        std::remove(path.c_str());
        hashindex::ExtensibleHashIndex idx(manager, person, 0);
        run([&] { idx.create(path); }, "verify index created");
        check(refused([&] {
                  idx.insert(tuple::Column(4), isam::Rid{4, 4});
                  idx.insert(tuple::Column(12), isam::Rid{12, 12});
                  idx.insert(tuple::Column(5), isam::Rid{5, 5});
              }).empty(),
              "three keys insert for the verify checks");
        std::size_t walked = 0;
        std::vector<std::string> findings;
        const auto all_occupied = [](const isam::Rid&) { return true; };
        check(refused([&] { idx.verify(all_occupied, walked, findings); })
                          .empty() &&
                  findings.empty() && walked == 3,
              "a clean hash index verifies with no findings");
        run([&] { idx.close(); }, "verify index closed for corruption");

        // Corrupt entry 0's rid on disk: it must be named, key and rid.
        // Block 2 is slot 0's lazy bucket (0 header, 1 directory).
        run([&] { manager.flush_all(); }, "pool flushed before corruption");
        {
            bufman::File file;
            run([&] { file = manager.open_file(path); },
                "corruption file opened");
            std::size_t frame = 0;
            run([&] { manager.pin(file, 2, frame); }, "bucket block pinned");
            char* buffer = manager.frame(frame).buffer;
            const std::uint32_t bogus_page = 9999;
            const std::uint32_t bogus_slot = 9999;
            std::memcpy(buffer + 12 + 4, &bogus_page, 4);
            std::memcpy(buffer + 12 + 8, &bogus_slot, 4);
            manager.unpin(frame, true);
            try {
                manager.close_file(file);
            } catch (const common::Error&) {
            }
        }
        run([&] { idx.open(path); }, "the corrupted index reopens");
        const auto below_1000 = [](const isam::Rid& rid) {
            return rid.page_id < 1000;
        };
        run([&] { idx.verify(below_1000, walked, findings); },
            "a dangling entry is named by rid");
        check(findings.size() == 1 &&
                  findings[0].find("dangling") != std::string::npos &&
                  findings[0].find("(9999,9999)") != std::string::npos,
              "a dangling entry is named by rid");
        run([&] { idx.close(); }, "the dangling fixture closed");
        std::remove(path.c_str());
    }

    // --- 5.4d. misplaced entries and depth/total corruption ---
    {
        const std::string path = temp_path("misplaced");
        std::remove(path.c_str());
        hashindex::ExtensibleHashIndex idx(manager, person, 0);
        run([&] { idx.create(path); }, "misplaced index created");
        check(refused([&] {
                  idx.insert(tuple::Column(4), isam::Rid{4, 4});
                  idx.insert(tuple::Column(8), isam::Rid{8, 8});
              }).empty(),
              "two even keys insert for the misplaced check");
        run([&] { idx.close(); }, "misplaced index closed");
        run([&] { manager.flush_all(); }, "pool flushed for the key swap");
        {
            bufman::File file;
            run([&] { file = manager.open_file(path); },
                "misplaced corruption file opened");
            std::size_t frame = 0;
            run([&] { manager.pin(file, 2, frame); },
                "the even bucket pinned");
            char* buffer = manager.frame(frame).buffer;
            char odd_key[4] = {};
            isam::KeyCodec int_codec(tuple::ColumnType::Integer, 0);
            int_codec.encode(tuple::Column(5), odd_key);
            std::memcpy(buffer + 12, odd_key, 4);  // entry 0's key -> odd
            manager.unpin(frame, true);
            try {
                manager.close_file(file);
            } catch (const common::Error&) {
            }
        }
        run([&] { idx.open(path); }, "the misplaced index reopens");
        std::size_t walked = 0;
        std::vector<std::string> findings;
        const auto all_occupied = [](const isam::Rid&) { return true; };
        run([&] { idx.verify(all_occupied, walked, findings); },
            "a misplaced entry is named by bucket, key, and rid");
        check(findings.size() == 1 &&
                  findings[0].find("misplaced") != std::string::npos &&
                  findings[0].find("bucket 2") != std::string::npos,
              "a misplaced entry is named by bucket, key, and rid");
        run([&] { idx.close(); }, "misplaced fixture closed");
        std::remove(path.c_str());
    }
    {
        const std::string path = temp_path("depth");
        std::remove(path.c_str());
        hashindex::ExtensibleHashIndex idx(manager, person, 0);
        run([&] { idx.create(path); }, "depth index created");
        run([&] { idx.insert(tuple::Column(4), isam::Rid{4, 4}); },
            "one key inserts for the depth check");
        run([&] { idx.close(); }, "depth index closed");
        run([&] { manager.flush_all(); }, "pool flushed for the depth bump");
        {
            bufman::File file;
            run([&] { file = manager.open_file(path); },
                "depth corruption file opened");
            std::size_t frame = 0;
            run([&] { manager.pin(file, 2, frame); },
                "the depth bucket pinned");
            char* buffer = manager.frame(frame).buffer;
            const std::uint32_t bogus_depth = 99;
            std::memcpy(buffer + 4, &bogus_depth, 4);
            manager.unpin(frame, true);
            try {
                manager.close_file(file);
            } catch (const common::Error&) {
            }
        }
        run([&] { idx.open(path); }, "the depth-corrupt index reopens");
        std::size_t walked = 0;
        std::vector<std::string> findings;
        const auto all_occupied = [](const isam::Rid&) { return true; };
        run([&] { idx.verify(all_occupied, walked, findings); },
            "a bucket deeper than the global depth is named");
        check(!findings.empty() &&
                  findings[0].find("local depth") != std::string::npos &&
                  findings[0].find("exceeds") != std::string::npos,
              "a bucket deeper than the global depth is named");
        run([&] { idx.close(); }, "depth fixture closed");
        std::remove(path.c_str());
    }
    {
        const std::string path = temp_path("total");
        std::remove(path.c_str());
        hashindex::ExtensibleHashIndex idx(manager, person, 0);
        run([&] { idx.create(path); }, "total index created");
        run([&] { idx.insert(tuple::Column(4), isam::Rid{4, 4}); },
            "one key inserts for the total check");
        run([&] { idx.close(); }, "total index closed");
        run([&] { manager.flush_all(); }, "pool flushed for the total bump");
        {
            bufman::File file;
            run([&] { file = manager.open_file(path); },
                "total corruption file opened");
            std::size_t frame = 0;
            run([&] { manager.pin(file, 0, frame); }, "header pinned");
            char* buffer = manager.frame(frame).buffer;
            const std::uint64_t bogus_total = 99999;
            std::memcpy(buffer + 428, &bogus_total, 8);  // header entry count
            manager.unpin(frame, true);
            try {
                manager.close_file(file);
            } catch (const common::Error&) {
            }
        }
        run([&] { idx.open(path); }, "the total-corrupt index reopens");
        std::size_t walked = 0;
        std::vector<std::string> findings;
        const auto all_occupied = [](const isam::Rid&) { return true; };
        run([&] { idx.verify(all_occupied, walked, findings); },
            "a header total that disagrees with the walk is named");
        check(findings.size() == 1 &&
                  findings[0].find("header counts") != std::string::npos &&
                  walked == 1,
              "a header total that disagrees with the walk is named");
        run([&] { idx.close(); }, "total fixture closed");
        std::remove(path.c_str());
    }

    // --- 5.5. build, and persistence across close and reopen ---
    {
        const std::string path = temp_path("build");
        std::remove(path.c_str());
        hashindex::ExtensibleHashIndex idx(manager, person, 0);
        std::vector<std::pair<tuple::Column, isam::Rid>> entries;
        for (int value = 0; value <= 999; ++value) {
            entries.emplace_back(tuple::Column(value),
                                 isam::Rid{static_cast<std::uint32_t>(value),
                                           static_cast<std::uint32_t>(value)});
        }
        entries.emplace_back(tuple::Column(500), isam::Rid{7000, 0});
        entries.emplace_back(tuple::Column(500), isam::Rid{7001, 0});
        run([&] { idx.build(path, entries); },
            "a build over 1002 entries (one duplicated key) succeeds");
        check(idx.entry_count() == 1002, "the built header counts every entry");
        check(idx.global_depth() > 1,
              "the build grew the directory past its initial depth");
        bool all_found = true;
        for (int value = 0; value <= 999; ++value) {
            std::vector<isam::Rid> out;
            try {
                idx.lookup(tuple::Column(value), out);
                all_found = !out.empty() &&
                            out[0] ==
                                isam::Rid{static_cast<std::uint32_t>(value),
                                          static_cast<std::uint32_t>(value)} &&
                            all_found;
            } catch (const common::Error&) {
                all_found = false;
            }
        }
        check(all_found, "every built key is found with its exact rid");
        std::vector<isam::Rid> dups;
        check(refused([&] { idx.lookup(tuple::Column(500), dups); }).empty() &&
                  dups.size() == 3,
              "the duplicated key returns all three rids");
        std::vector<isam::Rid> miss;
        check(refused([&] { idx.lookup(tuple::Column(2000), miss); }).empty() &&
                  miss.empty(),
              "a missing key answers empty");

        // Persistence: close and reopen, answers identical.
        std::vector<isam::Rid> before;
        check(refused([&] { idx.lookup(tuple::Column(500), before); })
                          .empty() &&
                  before.size() == 3,
              "the duplicate answers before the close");
        run([&] {
            idx.close();
            idx.open(path);
        }, "the built index closes and reopens");
        check(idx.entry_count() == 1002 && idx.global_depth() > 1,
              "the reopened header reports the built state");
        std::vector<isam::Rid> after;
        check(refused([&] { idx.lookup(tuple::Column(500), after); }).empty() &&
                  after == before,
              "lookups answer identically after reopen");
        std::size_t walked = 0;
        std::vector<std::string> findings;
        const auto all_occupied = [](const isam::Rid&) { return true; };
        check(refused([&] { idx.verify(all_occupied, walked, findings); })
                          .empty() &&
                  findings.empty() && walked == 1002,
              "the reopened built index verifies clean");
        run([&] { idx.close(); }, "build index closed");
        std::remove(path.c_str());

        // An empty build is a valid empty index.
        const std::string empty_path = temp_path("build-empty");
        std::remove(empty_path.c_str());
        hashindex::ExtensibleHashIndex empty_idx(manager, person, 0);
        run([&] { empty_idx.build(empty_path, {}); },
            "an empty build succeeds");
        check(empty_idx.entry_count() == 0, "the empty build holds no entries");
        std::vector<isam::Rid> none;
        check(refused([&] { empty_idx.lookup(tuple::Column(1), none); })
                          .empty() &&
                  none.empty(),
              "lookups on an empty build answer empty");
        run([&] { empty_idx.close(); }, "the empty build closes");
        std::remove(empty_path.c_str());
    }

    std::cout << (failures == 0 ? "hash suite: all checks passed."
                                : "hash suite: " + std::to_string(failures) +
                                      " check(s) FAILED.")
              << "\n";
    return failures;
}

}

int run_hash_module_checks() {
    return run_hash_checks();
}
