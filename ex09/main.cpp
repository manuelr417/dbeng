#include "BlockFile.h"
#include "BufferManager.h"
#include "LRUPolicy.h"
#include "ReplacementPolicy.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
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

std::string temp_path(const char* suffix) {
    return std::string("/tmp/ex09-bufman-") + std::to_string(getpid()) + suffix;
}

// Seeds `count` blocks; block i is filled with the byte 'a' + i, so a frame's
// buffer[0] tells which block it holds.
void seed_file(const std::string& path, int count) {
    bufman::File file;
    std::string error;
    if (!bufman::open_for_append(path, file, error)) {
        std::cerr << "seed: " << error << '\n';
        std::exit(1);
    }
    std::array<char, bufman::kBlockSize> block{};
    for (int i = 0; i < count; ++i) {
        std::memset(block.data(), static_cast<char>('a' + i), block.size());
        if (!bufman::write_block(file, static_cast<std::uint64_t>(i),
                                    block.data(), block.size(), error)) {
            std::cerr << "seed: " << error << '\n';
            std::exit(1);
        }
    }
    bufman::close(file);
}

char read_disk_byte(const std::string& path, std::uint64_t block_number) {
    bufman::File file;
    std::string error;
    if (!bufman::open_for_read(path, file, error)) {
        std::cerr << "verify: " << error << '\n';
        std::exit(1);
    }
    std::array<char, bufman::kBlockSize> block{};
    if (!bufman::read_block(file, block_number, block, error)) {
        std::cerr << "verify: " << error << '\n';
        std::exit(1);
    }
    bufman::close(file);
    return block[0];
}

std::array<char, bufman::kBlockSize> read_disk_block(const std::string& path,
                                                     std::uint64_t block_number) {
    bufman::File file;
    std::string error;
    if (!bufman::open_for_read(path, file, error)) {
        std::cerr << "verify: " << error << '\n';
        std::exit(1);
    }
    std::array<char, bufman::kBlockSize> block{};
    if (!bufman::read_block(file, block_number, block, error)) {
        std::cerr << "verify: " << error << '\n';
        std::exit(1);
    }
    bufman::close(file);
    return block;
}

Person make_person(int pid, const char* name, int age, const char* city) {
    Person value{};
    value.pid = pid;
    value.age = age;
    std::strncpy(value.name, name, sizeof(value.name) - 1);
    std::strncpy(value.city, city, sizeof(value.city) - 1);
    return value;
}

// Writes one block in the Person record layout (serialize_block zero-fills
// the free slots), so on-disk bytes match what the pool will read.
void seed_person_block(const std::string& path, std::uint64_t block_number,
                       const std::vector<Person>& people) {
    bufman::File file;
    std::string error;
    if (!bufman::open_for_append(path, file, error)) {
        std::cerr << "seed: " << error << '\n';
        std::exit(1);
    }
    std::array<char, bufman::kBlockSize> block{};
    bufman::serialize_block(people, block.data());
    if (!bufman::write_block(file, block_number, block.data(), block.size(), error)) {
        std::cerr << "seed: " << error << '\n';
        std::exit(1);
    }
    bufman::close(file);
}

// Records every hook call so tests can assert the manager -> policy contract.
class RecordingPolicy final : public bufman::ReplacementPolicy {
public:
    void init(std::size_t) override {
        events.clear();
        last_candidates.clear();
    }
    void on_access(std::size_t frame) override {
        events.push_back("access:" + std::to_string(frame));
    }
    void on_load(std::size_t frame) override {
        events.push_back("load:" + std::to_string(frame));
    }
    void on_remove(std::size_t frame) override {
        events.push_back("remove:" + std::to_string(frame));
    }
    std::optional<std::size_t> pick_victim(
            const std::vector<std::size_t>& candidates) const override {
        last_candidates = candidates;
        if (candidates.empty()) {
            return std::nullopt;
        }
        return candidates.front();
    }

    mutable std::vector<std::string> events;
    mutable std::vector<std::size_t> last_candidates;
};

}

