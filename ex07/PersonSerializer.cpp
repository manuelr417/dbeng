#include "PersonSerializer.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace person_serializer {

// Packs one Person into `kRecordSize` bytes, field by field. We copy each
// field individually (rather than memcpy'ing the whole struct) because the
// compiler is free to insert padding between struct members; only this
// explicit, gap-free layout is guaranteed to match on disk.
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

// Mirror image of serialize: walks the same fields in the same order at the
// same offsets, copying bytes out of the buffer and into a fresh Person.
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

// Zero-fills a block so every unused record slot reads back as pid == 0,
// which deserialize_block below treats as "end of data in this block".
void initialize_block(char* block) {
    assert(block != nullptr);
    std::memset(block, 0, kBlockSize);
}

// Lays out up to kRecordsPerBlock records back to back: record i starts at
// byte i * kRecordSize. Any leftover slots stay zeroed by initialize_block.
std::size_t serialize_block(const std::vector<Person>& persons, char* block) {
    assert(block != nullptr);
    initialize_block(block);
    const std::size_t count = std::min(persons.size(), kRecordsPerBlock);
    for (std::size_t i = 0; i < count; ++i) {
        serialize(persons[i], block + i * kRecordSize, kRecordSize);
    }
    return count;
}

// Reads records out of a block in the same fixed-slot order, stopping at the
// first pid == 0 slot (see initialize_block) since that marks unused space.
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
