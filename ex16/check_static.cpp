// ex14 — static-hash module checks: the StaticHashIndex lifecycle and
// self-identification refusals first, then — as the module lands — the
// modulo routing, overflow spill, erase, verify, build, and
// persistence behavior.

#include "bufpool/BufferManager.h"
#include "hashindex/ExtensibleHashIndex.h"
#include "hashindex/StaticHashIndex.h"
#include "hashindex/HashPages.h"
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
    return "/tmp/ex14-static-" + std::to_string(getpid()) + "-" + tag +
           ".shdx";
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

int run_static_checks() {
    std::string error;
    const schema::TableSchema person = load_schema(kPersonJson, error);
    check(error.empty() && person.column_count() == 4,
          "person schema loads for the static hash checks");
    bufman::BufferManager manager;
    manager.init(16, std::make_unique<bufman::LRUPolicy>());

    // --- lifecycle: create, reopen, self-identification refusals ---
    {
        check(hashindex::static_bucket_capacity(4) == 340,
              "the static bucket capacity follows the 8-byte-header formula");

        const std::string path = temp_path("lifecycle");
        std::remove(path.c_str());
        hashindex::StaticHashIndex idx(manager, person, 0);
        check(idx.create(path, 8, error),
              "create yields an open, empty static hash index");
        check(idx.is_open() && idx.entry_count() == 0 &&
                  idx.bucket_count() == 8,
              "a fresh static index is empty over its 8 buckets");
        check(idx.close(error), "the fresh static index closes");
        check(!idx.is_open(), "a closed static index reports closed");

        hashindex::StaticHashIndex reopened(manager, person, 0);
        check(reopened.open(path, error),
              "a static index file reopens after close");
        check(reopened.bucket_count() == 8 && reopened.entry_count() == 0,
              "the reopened header reports the stored state");
        check(reopened.close(error), "the reopened index closes");

        hashindex::StaticHashIndex wrong_table(manager, person, 0);
        const schema::TableSchema car = load_schema(
            "{\"table_name\":\"car\",\"columns\":["
            "{\"column_name\":\"pid\",\"column_type\":\"Integer\",\"size\":4}]}",
            error);
        hashindex::StaticHashIndex for_car(manager, car, 0);
        check(!for_car.open(path, error) &&
                  error.find("table") != std::string::npos,
              "opening under a different table is refused by name");
        const schema::TableSchema wrong_column = load_schema(
            "{\"table_name\":\"person\",\"columns\":["
            "{\"column_name\":\"age\",\"column_type\":\"Integer\",\"size\":4}]}",
            error);
        hashindex::StaticHashIndex for_age(manager, wrong_column, 0);
        check(!for_age.open(path, error) &&
                  error.find("column") != std::string::npos,
              "opening under a different column is refused by name");
        const schema::TableSchema wrong_type = load_schema(
            "{\"table_name\":\"person\",\"columns\":["
            "{\"column_name\":\"pid\",\"column_type\":\"Double\",\"size\":8}]}",
            error);
        hashindex::StaticHashIndex for_double(manager, wrong_type, 0);
        check(!for_double.open(path, error) &&
                  error.find("key type") != std::string::npos,
              "opening under a different key type is refused");

        // An extensible hash index file is a foreign file here: the
        // magics differ, and the static open must say so by name.
        const std::string extensible_path = temp_path("extensible");
        std::remove(extensible_path.c_str());
        hashindex::ExtensibleHashIndex extensible(manager, person, 0);
        check(extensible.create(extensible_path, error),
              "an extensible index file is created for the refusal check");
        check(extensible.close(error), "the extensible file closes");
        hashindex::StaticHashIndex as_static(manager, person, 0);
        check(!as_static.open(extensible_path, error) &&
                  error.find("not a static hash index") != std::string::npos,
              "an extensible hash file is refused by magic");
        std::remove(extensible_path.c_str());

        const std::string foreign = temp_path("foreign");
        {
            std::ofstream junk(foreign, std::ios::binary | std::ios::trunc);
            junk << "not a static hash index file at all, just text padding";
            junk.seekp(bufman::kPageSize - 1);
            junk.put('\0');
        }
        hashindex::StaticHashIndex foreign_idx(manager, person, 0);
        check(!foreign_idx.open(foreign, error) &&
                  error.find("not a static hash index") != std::string::npos,
              "a foreign file is refused by magic");
        std::remove(foreign.c_str());
        hashindex::StaticHashIndex missing(manager, person, 0);
        check(!missing.open(temp_path("missing"), error),
              "a missing file is refused");
        check(!idx.create(path, 8, error) &&
                  error.find("already holds") != std::string::npos,
              "create refuses an existing non-empty file by name");
        check(!idx.create(temp_path("zero"), 0, error) &&
                  error.find("at least 1") != std::string::npos,
              "a bucket count of zero is refused by name");
        std::remove(path.c_str());
    }

    // --- routing: modulo picks the bucket; collisions stay exact ---
    {
        const std::string path = temp_path("routing");
        std::remove(path.c_str());
        hashindex::StaticHashIndex idx(manager, person, 0);
        check(idx.create(path, 8, error), "the routing index is created");
        check(idx.insert(tuple::Column(1), isam::Rid{1, 0}, error) &&
                  idx.insert(tuple::Column(9), isam::Rid{9, 0}, error),
              "integer keys 1 and 9 insert (both hash to 1 mod 8)");
        std::vector<isam::Rid> one;
        std::vector<isam::Rid> nine;
        check(idx.lookup(tuple::Column(1), one, error) &&
                  idx.lookup(tuple::Column(9), nine, error),
              "both congruent keys answer after insert");
        check(one.size() == 1 && one[0] == isam::Rid{1, 0} &&
                  nine.size() == 1 && nine[0] == isam::Rid{9, 0},
              "congruent keys share a bucket yet answer exactly their own rid");
        std::vector<isam::Rid> miss;
        check(idx.lookup(tuple::Column(17), miss, error) && miss.empty(),
              "another congruent key that never entered answers empty");
        check(idx.close(error), "the routing index closes");
        std::remove(path.c_str());
    }

    // --- overflow spill: a full bucket chains, nothing restructures ---
    {
        const std::string path = temp_path("spill");
        std::remove(path.c_str());
        hashindex::StaticHashIndex idx(manager, person, 0);
        check(idx.create(path, 8, error), "the spill index is created");
        std::vector<isam::Rid> inserted;
        bool flood_ok = true;
        for (std::uint32_t i = 0; i < 350; ++i) {
            const isam::Rid rid{i, 0};
            flood_ok = idx.insert(tuple::Column(9), rid, error) && flood_ok;
            inserted.push_back(rid);
        }
        check(flood_ok, "350 duplicates of one key all insert");
        check(idx.bucket_count() == 8,
              "the insert flood leaves the bucket count alone");
        check(idx.entry_count() == 350, "the header counts the flood");
        std::vector<isam::Rid> found;
        check(idx.lookup(tuple::Column(9), found, error) &&
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
        check(idx.lookup(tuple::Column(8), miss, error) && miss.empty(),
              "a different key returns none of the flood");

        // Persistence: close and reopen, the chain answers identically.
        check(idx.close(error) && idx.open(path, error),
              "the spilled index closes and reopens");
        check(idx.bucket_count() == 8 && idx.entry_count() == 350,
              "the reopened header reports the spilled state");
        std::vector<isam::Rid> after;
        check(idx.lookup(tuple::Column(9), after, error) && after == found,
              "lookups answer identically after reopen");
        check(idx.close(error), "the spill index closes");
        std::remove(path.c_str());
    }

    // --- build matches per-entry inserts ---
    {
        std::vector<std::pair<tuple::Column, isam::Rid>> entries;
        for (int value = 0; value <= 999; ++value) {
            entries.emplace_back(
                tuple::Column(value),
                isam::Rid{static_cast<std::uint32_t>(value),
                          static_cast<std::uint32_t>(value)});
        }
        entries.emplace_back(tuple::Column(500), isam::Rid{7000, 0});
        entries.emplace_back(tuple::Column(500), isam::Rid{7001, 0});

        const std::string built_path = temp_path("build");
        std::remove(built_path.c_str());
        hashindex::StaticHashIndex built(manager, person, 0);
        check(built.build(built_path, 64, entries, error),
              "a build over 1002 entries (one duplicated key) succeeds");
        check(built.entry_count() == 1002 && built.bucket_count() == 64,
              "the built header counts every entry over its 64 buckets");

        const std::string inserted_path = temp_path("inserted");
        std::remove(inserted_path.c_str());
        hashindex::StaticHashIndex stepwise(manager, person, 0);
        bool stepwise_ok = stepwise.create(inserted_path, 64, error);
        for (const auto& [value, rid] : entries) {
            stepwise_ok = stepwise.insert(value, rid, error) && stepwise_ok;
        }
        check(stepwise_ok, "the twin index took every entry one insert at a time");

        bool all_found = true;
        bool answers_match = true;
        for (int value = 0; value <= 999; value += 7) {
            std::vector<isam::Rid> from_build;
            std::vector<isam::Rid> from_inserts;
            all_found =
                built.lookup(tuple::Column(value), from_build, error) &&
                !from_build.empty() &&
                from_build[0] ==
                    isam::Rid{static_cast<std::uint32_t>(value),
                              static_cast<std::uint32_t>(value)} &&
                all_found;
            answers_match =
                stepwise.lookup(tuple::Column(value), from_inserts, error) &&
                from_inserts == from_build &&
                answers_match;
        }
        check(all_found, "every built key is found with its exact rid");
        check(answers_match,
              "the built index answers exactly what per-entry inserts answer");
        std::vector<isam::Rid> dups;
        check(built.lookup(tuple::Column(500), dups, error) &&
                  dups.size() == 3,
              "the duplicated key returns all three rids");
        std::vector<isam::Rid> miss;
        check(built.lookup(tuple::Column(2000), miss, error) && miss.empty(),
              "a missing key answers empty");

        // Persistence over the built file: close and reopen.
        std::vector<isam::Rid> before;
        check(built.lookup(tuple::Column(500), before, error) &&
                  before.size() == 3,
              "the duplicate answers before the close");
        check(built.close(error) && built.open(built_path, error),
              "the built index closes and reopens");
        std::vector<isam::Rid> after;
        check(built.lookup(tuple::Column(500), after, error) && after == before,
              "lookups answer identically after reopen");
        check(built.close(error) && stepwise.close(error),
              "the build indexes close");
        std::remove(built_path.c_str());
        std::remove(inserted_path.c_str());

        // An empty build is a valid empty index.
        const std::string empty_path = temp_path("build-empty");
        std::remove(empty_path.c_str());
        hashindex::StaticHashIndex empty_idx(manager, person, 0);
        check(empty_idx.build(empty_path, 4, {}, error),
              "an empty build succeeds");
        check(empty_idx.entry_count() == 0 && empty_idx.bucket_count() == 4,
              "the empty build holds no entries over its buckets");
        std::vector<isam::Rid> none;
        check(empty_idx.lookup(tuple::Column(1), none, error) && none.empty(),
              "lookups on an empty build answer empty");
        check(empty_idx.close(error), "the empty build closes");
        std::remove(empty_path.c_str());
    }

    // --- erase: exact pair from bucket and chain, no restructuring ---
    {
        const std::string path = temp_path("erase");
        std::remove(path.c_str());
        hashindex::StaticHashIndex idx(manager, person, 0);
        check(idx.create(path, 8, error), "the erase index is created");
        // Keys 1..99 (skipping 50) land one per congruence class mod 8;
        // key 50 arrives only as two duplicates.
        bool inserted = true;
        for (int value = 1; value <= 99; ++value) {
            if (value == 50) continue;
            inserted = idx.insert(tuple::Column(value),
                                  isam::Rid{static_cast<std::uint32_t>(value),
                                            static_cast<std::uint32_t>(value)},
                                  error) &&
                       inserted;
        }
        check(inserted && idx.insert(tuple::Column(50), isam::Rid{500, 0}, error) &&
                  idx.insert(tuple::Column(50), isam::Rid{501, 0}, error),
              "the erase fixture holds 98 keys plus two duplicates of 50");
        std::vector<isam::Rid> out;
        check(idx.erase(tuple::Column(50), isam::Rid{500, 0}, error),
              "erasing one duplicate succeeds");
        check(idx.entry_count() == 99, "the header counts the erase");
        check(idx.lookup(tuple::Column(50), out, error) &&
                  out.size() == 1 && out[0] == isam::Rid{501, 0},
              "the erased duplicate is gone, its sibling remains");
        check(idx.erase(tuple::Column(50), isam::Rid{501, 0}, error),
              "erasing the sibling succeeds");
        check(idx.entry_count() == 98, "the header counts the sibling erase");
        check(idx.lookup(tuple::Column(50), out, error) && out.empty(),
              "key 50 answers empty after both erases");
        check(idx.lookup(tuple::Column(1), out, error) &&
                  idx.lookup(tuple::Column(99), out, error) &&
                  out.size() == 1,
              "the neighbors still answer after erases");
        check(!idx.erase(tuple::Column(50), isam::Rid{501, 0}, error) &&
                  error.find("no entry") != std::string::npos,
              "erasing an absent pair is refused by name");
        check(!idx.erase(tuple::Column(100), isam::Rid{100, 100}, error),
              "erasing a key that never entered is refused");
        check(idx.entry_count() == 98,
              "the refused erases changed nothing");
        check(idx.bucket_count() == 8,
              "erases never restructure the bucket array");
        check(idx.close(error), "the erase index closes");
        std::remove(path.c_str());
    }

    // --- erase reaches into overflow chains ---
    {
        const std::string path = temp_path("erase-ovf");
        std::remove(path.c_str());
        hashindex::StaticHashIndex idx(manager, person, 0);
        check(idx.create(path, 8, error), "the overflow-erase index is created");
        bool inserted = true;
        for (std::uint32_t i = 0; i < 350; ++i) {
            inserted = idx.insert(tuple::Column(9), isam::Rid{i, 0}, error) &&
                       inserted;
        }
        check(inserted && idx.entry_count() == 350,
              "the flood is in place (bucket plus overflow chain)");
        bool erased = true;
        for (std::uint32_t i = 0; i < 10; ++i) {
            erased = idx.erase(tuple::Column(9), isam::Rid{i, 0}, error) &&
                     erased;
        }
        check(erased, "ten bucket-resident duplicates erase");
        for (std::uint32_t i = 340; i < 350; ++i) {
            erased = idx.erase(tuple::Column(9), isam::Rid{i, 0}, error) &&
                     erased;
        }
        check(erased, "ten overflow-resident duplicates erase");
        check(idx.entry_count() == 330, "the header counts both erase bands");
        std::vector<isam::Rid> found;
        check(idx.lookup(tuple::Column(9), found, error) &&
                  found.size() == 330,
              "the survivors answer across bucket and chain");
        bool contiguous = found.size() == 330;
        for (std::uint32_t i = 10; contiguous && i < 340; ++i) {
            contiguous = found[i - 10] == isam::Rid{i, 0};
        }
        check(contiguous,
              "exactly rids 10..339 remain, in arrival order");
        for (std::uint32_t i = 10; i < 340; ++i) {
            erased = idx.erase(tuple::Column(9), isam::Rid{i, 0}, error) &&
                     erased;
        }
        check(erased && idx.entry_count() == 0,
              "erasing everything leaves an empty index");
        std::vector<isam::Rid> empty;
        check(idx.lookup(tuple::Column(9), empty, error) && empty.empty(),
              "the emptied key answers empty");
        check(idx.close(error), "the overflow-erase index closes");
        std::remove(path.c_str());
    }

    // --- the verify pass, with seeded corruption ---
    {
        const std::string path = temp_path("verify");
        std::remove(path.c_str());
        hashindex::StaticHashIndex idx(manager, person, 0);
        check(idx.create(path, 8, error), "the verify index is created");
        check(idx.insert(tuple::Column(4), isam::Rid{4, 4}, error) &&
                  idx.insert(tuple::Column(12), isam::Rid{12, 12}, error) &&
                  idx.insert(tuple::Column(5), isam::Rid{5, 5}, error),
              "three keys insert for the verify checks");
        std::size_t walked = 0;
        std::vector<std::string> findings;
        const auto all_occupied = [](const isam::Rid&) { return true; };
        check(idx.verify(all_occupied, walked, findings, error) &&
                  findings.empty() && walked == 3,
              "a clean static index verifies with no findings");
        check(idx.close(error), "the verify index closed for corruption");

        // Corrupt entry 0's rid on disk: it must be named, key and rid.
        // Key 4 hashes to slot 4 of 8: bucket block 2 + 4 = 6 (0 header,
        // 1 directory); a static bucket's entries start at offset 8.
        check(manager.flush_all(error), "pool flushed before corruption");
        {
            bufman::File file;
            check(manager.open_file(path, file, error),
                  "corruption file opened");
            std::size_t frame = 0;
            check(manager.pin(file, 6, frame, error), "bucket block pinned");
            char* buffer = manager.frame(frame).buffer;
            const std::uint32_t bogus_page = 9999;
            const std::uint32_t bogus_slot = 9999;
            std::memcpy(buffer + 8 + 4, &bogus_page, 4);
            std::memcpy(buffer + 8 + 8, &bogus_slot, 4);
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
        check(idx.close(error), "the dangling fixture closed");
        std::remove(path.c_str());
    }

    // --- misplaced entries and header-total corruption ---
    {
        const std::string path = temp_path("misplaced");
        std::remove(path.c_str());
        hashindex::StaticHashIndex idx(manager, person, 0);
        check(idx.create(path, 8, error), "the misplaced index is created");
        check(idx.insert(tuple::Column(4), isam::Rid{4, 4}, error) &&
                  idx.insert(tuple::Column(12), isam::Rid{12, 12}, error),
              "two keys of one congruence class insert (slot 4 of 8)");
        check(idx.close(error), "the misplaced index closed");
        check(manager.flush_all(error), "pool flushed for the key swap");
        {
            bufman::File file;
            check(manager.open_file(path, file, error),
                  "misplaced corruption file opened");
            std::size_t frame = 0;
            check(manager.pin(file, 6, frame, error),
                  "the slot-4 bucket pinned");
            char* buffer = manager.frame(frame).buffer;
            char other_key[4] = {};
            isam::KeyCodec int_codec(tuple::ColumnType::Integer, 0);
            int_codec.encode(tuple::Column(5), other_key, error);
            std::memcpy(buffer + 8, other_key, 4);  // entry 0's key -> slot 5
            std::string unpin_error;
            manager.unpin(frame, true, unpin_error);
            std::string close_error;
            manager.close_file(file, close_error);
        }
        check(idx.open(path, error), "the misplaced index reopens");
        std::size_t walked = 0;
        std::vector<std::string> findings;
        const auto all_occupied = [](const isam::Rid&) { return true; };
        check(idx.verify(all_occupied, walked, findings, error) &&
                  findings.size() == 1 &&
                  findings[0].find("misplaced") != std::string::npos &&
                  findings[0].find("bucket 6") != std::string::npos,
              "a misplaced entry is named by bucket, key, and rid");
        check(idx.close(error), "the misplaced fixture closed");
        std::remove(path.c_str());
    }
    {
        const std::string path = temp_path("total");
        std::remove(path.c_str());
        hashindex::StaticHashIndex idx(manager, person, 0);
        check(idx.create(path, 8, error), "the total index is created");
        check(idx.insert(tuple::Column(4), isam::Rid{4, 4}, error),
              "one key inserts for the total check");
        check(idx.close(error), "the total index closed");
        check(manager.flush_all(error), "pool flushed for the total bump");
        {
            bufman::File file;
            check(manager.open_file(path, file, error),
                  "total corruption file opened");
            std::size_t frame = 0;
            check(manager.pin(file, 0, frame, error), "header pinned");
            char* buffer = manager.frame(frame).buffer;
            const std::uint64_t bogus_total = 99999;
            std::memcpy(buffer + 428, &bogus_total, 8);  // header entry count
            std::string unpin_error;
            manager.unpin(frame, true, unpin_error);
            std::string close_error;
            manager.close_file(file, close_error);
        }
        check(idx.open(path, error), "the total-corrupt index reopens");
        std::size_t walked = 0;
        std::vector<std::string> findings;
        const auto all_occupied = [](const isam::Rid&) { return true; };
        check(idx.verify(all_occupied, walked, findings, error) &&
                  findings.size() == 1 &&
                  findings[0].find("header counts") != std::string::npos &&
                  walked == 1,
              "a header total that disagrees with the walk is named");
        check(idx.close(error), "the total fixture closed");
        std::remove(path.c_str());
    }

    std::cout << (failures == 0
                      ? "static hash suite: all checks passed."
                      : "static hash suite: " + std::to_string(failures) +
                            " check(s) FAILED.")
              << "\n";
    return failures;
}

}

int run_static_hash_module_checks() {
    return run_static_checks();
}