int main() {
    const std::string seeded_path = temp_path("-seed.bin");
    std::remove(seeded_path.c_str());
    seed_file(seeded_path, 6);

    // --- miss loads from disk; hit reuses the frame and stacks pins ---
    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(3, std::make_unique<bufman::LRUPolicy>());
        bufman::File file;
        check(mgr.open_file(seeded_path, file, error), "open_file succeeds");

        std::size_t f = 999;
        check(mgr.pin(file, 0, f, error), "pin block 0 (miss) succeeds");
        check(f == 0, "block 0 lands in never-used frame 0");
        check(mgr.frame(f).buffer[0] == 'a', "block 0 bytes loaded from disk");
        check(mgr.frame(f).pin_count == 1, "pin gives pin_count 1");

        std::size_t f_again = 999;
        check(mgr.pin(file, 0, f_again, error) && f_again == f,
              "re-pinning block 0 hits the same frame");
        check(mgr.frame(f).pin_count == 2, "second pin gives pin_count 2");
        check(mgr.resident(file, 0), "resident reports a loaded block");
        check(!mgr.resident(file, 5), "resident reports an absent block");
        mgr.unpin(f, false, error);
        mgr.unpin(f, false, error);
        check(mgr.frame(f).pin_count == 0, "two unpins give pin_count 0");

        // --- free frames are consumed in order; then LRU decides ---
        std::size_t f1 = 999, f2 = 999;
        check(mgr.pin(file, 1, f1, error) && f1 == 1, "block 1 lands in never-used frame 1");
        check(mgr.pin(file, 2, f2, error) && f2 == 2, "block 2 lands in never-used frame 2");
        // Frame 0 was already unpinned in the hit test above.
        mgr.unpin(1, false, error);
        mgr.unpin(2, false, error);

        // LRU order (MRU -> LRU) is now 0, 2, 1.
        mgr.pin(file, 0, f, error);
        mgr.unpin(f, false, error);
        std::size_t f3 = 999;
        check(mgr.pin(file, 3, f3, error) && f3 == 1,
              "block 3 evicts the LRU frame (block 1's), not block 0's");
        check(mgr.frame(f3).buffer[0] == 'd', "block 3 bytes loaded");
        mgr.unpin(f3, false, error);

        std::size_t f1b = 999;
        check(mgr.pin(file, 1, f1b, error) && f1b == 2,
              "re-pinning block 1 evicts the next LRU frame (block 2's)");
        check(mgr.frame(f1b).buffer[0] == 'b', "block 1 bytes reloaded from disk");
        mgr.unpin(f1b, false, error);
        mgr.close_all(error);
    }

    // --- dirty write-back on eviction; modified bytes survive on disk ---
    const std::string dirty_path = temp_path("-dirty.bin");
    std::remove(dirty_path.c_str());
    seed_file(dirty_path, 3);
    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(2, std::make_unique<bufman::LRUPolicy>());
        bufman::File file;
        mgr.open_file(dirty_path, file, error);

        std::size_t f0 = 999, f1 = 999;
        mgr.pin(file, 0, f0, error);
        mgr.pin(file, 1, f1, error);
        mgr.unpin(f0, false, error);
        mgr.unpin(f1, false, error);
        mgr.pin(file, 0, f0, error);
        mgr.unpin(f0, false, error);

        std::size_t fz = 999;
        check(mgr.pin(file, 2, fz, error) && fz == 1,
              "block 2 evicts the LRU frame");
        mgr.frame(fz).buffer[0] = 'Z';
        mgr.unpin(fz, true, error);

        mgr.pin(file, 0, f0, error);
        mgr.unpin(f0, false, error);
        std::size_t fre = 999;
        check(mgr.pin(file, 1, fre, error) && fre == fz,
              "pinning another block evicts the dirty frame");
        check(read_disk_byte(dirty_path, 2) == 'Z',
              "dirty frame was written back to disk before reuse");
        check(mgr.frame(fre).buffer[0] == 'b', "fresh bytes loaded after eviction");

        mgr.pin(file, 0, f0, error);
        mgr.unpin(f0, false, error);
        check(mgr.pin(file, 2, fz, error), "re-pin block 2 after eviction");
        check(mgr.frame(fz).buffer[0] == 'Z',
              "modified bytes survive write-back and reload");
        mgr.unpin(fz, false, error);
        mgr.close_all(error);
    }

    // --- flush writes through immediately and is a no-op when clean ---
    const std::string flush_path = temp_path("-flush.bin");
    std::remove(flush_path.c_str());
    seed_file(flush_path, 1);
    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(1, std::make_unique<bufman::LRUPolicy>());
        bufman::File file;
        mgr.open_file(flush_path, file, error);

        std::size_t f = 999;
        mgr.pin(file, 0, f, error);
        mgr.frame(f).buffer[0] = 'F';
        mgr.unpin(f, true, error);
        check(mgr.flush(f, error), "flush succeeds");
        check(read_disk_byte(flush_path, 0) == 'F', "flush wrote the dirty frame");
        check(mgr.flush(f, error) && error.empty(), "flush on a clean frame is a no-op");
        mgr.close_all(error);

        std::size_t f2 = 999;
        check(!mgr.pin(file, 0, f2, error), "pin after close_all fails");
        check(!error.empty(), "pin after close_all reports an error");
    }

    // --- pinned frames are never evicted ---
    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(2, std::make_unique<bufman::LRUPolicy>());
        bufman::File file;
        mgr.open_file(seeded_path, file, error);

        std::size_t f0 = 999, f1 = 999, fx = 999;
        mgr.pin(file, 0, f0, error);
        mgr.pin(file, 1, f1, error);
        check(!mgr.pin(file, 2, fx, error), "pin with all frames pinned fails");
        check(error == "all frames pinned", "all-frames-pinned error message");
        mgr.unpin(f1, false, error);
        check(mgr.pin(file, 2, fx, error) && fx == f1,
              "after unpin, block 2 takes the LRU frame");
        mgr.unpin(f0, false, error);
        mgr.unpin(fx, false, error);
        mgr.close_all(error);
    }

    // --- unpin at pin count 0 throws ---
    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(1, std::make_unique<bufman::LRUPolicy>());
        bufman::File file;
        mgr.open_file(seeded_path, file, error);

        std::size_t f = 999;
        mgr.pin(file, 0, f, error);
        mgr.unpin(f, false, error);
        bool threw = false;
        try {
            mgr.unpin(f, false, error);
        } catch (const std::logic_error&) {
            threw = true;
        }
        check(threw, "unpin at pin count 0 throws logic_error");
        mgr.close_all(error);
    }

    // --- manager -> policy contract, observed through a recording fake ---
    {
        bufman::BufferManager mgr;
        std::string error;
        auto policy = std::make_unique<RecordingPolicy>();
        RecordingPolicy* policy_ptr = policy.get();
        mgr.init(2, std::move(policy));
        bufman::File file;
        mgr.open_file(seeded_path, file, error);

        std::size_t f0 = 999, f1 = 999;
        mgr.pin(file, 0, f0, error);
        mgr.pin(file, 0, f0, error);
        mgr.unpin(f0, false, error);
        mgr.unpin(f0, false, error);
        mgr.pin(file, 1, f1, error);
        mgr.unpin(f1, false, error);
        check(policy_ptr->events ==
                  std::vector<std::string>({"load:0", "access:0", "load:1"}),
              "policy sees load/access hooks in order");

        std::size_t fx = 999;
        mgr.pin(file, 2, fx, error);
        check(policy_ptr->last_candidates == std::vector<std::size_t>({0, 1}),
              "pick_victim sees the unpinned page-holding frames");
        check(policy_ptr->events.size() == 5 && policy_ptr->events[3] == "remove:0" &&
                  policy_ptr->events[4] == "load:0",
              "eviction reports remove then load");
        mgr.unpin(fx, false, error);

        mgr.pin(file, 1, f1, error);
        check(policy_ptr->events.back() == "access:1", "hit is reported as access");
        mgr.pin(file, 3, fx, error);
        check(policy_ptr->last_candidates == std::vector<std::size_t>({0}),
              "pinned frames are excluded from candidates");
        mgr.unpin(f1, false, error);
        mgr.unpin(fx, false, error);
    }

    // --- person records read and written through frame buffers ---
    const std::string people_path = temp_path("-people.bin");
    std::remove(people_path.c_str());
    seed_person_block(people_path, 0,
                      {make_person(1, "Ana", 20, "PR"), make_person(2, "Bob", 31, "NY")});
    seed_person_block(people_path, 1, {});
    seed_person_block(people_path, 2, {});

    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(2, std::make_unique<bufman::LRUPolicy>());
        bufman::File file;
        check(mgr.open_file(people_path, file, error), "open person file");

        std::size_t f0 = 999;
        check(mgr.pin(file, 0, f0, error) && f0 == 0, "person block 0 pins into frame 0");
        check(bufman::record_count(mgr.frame(f0).buffer) == 2, "seeded block 0 holds 2 records");
        Person p{};
        check(bufman::get_record(mgr.frame(f0).buffer, 0, p) && p.pid == 1 &&
                  std::string(p.name) == "Ana" && p.age == 20 && std::string(p.city) == "PR",
              "slot 0 reads back Ana through the frame buffer");
        check(bufman::get_record(mgr.frame(f0).buffer, 1, p) && p.pid == 2 &&
                  std::string(p.name) == "Bob",
              "slot 1 reads back Bob");
        check(!bufman::get_record(mgr.frame(f0).buffer, 2, p), "empty slot 2 reports no record");

        // Update in place, then march the dirty frame to the LRU position so
        // the next miss forces a write-back.
        check(bufman::put_record(mgr.frame(f0).buffer, 0, make_person(1, "Ana", 21, "PR")),
              "put_record overwrites slot 0 in the frame buffer");
        mgr.unpin(f0, true, error);
        std::size_t f1 = 999;
        mgr.pin(file, 1, f1, error);
        mgr.unpin(f1, false, error);
        mgr.pin(file, 0, f0, error);
        mgr.unpin(f0, false, error);
        mgr.pin(file, 1, f1, error);
        mgr.unpin(f1, false, error);
        std::size_t f2 = 999;
        check(mgr.pin(file, 2, f2, error) && f2 == 0,
              "block 2 evicts the LRU frame (the dirty block 0)");
        mgr.unpin(f2, false, error);

        std::size_t fre = 999;
        check(mgr.pin(file, 0, fre, error) && fre == 1, "re-pinning block 0 loads from disk");
        check(bufman::get_record(mgr.frame(fre).buffer, 0, p) && p.age == 21,
              "updated age survives write-back and reload");
        mgr.unpin(fre, false, error);
        const auto disk0 = read_disk_block(people_path, 0);
        check(bufman::get_record(disk0.data(), 0, p) && p.age == 21,
              "disk block 0 holds the updated record");
        mgr.close_all(error);
    }

    {
        // Pool of one: every pin after a dirty unpin evicts and writes back.
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(1, std::make_unique<bufman::LRUPolicy>());
        bufman::File file;
        mgr.open_file(people_path, file, error);
        Person p{};

        std::size_t f = 999;
        check(mgr.pin(file, 1, f, error), "pin zeroed block 1");
        auto slot = bufman::first_free_slot(mgr.frame(f).buffer);
        check(slot && *slot == 0, "first free slot of a zeroed block is 0");
        check(bufman::put_record(mgr.frame(f).buffer, *slot, make_person(3, "Cid", 44, "LA")),
              "append Cid into the free slot");
        mgr.unpin(f, true, error);
        mgr.pin(file, 2, f, error);
        mgr.unpin(f, false, error);
        auto disk1 = read_disk_block(people_path, 1);
        check(bufman::record_count(disk1.data()) == 1 && bufman::get_record(disk1.data(), 0, p) &&
                  p.pid == 3,
              "appended record was written back to disk");

        check(mgr.pin(file, 1, f, error), "re-pin block 1");
        slot = bufman::first_free_slot(mgr.frame(f).buffer);
        check(slot && *slot == 1, "next free slot is 1");
        check(bufman::put_record(mgr.frame(f).buffer, *slot, make_person(5, "Eve", 33, "OH")),
              "append Eve into the next free slot");
        mgr.unpin(f, true, error);
        mgr.pin(file, 2, f, error);
        mgr.unpin(f, false, error);
        disk1 = read_disk_block(people_path, 1);
        check(bufman::record_count(disk1.data()) == 2, "block 1 now holds 2 records");
        mgr.close_all(error);
    }

    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(1, std::make_unique<bufman::LRUPolicy>());
        bufman::File file;
        mgr.open_file(people_path, file, error);

        std::size_t f = 999;
        mgr.pin(file, 2, f, error);
        bool all_put = true;
        for (std::size_t i = 0; i < bufman::kRecordsPerBlock; ++i) {
            all_put = all_put && bufman::put_record(mgr.frame(f).buffer, i,
                                                    make_person(static_cast<int>(100 + i), "Px", 40, "TX"));
        }
        check(all_put, "put_record fills all 195 slots");
        check(!bufman::first_free_slot(mgr.frame(f).buffer), "full block has no free slot");
        mgr.unpin(f, true, error);
        mgr.pin(file, 0, f, error);
        mgr.unpin(f, false, error);
        const auto disk2 = read_disk_block(people_path, 2);
        check(bufman::record_count(disk2.data()) == bufman::kRecordsPerBlock,
              "full block persisted with 195 records");

        std::size_t f3 = 999;
        check(!mgr.pin(file, 3, f3, error), "pin past EOF fails: new blocks must be seeded first");
        mgr.close_all(error);
    }

    {
        std::array<char, bufman::kBlockSize> block{};
        bufman::initialize_block(block.data());
        Person zoe = make_person(9, "Zoe", 30, "PR");
        check(!bufman::get_record(block.data(), bufman::kRecordsPerBlock, zoe),
              "get_record rejects an out-of-range slot");
        check(!bufman::put_record(block.data(), bufman::kRecordsPerBlock, zoe),
              "put_record rejects an out-of-range slot");
        Person nobody = make_person(0, "Nobody", 1, "PR");
        check(!bufman::put_record(block.data(), 0, nobody),
              "put_record rejects pid 0 (free-slot sentinel)");
    }

    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(3, std::make_unique<bufman::LRUPolicy>());
        bufman::File file;
        mgr.open_file(people_path, file, error);

        std::size_t first_pass[3] = {999, 999, 999};
        std::cout << "\nscan of " << people_path << " through the buffer pool:\n";
        std::uint64_t total = 0;
        for (std::uint64_t b = 0; b < 3; ++b) {
            std::size_t f = 999;
            mgr.pin(file, b, f, error);
            first_pass[b] = f;
            const std::size_t count = bufman::record_count(mgr.frame(f).buffer);
            total += count;
            std::cout << "  block " << b << " (frame " << f << "): " << count << " record(s)\n";
            if (b < 2) {
                for (std::size_t s = 0; s < count; ++s) {
                    Person r{};
                    bufman::get_record(mgr.frame(f).buffer, s, r);
                    std::cout << "    slot " << s << ": pid=" << r.pid << ", name=" << r.name
                              << ", age=" << r.age << ", city=" << r.city << '\n';
                }
            }
            mgr.unpin(f, false, error);
        }
        std::cout << "  scanned 3 block(s), " << total << " record(s)\n";

        bool same_frames = true;
        for (std::uint64_t b = 0; b < 3; ++b) {
            std::size_t f = 999;
            mgr.pin(file, b, f, error);
            same_frames = same_frames && f == first_pass[b];
            mgr.unpin(f, false, error);
        }
        check(same_frames, "second scan pass hits the same frames (pool hits)");
        mgr.close_all(error);
    }

    // --- alloc_page creates zeroed, dirty, pinned pages through the pool ---
    const std::string alloc_path = temp_path("-alloc.bin");
    std::remove(alloc_path.c_str());
    {
        // Pool of one: the second alloc_page must evict the dirty page.
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(1, std::make_unique<bufman::LRUPolicy>());
        bufman::File file;
        mgr.open_file(alloc_path, file, error);
        Person p{};

        std::size_t f = 999;
        check(mgr.alloc_page(file, 0, f, error) && f == 0, "alloc_page creates block 0 in frame 0");
        check(mgr.frame(f).dirty, "new page starts dirty");
        check(mgr.frame(f).pin_count == 1, "new page starts pinned");
        check(bufman::record_count(mgr.frame(f).buffer) == 0, "new page buffer is zeroed");

        std::size_t fdup = 999;
        check(!mgr.alloc_page(file, 0, fdup, error),
              "alloc_page refuses an already resident block");

        check(bufman::put_record(mgr.frame(f).buffer, 0, make_person(7, "Gil", 28, "WA")),
              "record written into the new page");
        mgr.unpin(f, true, error);

        std::size_t fre = 999;
        check(mgr.alloc_page(file, 1, fre, error) && fre == 0,
              "alloc_page evicts the dirty page and reuses its frame");
        check(!mgr.alloc_page(file, 0, fre, error),
              "alloc_page refuses a block that already exists on disk");
        check(bufman::put_record(mgr.frame(fre).buffer, 0, make_person(8, "Hal", 35, "OH")),
              "record written into the second new page");
        mgr.unpin(fre, true, error);
        mgr.close_all(error);

        const auto disk0 = read_disk_block(alloc_path, 0);
        check(bufman::get_record(disk0.data(), 0, p) && p.pid == 7,
              "first new page was written back to disk");
        const auto disk1 = read_disk_block(alloc_path, 1);
        check(bufman::get_record(disk1.data(), 0, p) && p.pid == 8,
              "second new page was written back to disk");
    }

    std::remove(seeded_path.c_str());
    std::remove(dirty_path.c_str());
    std::remove(flush_path.c_str());
    std::remove(people_path.c_str());

    std::cout << (failures == 0 ? "\nAll checks passed." : "\nSome checks FAILED.")
              << "\n";
    return failures == 0 ? 0 : 1;
}
