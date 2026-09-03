#include "PersonSerializer.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace person_serializer {

void serialize(const Person& person, char* buffer, std::size_t max_len) {
    assert(buffer != nullptr);
    if (max_len < kRecordSize) {
        return;
    }

    std::size_t offset = 0;
    std::memcpy(buffer + offset, &person.pid, sizeof(person.pid));
    offset += sizeof(person.pid);
    std::memcpy(buffer + offset, person.name, sizeof(person.name));
    offset += sizeof(person.name);
    std::memcpy(buffer + offset, &person.age, sizeof(person.age));
    offset += sizeof(person.age);
    std::memcpy(buffer + offset, person.city, sizeof(person.city));
}

bool deserialize(const char* buffer, std::size_t max_len, Person& person) {
    if (buffer == nullptr || max_len < kRecordSize) {
        return false;
    }

    person = Person{};
    std::size_t offset = 0;
    std::memcpy(&person.pid, buffer + offset, sizeof(person.pid));
    offset += sizeof(person.pid);
    std::memcpy(person.name, buffer + offset, sizeof(person.name));
    offset += sizeof(person.name);
    std::memcpy(&person.age, buffer + offset, sizeof(person.age));
    offset += sizeof(person.age);
    std::memcpy(person.city, buffer + offset, sizeof(person.city));
    return true;
}

void initialize_block(char* block) {
    assert(block != nullptr);
    std::memset(block, 0, kBlockSize);
}

std::size_t serialize_block(const std::vector<Person>& persons, char* block) {
    assert(block != nullptr);
    initialize_block(block);
    const std::size_t count = std::min(persons.size(), kRecordsPerBlock);
    for (std::size_t i = 0; i < count; ++i) {
        serialize(persons[i], block + i * kRecordSize, kRecordSize);
    }
    return count;
}

std::vector<Person> deserialize_block(const char* block) {
    std::vector<Person> people;
    if (block == nullptr) {
        return people;
    }

    for (std::size_t i = 0; i < kRecordsPerBlock; ++i) {
        Person person{};
        if (!deserialize(block + i * kRecordSize, kRecordSize, person) || person.pid == 0) {
            break;
        }
        people.push_back(person);
    }
    return people;
}

}
