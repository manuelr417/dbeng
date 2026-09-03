#include "BlockFile.h"
#include "PersonCsv.h"
#include "PersonSerializer.h"

#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <algorithm>
#include <unistd.h>

namespace {
std::string temp_path(const char* suffix) {
    return std::string("/tmp/ex07-test-") + std::to_string(getpid()) + suffix;
}

Person person(int pid, const char* name, int age, const char* city) {
    Person value{};
    value.pid = pid;
    value.age = age;
    std::strncpy(value.name, name, sizeof(value.name) - 1);
    std::memcpy(value.city, city, std::min(sizeof(value.city), std::strlen(city)));
    return value;
}
}

int main() {
    char record[person_serializer::kRecordSize]{};
    Person original = person(7, "Alice", 30, "PR");
    Person restored{};
    person_serializer::serialize(original, record, sizeof(record));
    assert(person_serializer::deserialize(record, sizeof(record), restored));
    assert(std::memcmp(&original.pid, &restored.pid, sizeof(original.pid)) == 0);
    assert(std::strcmp(original.name, restored.name) == 0);
    assert(original.age == restored.age);
    assert(std::string(restored.city, sizeof(restored.city)) == std::string("PR\0", 3));

    const std::string csv_path = temp_path(".csv");
    const std::string data_path = temp_path(".bin");
    {
        std::ofstream csv(csv_path);
        csv << "1,Ana,20,PR\n";
        csv << "bad,Row,20,PR\n";
        csv << "2,Bob,31,NY\n";
    }
    std::ostringstream diagnostics;
    const auto loaded = person_csv::load(csv_path, diagnostics);
    assert(loaded.people.size() == 2);
    assert(loaded.skipped == 1);

    block_file::File file;
    std::string error;
    assert(block_file::open_for_append(data_path, file, error));
    assert(block_file::append_records(file, loaded.people, error));
    block_file::close(file);

    assert(block_file::open_for_read(data_path, file, error));
    std::array<char, person_serializer::kBlockSize> block{};
    assert(block_file::read_block(file, 0, block, error));
    const auto records = person_serializer::deserialize_block(block.data());
    assert(records.size() == 2);
    assert(records[0].pid == 1);
    assert(records[1].pid == 2);
    block_file::close(file);

    const std::string boundary_path = temp_path("-boundary.bin");
    std::vector<Person> boundary_people;
    for (int pid = 1; pid <= 196; ++pid) {
        boundary_people.push_back(person(pid, "X", 20, "PR"));
    }
    assert(block_file::open_for_append(boundary_path, file, error));
    assert(block_file::append_records(file, boundary_people, error));
    block_file::close(file);
    assert(block_file::open_for_read(boundary_path, file, error));
    assert(block_file::block_count(file, error) == 2);
    assert(block_file::read_block(file, 1, block, error));
    assert(person_serializer::deserialize_block(block.data()).size() == 1);
    block_file::close(file);

    std::remove(csv_path.c_str());
    std::remove(data_path.c_str());
    std::remove(boundary_path.c_str());
    std::cout << "all tests passed\n";
}
