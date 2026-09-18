#include "bufpool/BufferManager.h"
#include "file/BlockFile.h"
#include "heapfile/HeapFile.h"
#include "heapfile/SlottedPage.h"
#include "policy/LRUPolicy.h"
#include "policy/ReplacementPolicy.h"

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
    return std::string("/tmp/ex10-bufman-") + std::to_string(getpid()) + suffix;
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

// Reads a native-endian 4-byte integer out of a page buffer, so tests can
// assert raw byte offsets of the slotted-page header.
std::uint32_t read_u32_at(const char* buffer, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

// Loads a disk block into a fresh buffer and binds a SlottedPage view over
// it, so tests can inspect on-disk page bytes directly.
bool attach_disk_page(const std::string& path, std::uint64_t block_number,
                      std::array<char, bufman::kPageSize>& storage,
                      heapfile::SlottedPage& page, std::string& error) {
    const auto block = read_disk_block(path, block_number);
    std::memcpy(storage.data(), block.data(), storage.size());
    page.bind(storage.data(), bufman::kRecordSize);
    return page.attach(error);
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

    // --- SlottedPage layout offsets ---
    {
        std::array<char, bufman::kPageSize> buffer{};
        std::string error;
        heapfile::SlottedPage page;
        page.bind(buffer.data(), bufman::kRecordSize);
        page.format(7, 9);

        check(read_u32_at(buffer.data(), heapfile::kSlotCountOffset) == 185,
              "slot count sits at [kPageSize-12, kPageSize-8) and reads 185");
        check(read_u32_at(buffer.data(), heapfile::kPrevPageIdOffset) == 7,
              "prev page id sits at [kPageSize-8, kPageSize-4)");
        check(read_u32_at(buffer.data(), heapfile::kNextPageIdOffset) == 9,
              "next page id sits in the last 4 bytes");
        check(page.put(0, make_person(1, "Aa", 1, "PR"), error) &&
                  buffer[heapfile::kSlotCountOffset - 185] == 1,
              "occupancy array starts at kPageSize-12-185");
        check(page.put(184, make_person(2, "Zz", 2, "NY"), error) &&
                  buffer[heapfile::kSlotCountOffset - 1] == 1,
              "occupancy bytes end exactly at kPageSize-12");
        check(read_u32_at(buffer.data(), heapfile::kSlotCountOffset) == 185 &&
                  read_u32_at(buffer.data(), heapfile::kPrevPageIdOffset) == 7 &&
                  read_u32_at(buffer.data(), heapfile::kNextPageIdOffset) == 9,
              "occupancy writes did not touch the header");
    }

    // --- SlottedPage capacity and dead-space math ---
    {
        check(heapfile::slot_capacity(bufman::kRecordSize) == 185 &&
                  heapfile::dead_space(bufman::kRecordSize) == 14,
              "capacity 185 and dead space 14 for the 21-byte Person");
        check(heapfile::slot_capacity(1) == 2042 && heapfile::dead_space(1) == 0,
              "R=1 packs 2042 slots with no dead space");
        check(heapfile::slot_capacity(25) == 157 && heapfile::dead_space(25) == 2,
              "R=25 packs 157 slots with 2 bytes of dead space");
        check(185 * (bufman::kRecordSize + 1) + 14 + heapfile::kPageHeaderSize ==
                  bufman::kPageSize,
              "records + dead space + occupancy + header fill the page exactly");
    }

    // --- SlottedPage fresh-page state ---
    {
        std::array<char, bufman::kPageSize> buffer{};
        std::string error;
        heapfile::SlottedPage page;
        page.bind(buffer.data(), bufman::kRecordSize);
        page.format();
        check(page.capacity() == 185, "format derives capacity 185 from the record size");
        check(page.occupied_count() == 0, "fresh page has zero occupied slots");
        check(page.first_free_slot() && *page.first_free_slot() == 0,
              "first free slot of a fresh page is 0");
        check(page.has_free_slot(), "fresh page has a free slot");
        check(page.prev_page() == heapfile::kNoPage && page.next_page() == heapfile::kNoPage,
              "fresh page defaults prev/next to kNoPage");
        check(!page.is_occupied(0, error) && error.empty(),
              "slot 0 reads free with an empty error");
    }

    // --- SlottedPage put/get round-trip ---
    {
        std::array<char, bufman::kPageSize> buffer{};
        std::string error;
        heapfile::SlottedPage page;
        page.bind(buffer.data(), bufman::kRecordSize);
        page.format();

        bool round_trip = true;
        for (std::size_t slot = 0; slot < page.capacity(); ++slot) {
            const Person in = make_person(static_cast<int>(slot) + 1, "Rt",
                                          static_cast<int>(slot % 100), "TX");
            round_trip = round_trip && page.put(slot, in, error);
            Person out{};
            round_trip = round_trip && page.get(slot, out, error);
            round_trip = round_trip && out.pid == in.pid && out.age == in.age &&
                         std::string(out.name) == std::string(in.name) &&
                         std::string(out.city) == std::string(in.city);
        }
        check(round_trip, "put then get preserves every Person on all 185 slots");
        check(page.occupied_count() == 185, "all 185 slots flip to occupied");
        check(!page.first_free_slot() && !page.has_free_slot(),
              "a full page has no free slot");
    }

    // --- SlottedPage erase and reuse ---
    {
        std::array<char, bufman::kPageSize> buffer{};
        std::string error;
        heapfile::SlottedPage page;
        page.bind(buffer.data(), bufman::kRecordSize);
        page.format();
        bool filled = true;
        for (std::size_t slot = 0; slot < 100; ++slot) {
            filled = filled &&
                     page.put(slot, make_person(static_cast<int>(slot) + 1, "Er", 30, "PR"),
                              error);
        }
        check(filled, "fill 100 slots");
        check(page.erase(90, error), "erase of an occupied slot succeeds");
        Person out{};
        check(!page.get(90, out, error) && !error.empty(),
              "get on an erased slot fails with an error");
        check(page.first_free_slot() && *page.first_free_slot() == 90,
              "first free slot is the erased middle slot");
        check(page.put(90, make_person(77, "Re", 41, "OH"), error) &&
                  page.get(90, out, error) && out.pid == 77,
              "re-put into the erased slot works");
        check(page.occupied_count() == 100, "occupied count is back to 100 after re-put");
    }

    // --- SlottedPage bounds checks ---
    {
        std::array<char, bufman::kPageSize> buffer{};
        std::string error;
        heapfile::SlottedPage page;
        page.bind(buffer.data(), bufman::kRecordSize);
        page.format();
        const std::size_t beyond = page.capacity();
        Person out{};
        check(!page.get(beyond, out, error) && !error.empty(),
              "get rejects an out-of-range slot");
        check(!page.put(beyond, make_person(1, "Bd", 1, "PR"), error) && !error.empty(),
              "put rejects an out-of-range slot");
        check(!page.erase(beyond, error) && !error.empty(),
              "erase rejects an out-of-range slot");
        check(!page.is_occupied(beyond, error) && !error.empty(),
              "is_occupied rejects an out-of-range slot");
    }

    // --- SlottedPage dead space untouched ---
    {
        std::array<char, bufman::kPageSize> buffer{};
        std::string error;
        heapfile::SlottedPage page;
        page.bind(buffer.data(), bufman::kRecordSize);
        page.format();
        const std::size_t dead_start = bufman::kRecordSize * page.capacity();
        const std::size_t dead_size = heapfile::dead_space(bufman::kRecordSize);
        for (std::size_t i = 0; i < dead_size; ++i) {
            buffer[dead_start + i] = static_cast<char>(0x50 + i);
        }
        check(page.attach(error), "attach accepts the freshly formatted header");
        check(page.put(0, make_person(1, "Ds", 1, "PR"), error) && page.erase(0, error) &&
                  page.put(1, make_person(2, "Dt", 2, "PR"), error),
              "put/erase cycles run with a seeded dead space");
        bool markers_intact = true;
        for (std::size_t i = 0; i < dead_size; ++i) {
            markers_intact = markers_intact &&
                             buffer[dead_start + i] == static_cast<char>(0x50 + i);
        }
        check(markers_intact, "dead-space bytes survive attach and put/erase cycles");
    }

    // --- SlottedPage attach validation ---
    {
        std::array<char, bufman::kPageSize> buffer{};
        std::string error;
        heapfile::SlottedPage page;
        page.bind(buffer.data(), bufman::kRecordSize);
        page.format();

        heapfile::SlottedPage foreign;
        foreign.bind(buffer.data(), 25);
        check(!foreign.attach(error) && !error.empty(),
              "attach rejects a header whose slot count does not match the record size");
    }

    // --- SlottedPage through the pool: chain links and sentinels survive
    //     write-back and reload ---
    const std::string slotted_path = temp_path("-slotted.bin");
    std::remove(slotted_path.c_str());
    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(1, std::make_unique<bufman::LRUPolicy>());
        bufman::File file;
        check(mgr.open_file(slotted_path, file, error), "open slotted-page test file");

        std::size_t f = 999;
        check(mgr.alloc_page(file, 0, f, error), "alloc page 0");
        heapfile::SlottedPage page;
        page.bind(mgr.frame(f).buffer, bufman::kRecordSize);
        page.format();
        page.set_next_page(1);
        mgr.unpin(f, true, error);

        check(mgr.alloc_page(file, 1, f, error),
              "alloc page 1 (evicts the dirty page 0)");
        page.bind(mgr.frame(f).buffer, bufman::kRecordSize);
        page.format();
        page.set_prev_page(0);
        mgr.unpin(f, true, error);

        check(mgr.pin(file, 0, f, error) && f == 0, "re-pin page 0 (loads from disk)");
        page.bind(mgr.frame(f).buffer, bufman::kRecordSize);
        check(page.attach(error), "page 0 attaches with the expected slot count");
        check(page.prev_page() == heapfile::kNoPage,
              "prev of page 0 reads kNoPage after a reload");
        check(page.next_page() == 1, "head->tail walk: page 0's next is page 1");
        mgr.unpin(f, false, error);

        check(mgr.pin(file, 1, f, error) && f == 0, "re-pin page 1");
        page.bind(mgr.frame(f).buffer, bufman::kRecordSize);
        check(page.attach(error), "page 1 attaches with the expected slot count");
        check(page.prev_page() == 0, "tail->head walk: page 1's prev is page 0");
        check(page.next_page() == heapfile::kNoPage,
              "next of the last page reads kNoPage after a reload");
        mgr.unpin(f, false, error);
        mgr.close_all(error);
    }

    // --- SlottedPage records persist through eviction ---
    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(2, std::make_unique<bufman::LRUPolicy>());
        bufman::File file;
        check(mgr.open_file(slotted_path, file, error), "reopen slotted-page test file");

        std::size_t f = 999;
        check(mgr.pin(file, 1, f, error), "pin page 1");
        heapfile::SlottedPage page;
        page.bind(mgr.frame(f).buffer, bufman::kRecordSize);
        check(page.attach(error), "page 1 attaches for record filling");
        const std::size_t fill_count = 50;
        bool filled = true;
        for (std::size_t slot = 0; slot < fill_count; ++slot) {
            filled = filled && page.put(slot, make_person(static_cast<int>(slot) + 1, "Pp",
                                                          static_cast<int>(slot), "PR"),
                                        error);
        }
        check(filled, "put 50 records into page 1");
        check(page.occupied_count() == fill_count, "page 1 reports 50 occupied slots");
        mgr.unpin(f, true, error);

        std::size_t f2 = 999;
        check(mgr.alloc_page(file, 2, f2, error), "alloc page 2 into the second frame");
        mgr.unpin(f2, true, error);

        std::size_t f0 = 999;
        check(mgr.pin(file, 0, f0, error) && f0 == f,
              "pinning page 0 evicts the LRU dirty page 1 (write-back)");
        mgr.unpin(f0, false, error);

        std::size_t fre = 999;
        check(mgr.pin(file, 1, fre, error), "re-pin page 1 (loads from disk)");
        page.bind(mgr.frame(fre).buffer, bufman::kRecordSize);
        check(page.attach(error), "page 1 attaches after reload");
        check(page.occupied_count() == fill_count,
              "all 50 records survived eviction and reload");
        Person out{};
        check(page.get(0, out, error) && out.pid == 1 && std::string(out.name) == "Pp",
              "slot 0 record survived write-back and reload");
        check(page.get(fill_count - 1, out, error) && out.pid == static_cast<int>(fill_count),
              "last filled slot survived too");
        check(page.prev_page() == 0 && page.next_page() == heapfile::kNoPage,
              "page 1's chain links survived write-back and reload");
        mgr.unpin(fre, false, error);
        mgr.close_all(error);

        const auto disk1 = read_disk_block(slotted_path, 1);
        std::array<char, bufman::kPageSize> disk_copy{};
        std::memcpy(disk_copy.data(), disk1.data(), disk_copy.size());
        heapfile::SlottedPage disk_page;
        disk_page.bind(disk_copy.data(), bufman::kRecordSize);
        check(disk_page.attach(error), "on-disk page 1 attaches directly");
        check(disk_page.occupied_count() == fill_count,
              "on-disk page 1 holds the 50 records");
    }

    // --- HeapFile create ---
    const std::string hf_create_path = temp_path("-hf-create.bin");
    std::remove(hf_create_path.c_str());
    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(3, std::make_unique<bufman::LRUPolicy>());
        heapfile::HeapFile heap(mgr);

        check(heap.create(hf_create_path, error), "create makes a new heap file");
        std::array<char, bufman::kPageSize> storage{};
        heapfile::SlottedPage header;
        check(attach_disk_page(hf_create_path, 0, storage, header, error),
              "created file's block 0 attaches as a slotted page");
        check(header.prev_page() == heapfile::kNoPage &&
                  header.next_page() == heapfile::kNoPage,
              "fresh header prev/next are kNoPage");
        {
            bufman::File probe;
            check(bufman::open_for_read(hf_create_path, probe, error),
                  "created file opens for read");
            check(bufman::block_count(probe, error) == 1,
                  "created file has exactly one block");
            bufman::close(probe);
        }
        check(!heap.create(hf_create_path, error) && !error.empty(),
              "create refuses a path that already holds a heap file");
        mgr.close_all(error);
    }

    // --- HeapFile open validation ---
    const std::string hf_open_path = temp_path("-hf-open.bin");
    const std::string hf_old_path = temp_path("-hf-oldformat.bin");
    std::remove(hf_open_path.c_str());
    std::remove(hf_old_path.c_str());
    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(3, std::make_unique<bufman::LRUPolicy>());
        heapfile::HeapFile heap(mgr);

        check(!heap.open(temp_path("-hf-missing.bin"), error) && !error.empty(),
              "open of a missing path fails");

        // Block 0 in the old serializer format: its header region holds
        // record bytes, so the slot count cannot read 185.
        seed_person_block(hf_old_path, 0, {make_person(1, "Old", 40, "TX")});
        check(!heap.open(hf_old_path, error) && !error.empty(),
              "open rejects a file whose block 0 is not a header page");

        check(heap.create(hf_open_path, error), "create for the open round-trip");
        check(heap.open(hf_open_path, error), "open succeeds on a created file");
        check(heap.is_open(), "heap file reports open");
        check(heap.close(error) && !heap.is_open(), "close marks the file closed");
        check(heap.open(hf_open_path, error), "open -> close -> open round-trip works");
        mgr.close_all(error);
    }

    // --- HeapFile first insert ---
    const std::string hf_first_path = temp_path("-hf-first.bin");
    std::remove(hf_first_path.c_str());
    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(3, std::make_unique<bufman::LRUPolicy>());
        heapfile::HeapFile heap(mgr);
        check(heap.create(hf_first_path, error) && heap.open(hf_first_path, error),
              "create and open for the first insert");

        heapfile::RowId rid{};
        check(heap.insert(make_person(1, "Ana", 20, "PR"), rid, error),
              "first insert succeeds");
        check(rid == (heapfile::RowId{1, 0}), "first insert lands at row id (1, 0)");

        check(mgr.flush_all(error), "flush the pool before disk checks");
        std::array<char, bufman::kPageSize> storage{};
        heapfile::SlottedPage page;
        check(attach_disk_page(hf_first_path, 0, storage, page, error) &&
                  page.next_page() == 1,
              "header next points at page 1 after the first insert");
        check(attach_disk_page(hf_first_path, 1, storage, page, error) &&
                  page.prev_page() == 0 && page.next_page() == heapfile::kNoPage,
              "page 1 links back to the header and ends the chain");
        mgr.close_all(error);
    }

    // --- HeapFile fill and spill ---
    const std::string hf_spill_path = temp_path("-hf-spill.bin");
    std::remove(hf_spill_path.c_str());
    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(3, std::make_unique<bufman::LRUPolicy>());
        heapfile::HeapFile heap(mgr);
        check(heap.create(hf_spill_path, error) && heap.open(hf_spill_path, error),
              "create and open for fill and spill");

        bool filled = true;
        bool sequential = true;
        for (std::size_t i = 0; i < 185; ++i) {
            heapfile::RowId rid{};
            filled = filled && heap.insert(make_person(static_cast<int>(i) + 1, "Fs",
                                                       static_cast<int>(i), "TX"),
                                           rid, error);
            sequential = sequential &&
                         rid == (heapfile::RowId{1, static_cast<std::uint32_t>(i)});
        }
        check(filled, "185 inserts fill page 1");
        check(sequential, "page-1 inserts get row ids (1, 0)..(1, 184)");

        heapfile::RowId rid{};
        check(heap.insert(make_person(200, "Spill", 50, "NY"), rid, error),
              "the 186th insert succeeds");
        check(rid == (heapfile::RowId{2, 0}), "the 186th insert spills into row id (2, 0)");

        check(mgr.flush_all(error), "flush the pool before chain checks");
        std::array<char, bufman::kPageSize> storage{};
        heapfile::SlottedPage page;
        check(attach_disk_page(hf_spill_path, 0, storage, page, error) &&
                  page.next_page() == 1,
              "chain reads 0 -> 1");
        check(attach_disk_page(hf_spill_path, 1, storage, page, error) &&
                  page.next_page() == 2,
              "chain reads 1 -> 2");
        check(attach_disk_page(hf_spill_path, 2, storage, page, error) &&
                  page.prev_page() == 1 && page.next_page() == heapfile::kNoPage,
              "page 2 ends the chain");
        mgr.close_all(error);
    }

    // --- HeapFile insert reuses freed slots ---
    const std::string hf_reuse_path = temp_path("-hf-reuse.bin");
    std::remove(hf_reuse_path.c_str());
    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(3, std::make_unique<bufman::LRUPolicy>());
        heapfile::HeapFile heap(mgr);
        check(heap.create(hf_reuse_path, error) && heap.open(hf_reuse_path, error),
              "create and open for the reuse test");

        bool filled = true;
        for (std::size_t i = 0; i < 92; ++i) {
            heapfile::RowId rid{};
            filled = filled &&
                     heap.insert(make_person(static_cast<int>(i) + 1, "Ha", 30, "PR"),
                                 rid, error);
        }
        check(filled, "fill page 1 halfway (92 records)");

        check(heap.erase(heapfile::RowId{1, 50}, error), "erase a middle record");
        heapfile::RowId rid{};
        check(heap.insert(make_person(500, "Back", 41, "OH"), rid, error),
              "insert after erase succeeds");
        check(rid == (heapfile::RowId{1, 50}), "the freed slot is the next to be reused");
        mgr.close_all(error);
    }

    // --- HeapFile scan ---
    const std::string hf_scan_path = temp_path("-hf-scan.bin");
    std::remove(hf_scan_path.c_str());
    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(3, std::make_unique<bufman::LRUPolicy>());
        heapfile::HeapFile heap(mgr);
        check(heap.create(hf_scan_path, error) && heap.open(hf_scan_path, error),
              "create and open for the scan test");

        std::size_t count = 0;
        heapfile::HeapFileIterator it = heap.scan();
        Person p{};
        while (it.next(p, error)) {
            ++count;
        }
        check(error.empty() && count == 0, "an empty file scans zero records");

        const int first_pid = 11;
        bool inserted = true;
        for (int i = 0; i < 3; ++i) {
            heapfile::RowId rid{};
            inserted = inserted &&
                       heap.insert(make_person(first_pid + i, "Sc", 20 + i, "PR"), rid, error);
        }
        check(inserted, "insert three records for the scan");

        count = 0;
        bool in_order = true;
        it = heap.scan();
        while (it.next(p, error)) {
            in_order = in_order && p.pid == first_pid + static_cast<int>(count) &&
                       p.age == 20 + static_cast<int>(count);
            ++count;
        }
        check(error.empty() && count == 3, "scan yields exactly the three records");
        check(in_order, "scan reports records in insertion order with matching fields");
        check(!it.next(p, error) && error.empty(),
              "next after exhaustion reports exhaustion without error");
        check(!it.next(p, error) && error.empty(),
              "repeated next after exhaustion keeps reporting exhaustion");
        mgr.close_all(error);
    }

    // --- HeapFile find ---
    const std::string hf_rows_path = temp_path("-hf-rows.bin");
    std::remove(hf_rows_path.c_str());
    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(3, std::make_unique<bufman::LRUPolicy>());
        heapfile::HeapFile heap(mgr);
        check(heap.create(hf_rows_path, error) && heap.open(hf_rows_path, error),
              "create and open for the row-id tests");

        heapfile::RowId r0{}, r1{}, r2{};
        check(heap.insert(make_person(1, "On", 21, "PR"), r0, error) &&
                  heap.insert(make_person(2, "Tw", 22, "NY"), r1, error) &&
                  heap.insert(make_person(3, "Th", 23, "OH"), r2, error),
              "insert three records");
        Person p{};
        check(heap.find(r0, p, error) && p.pid == 1 && std::string(p.name) == "On",
              "find returns the exact record for the first row id");
        check(heap.find(r1, p, error) && p.pid == 2, "find returns the second record");
        check(heap.find(r2, p, error) && p.pid == 3, "find returns the third record");

        check(heap.erase(r1, error), "erase the second record");
        std::string err_free, err_header, err_slot, err_eof;
        check(!heap.find(r1, p, err_free) && !err_free.empty(),
              "find on an erased slot fails");
        check(!heap.find(heapfile::RowId{0, 0}, p, err_header) && !err_header.empty(),
              "find on (0, 0) fails");
        check(!heap.find(heapfile::RowId{1, 5000}, p, err_slot) && !err_slot.empty(),
              "find on a slot beyond capacity fails");
        check(!heap.find(heapfile::RowId{999, 0}, p, err_eof) && !err_eof.empty(),
              "find on a page beyond EOF fails");
        check(err_free != err_header && err_header != err_slot && err_slot != err_eof &&
                  err_free != err_eof && err_free != err_slot && err_header != err_eof,
              "the four not-found errors are distinct, readable messages");
        mgr.close_all(error);
    }

    // --- HeapFile update ---
    std::remove(hf_rows_path.c_str());
    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(3, std::make_unique<bufman::LRUPolicy>());
        heapfile::HeapFile heap(mgr);
        check(heap.create(hf_rows_path, error) && heap.open(hf_rows_path, error),
              "create and open for the update test");

        heapfile::RowId r0{}, r1{}, r2{};
        check(heap.insert(make_person(1, "On", 21, "PR"), r0, error) &&
                  heap.insert(make_person(2, "Tw", 22, "NY"), r1, error) &&
                  heap.insert(make_person(3, "Th", 23, "OH"), r2, error),
              "insert three records for the update test");

        check(heap.update(r1, make_person(2, "Tw", 23, "CA"), error),
              "update overwrites the middle record");
        Person p{};
        check(heap.find(r1, p, error) && p.age == 23 && std::string(p.city) == "CA",
              "find returns the updated values");
        check(heap.find(r0, p, error) && p.pid == 1 && p.age == 21,
              "the first record is untouched");
        check(heap.find(r2, p, error) && p.pid == 3 && p.age == 23,
              "the last record is untouched");

        check(heap.erase(r2, error), "erase the last record");
        check(!heap.update(r2, make_person(9, "Xx", 1, "PR"), error) && !error.empty(),
              "updating a free slot fails");
        mgr.close_all(error);
    }

    // --- HeapFile erase ---
    std::remove(hf_rows_path.c_str());
    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(3, std::make_unique<bufman::LRUPolicy>());
        heapfile::HeapFile heap(mgr);
        check(heap.create(hf_rows_path, error) && heap.open(hf_rows_path, error),
              "create and open for the erase test");

        heapfile::RowId r0{}, r1{};
        check(heap.insert(make_person(1, "On", 21, "PR"), r0, error) &&
                  heap.insert(make_person(2, "Tw", 22, "NY"), r1, error),
              "insert two records for the erase test");
        check(heap.erase(r0, error), "erase the first record");
        Person p{};
        check(!heap.find(r0, p, error) && !error.empty(),
              "an erased row id stops being findable");
        std::size_t count = 0;
        heapfile::HeapFileIterator it = heap.scan();
        while (it.next(p, error)) {
            ++count;
        }
        check(error.empty() && count == 1, "scan count drops by one after erase");
        mgr.close_all(error);
    }

    // --- HeapFile persistence with evictions, then chain integrity
    //     ---
    const std::string hf_persist_path = temp_path("-hf-persist.bin");
    std::remove(hf_persist_path.c_str());
    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(2, std::make_unique<bufman::LRUPolicy>());
        heapfile::HeapFile heap(mgr);
        check(heap.create(hf_persist_path, error) && heap.open(hf_persist_path, error),
              "create and open with a pool of two frames");

        const std::size_t total = 400;
        bool inserted = true;
        heapfile::RowId first{}, mid{}, last{};
        for (std::size_t i = 0; i < total; ++i) {
            heapfile::RowId rid{};
            inserted = inserted &&
                       heap.insert(make_person(static_cast<int>(i) + 1, "Hp",
                                               static_cast<int>(i % 100), "TX"),
                                   rid, error);
            if (i == 0) {
                first = rid;
            }
            if (i == 185) {
                mid = rid;
            }
            if (i == total - 1) {
                last = rid;
            }
        }
        check(inserted, "insert 400 records with a two-frame pool");
        check(first == (heapfile::RowId{1, 0}) && mid == (heapfile::RowId{2, 0}) &&
                  last == (heapfile::RowId{3, 29}),
              "row ids cross pages 1, 2, and 3 as expected");

        // Chain integrity after the splices: walk both directions through
        // the pool with a test-owned handle on the same file.
        check(mgr.flush_all(error), "flush the pool before the chain walk");
        bufman::File viewer;
        check(mgr.open_file(hf_persist_path, viewer, error), "open a test handle");
        const std::uint32_t expected_prev[4] = {heapfile::kNoPage, 0, 1, 2};
        const std::uint32_t expected_next[4] = {1, 2, 3, heapfile::kNoPage};
        bool chain_ok = true;
        for (std::uint32_t b = 0; b < 4; ++b) {
            std::size_t frame = 999;
            chain_ok = chain_ok && mgr.pin(viewer, b, frame, error);
            heapfile::SlottedPage page;
            page.bind(mgr.frame(frame).buffer, bufman::kRecordSize);
            chain_ok = chain_ok && page.attach(error);
            chain_ok = chain_ok && page.prev_page() == expected_prev[b] &&
                       page.next_page() == expected_next[b];
            mgr.unpin(frame, false, error);
        }
        check(chain_ok, "pages 0..3 link 0 <-> 1 <-> 2 <-> 3 in both directions");

        check(heap.close(error), "close after 400 inserts");
        check(heap.open(hf_persist_path, error), "reopen the persisted file");
        Person p{};
        check(heap.find(first, p, error) && p.pid == 1,
              "the first record survived close and reopen");
        check(heap.find(mid, p, error) && p.pid == 186,
              "the first record of page 2 survived too");
        check(heap.find(last, p, error) && p.pid == 400,
              "the last record survived too");
        std::size_t count = 0;
        heapfile::HeapFileIterator it = heap.scan();
        while (it.next(p, error)) {
            ++count;
        }
        check(error.empty() && count == total, "reopen scans exactly 400 records");
        mgr.close_all(error);
    }

    // --- HeapFile closed discipline ---
    const std::string hf_closed_path = temp_path("-hf-closed.bin");
    std::remove(hf_closed_path.c_str());
    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(3, std::make_unique<bufman::LRUPolicy>());
        heapfile::HeapFile heap(mgr);
        check(heap.create(hf_closed_path, error) && heap.open(hf_closed_path, error),
              "create and open for the closed-discipline test");
        check(heap.close(error), "close the heap file");

        heapfile::RowId rid{1, 0};
        Person p{};
        check(!heap.insert(p, rid, error) && error == "heap file is not open",
              "insert after close fails");
        check(!heap.find(rid, p, error) && error == "heap file is not open",
              "find after close fails");
        check(!heap.update(rid, p, error) && error == "heap file is not open",
              "update after close fails");
        check(!heap.erase(rid, error) && error == "heap file is not open",
              "erase after close fails");
        heapfile::HeapFileIterator it = heap.scan();
        check(!it.next(p, error) && error == "heap file is not open",
              "scan after close fails");
        check(!heap.close(error) && error == "heap file is not open",
              "close after close fails");
        mgr.close_all(error);
    }

    // --- erase file ---
    const std::string hf_erase_path = temp_path("-hf-erase.bin");
    std::remove(hf_erase_path.c_str());
    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(3, std::make_unique<bufman::LRUPolicy>());
        heapfile::HeapFile heap(mgr);
        check(heap.create(hf_erase_path, error) && heap.open(hf_erase_path, error),
              "create and open for the erase-file test");
        check(!heapfile::erase_file(mgr, hf_erase_path, error) && !error.empty(),
              "erase-file refuses while the file is open");
        check(heap.close(error), "close before erasing the file");
        check(heapfile::erase_file(mgr, hf_erase_path, error),
              "erase-file removes the closed file from disk");
        check(!heapfile::erase_file(mgr, hf_erase_path, error) && !error.empty(),
              "a second erase-file fails");
        mgr.close_all(error);
    }

    std::remove(seeded_path.c_str());
    std::remove(dirty_path.c_str());
    std::remove(flush_path.c_str());
    std::remove(people_path.c_str());
    std::remove(alloc_path.c_str());
    std::remove(slotted_path.c_str());
    std::remove(hf_create_path.c_str());
    std::remove(hf_open_path.c_str());
    std::remove(hf_old_path.c_str());
    std::remove(hf_first_path.c_str());
    std::remove(hf_spill_path.c_str());
    std::remove(hf_reuse_path.c_str());
    std::remove(hf_scan_path.c_str());
    std::remove(hf_rows_path.c_str());
    std::remove(hf_persist_path.c_str());
    std::remove(hf_closed_path.c_str());
    std::remove(hf_erase_path.c_str());

    std::cout << (failures == 0 ? "\nAll checks passed." : "\nSome checks FAILED.")
              << "\n";
    return failures == 0 ? 0 : 1;
}
