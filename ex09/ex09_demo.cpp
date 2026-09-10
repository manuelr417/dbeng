#include "BlockFile.h"
#include "BufferManager.h"
#include "LRUPolicy.h"
#include "PersonCsv.h"
#include "PersonGenerator.h"
#include "PersonSerializer.h"

#include <charconv>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

// Pool size for the demo process; large enough that a small scan stays
// resident, small enough that eviction paths get exercised.
constexpr std::size_t kPoolSize = 8;

void print_usage(const char* program) {
    std::cerr << "Usage:\n"
              << "  " << program << " append <csv-file> <binary-file>\n"
              << "  " << program << " read <binary-file> <block-number>\n"
              << "  " << program << " seek <binary-file> <block-number>...\n"
              << "  " << program << " scan <binary-file>\n"
              << "  " << program << " bulk <count> <binary-file>\n";
}

bool parse_uint64(const char* text, std::uint64_t& value) {
    const char* end = text + std::char_traits<char>::length(text);
    const auto result = std::from_chars(text, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

void print_person(const Person& person, std::size_t index) {
    std::cout << "  slot " << index << ": pid=" << person.pid
              << ", name=" << person.name
              << ", age=" << person.age
              << ", city=" << person.city << '\n';
}

// Appends records through the buffer pool, ex07-style: fill the last
// block's free slots first, then allocate new pages for the rest. Every
// touched frame is unpinned dirty; close_all flushes before the command
// returns.
bool append_people(bufman::BufferManager& mgr, bufman::File& file,
                   const std::vector<Person>& people, std::string& error) {
    std::uint64_t blocks = bufman::block_count(file, error);
    if (!error.empty()) {
        return false;
    }

    std::size_t next = 0;
    if (blocks > 0) {
        std::size_t f = 0;
        if (!mgr.pin(file, blocks - 1, f, error)) {
            return false;
        }
        std::size_t slot = bufman::record_count(mgr.frame(f).buffer);
        bool touched = false;
        while (slot < bufman::kRecordsPerBlock && next < people.size()) {
            if (!bufman::put_record(mgr.frame(f).buffer, slot++, people[next++])) {
                error = "put_record failed while filling the last block";
                return false;
            }
            touched = true;
        }
        mgr.unpin(f, touched, error);
        if (!error.empty()) {
            return false;
        }
    }

    while (next < people.size()) {
        std::size_t f = 0;
        if (!mgr.alloc_page(file, blocks, f, error)) {
            return false;
        }
        ++blocks;
        std::size_t slot = 0;
        while (slot < bufman::kRecordsPerBlock && next < people.size()) {
            if (!bufman::put_record(mgr.frame(f).buffer, slot++, people[next++])) {
                error = "put_record failed while filling a new page";
                return false;
            }
        }
        mgr.unpin(f, true, error);
        if (!error.empty()) {
            return false;
        }
    }
    return true;
}

int append_csv_file(const std::string& csv_path, const std::string& binary_path) {
    const bufman::LoadResult loaded = bufman::load(csv_path, std::cerr);
    if (loaded.people.empty()) {
        return loaded.skipped == 0 ? 0 : 1;
    }

    bufman::BufferManager mgr;
    std::string error;
    mgr.init(kPoolSize, std::make_unique<bufman::LRUPolicy>());
    bufman::File file;
    if (!mgr.open_file(binary_path, file, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    const bool success = append_people(mgr, file, loaded.people, error);
    mgr.close_all(error);
    if (!success) {
        std::cerr << error << '\n';
        return 1;
    }
    std::cout << "appended " << loaded.people.size() << " record(s)"
              << ", skipped " << loaded.skipped << " row(s)\n";
    return 0;
}

int bulk_insert(const std::string& binary_path, std::uint64_t count) {
    if (count == 0) {
        std::cout << "nothing to insert\n";
        return 0;
    }

    bufman::BufferManager mgr;
    std::string error;
    mgr.init(kPoolSize, std::make_unique<bufman::LRUPolicy>());
    bufman::File file;
    if (!mgr.open_file(binary_path, file, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    const std::uint64_t existing = bufman::record_count(file, error);
    if (!error.empty()) {
        mgr.close_all(error);
        std::cerr << error << '\n';
        return 1;
    }

    const auto people = bufman::generate(count, static_cast<int>(existing) + 1);
    const bool success = append_people(mgr, file, people, error);
    mgr.close_all(error);
    if (!success) {
        std::cerr << error << '\n';
        return 1;
    }
    std::cout << "appended " << people.size() << " generated record(s)\n";
    return 0;
}

int read_and_print_block(const std::string& binary_path, std::uint64_t block_number) {
    bufman::BufferManager mgr;
    std::string error;
    mgr.init(kPoolSize, std::make_unique<bufman::LRUPolicy>());
    bufman::File file;
    if (!mgr.open_file_read(binary_path, file, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    std::size_t f = 0;
    const bool success = mgr.pin(file, block_number, f, error);
    if (success) {
        const auto people = bufman::deserialize_block(mgr.frame(f).buffer);
        std::cout << "block " << block_number << ": " << people.size() << " record(s)\n";
        for (std::size_t i = 0; i < people.size(); ++i) {
            print_person(people[i], i);
        }
        mgr.unpin(f, false, error);
    }
    mgr.close_all(error);
    if (!success) {
        std::cerr << error << '\n';
        return 1;
    }
    return 0;
}

// Reads each requested block in command-line order, duplicates included.
// A block that fails (bad number, beyond EOF) prints a message and the loop
// continues; the command exits 1 only if anything failed.
int seek_blocks(const std::string& binary_path, char* const* block_args, int count) {
    bufman::BufferManager mgr;
    std::string error;
    mgr.init(kPoolSize, std::make_unique<bufman::LRUPolicy>());
    bufman::File file;
    if (!mgr.open_file_read(binary_path, file, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    int failures = 0;
    for (int i = 0; i < count; ++i) {
        std::uint64_t block_number = 0;
        if (!parse_uint64(block_args[i], block_number)) {
            std::cerr << "block " << block_args[i] << ": not a nonnegative integer\n";
            ++failures;
            continue;
        }
        const bool hit = mgr.resident(file, block_number);
        std::size_t f = 0;
        if (!mgr.pin(file, block_number, f, error)) {
            std::cerr << "block " << block_number << ": " << error << '\n';
            ++failures;
            continue;
        }
        const auto people = bufman::deserialize_block(mgr.frame(f).buffer);
        std::cout << "block " << block_number;
        if (hit) {
            std::cout << " (pool hit)";
        }
        std::cout << ": " << people.size() << " record(s)\n";
        for (std::size_t p = 0; p < people.size(); ++p) {
            print_person(people[p], p);
        }
        mgr.unpin(f, false, error);
    }
    mgr.close_all(error);
    return failures == 0 ? 0 : 1;
}

int scan_file(const std::string& binary_path) {
    bufman::BufferManager mgr;
    std::string error;
    mgr.init(kPoolSize, std::make_unique<bufman::LRUPolicy>());
    bufman::File file;
    if (!mgr.open_file_read(binary_path, file, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    const std::uint64_t blocks = bufman::block_count(file, error);
    if (!error.empty()) {
        mgr.close_all(error);
        std::cerr << error << '\n';
        return 1;
    }

    std::uint64_t total_records = 0;
    for (std::uint64_t block_number = 0; block_number < blocks; ++block_number) {
        std::size_t f = 0;
        if (!mgr.pin(file, block_number, f, error)) {
            mgr.close_all(error);
            std::cerr << error << '\n';
            return 1;
        }
        const auto people = bufman::deserialize_block(mgr.frame(f).buffer);
        std::cout << "block " << block_number << ": " << people.size() << " record(s)\n";
        for (std::size_t i = 0; i < people.size(); ++i) {
            print_person(people[i], i);
        }
        total_records += people.size();
        mgr.unpin(f, false, error);
    }
    mgr.close_all(error);
    std::cout << "scanned " << blocks << " block(s), " << total_records << " record(s)\n";
    return 0;
}

}

int main(int argc, char* argv[]) {
    if (argc == 4 && std::string(argv[1]) == "append") {
        return append_csv_file(argv[2], argv[3]);
    }
    if (argc == 4 && std::string(argv[1]) == "read") {
        std::uint64_t block_number = 0;
        if (!parse_uint64(argv[3], block_number)) {
            std::cerr << "block number must be a nonnegative integer\n";
            return 1;
        }
        return read_and_print_block(argv[2], block_number);
    }
    if (argc >= 4 && std::string(argv[1]) == "seek") {
        return seek_blocks(argv[2], argv + 3, argc - 3);
    }
    if (argc == 3 && std::string(argv[1]) == "scan") {
        return scan_file(argv[2]);
    }
    if (argc == 4 && std::string(argv[1]) == "bulk") {
        std::uint64_t count = 0;
        if (!parse_uint64(argv[2], count)) {
            std::cerr << "count must be a nonnegative integer\n";
            return 1;
        }
        return bulk_insert(argv[3], count);
    }
    print_usage(argv[0]);
    return 1;
}
