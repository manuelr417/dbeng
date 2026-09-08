#include "BlockFile.h"
#include "PersonCsv.h"
#include "PersonGenerator.h"
#include "PersonSerializer.h"

#include <charconv>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

void print_usage(const char* program) {
    std::cerr << "Usage:\n"
              << "  " << program << " append <csv-file> <binary-file>\n"
              << "  " << program << " read <binary-file> <block-number>\n"
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

int append_csv_file(const std::string& csv_path, const std::string& binary_path) {
    const person_csv::LoadResult loaded = person_csv::load(csv_path, std::cerr);
    if (loaded.people.empty()) {
        return loaded.skipped == 0 ? 0 : 1;
    }

    block_file::File file;
    std::string error;
    if (!block_file::open_for_append(binary_path, file, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    const bool success = block_file::append_records(file, loaded.people, error);
    block_file::close(file);
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

    block_file::File file;
    std::string error;
    if (!block_file::open_for_append(binary_path, file, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    const std::uint64_t existing = block_file::record_count(file, error);
    if (!error.empty()) {
        block_file::close(file);
        std::cerr << error << '\n';
        return 1;
    }

    const auto people = person_generator::generate(count, static_cast<int>(existing) + 1);
    const bool success = block_file::append_records(file, people, error);
    block_file::close(file);
    if (!success) {
        std::cerr << error << '\n';
        return 1;
    }
    std::cout << "appended " << people.size() << " generated record(s)\n";
    return 0;
}

int read_and_print_block(const std::string& binary_path, std::uint64_t block_number) {
    block_file::File file;
    std::string error;
    if (!block_file::open_for_read(binary_path, file, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::array<char, person_serializer::kBlockSize> block{};
    const bool success = block_file::read_block(file, block_number, block, error);
    block_file::close(file);
    if (!success) {
        std::cerr << error << '\n';
        return 1;
    }

    const auto people = person_serializer::deserialize_block(block.data());
    std::cout << "block " << block_number << ": " << people.size() << " record(s)\n";
    for (std::size_t i = 0; i < people.size(); ++i) {
        print_person(people[i], i);
    }
    return 0;
}

int scan_file(const std::string& binary_path) {
    block_file::File file;
    std::string error;
    if (!block_file::open_for_read(binary_path, file, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    const std::uint64_t blocks = block_file::block_count(file, error);
    if (!error.empty()) {
        block_file::close(file);
        std::cerr << error << '\n';
        return 1;
    }

    std::uint64_t total_records = 0;
    std::array<char, person_serializer::kBlockSize> block{};
    for (std::uint64_t block_number = 0; block_number < blocks; ++block_number) {
        if (!block_file::read_block(file, block_number, block, error)) {
            block_file::close(file);
            std::cerr << error << '\n';
            return 1;
        }
        const auto people = person_serializer::deserialize_block(block.data());
        std::cout << "block " << block_number << ": " << people.size() << " record(s)\n";
        for (std::size_t i = 0; i < people.size(); ++i) {
            print_person(people[i], i);
        }
        total_records += people.size();
    }
    block_file::close(file);
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
