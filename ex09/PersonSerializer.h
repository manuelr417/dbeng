#ifndef EX07_PERSON_SERIALIZER_H
#define EX07_PERSON_SERIALIZER_H

#include "Person.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace bufman {

constexpr std::size_t kRecordSize = sizeof(int) + 10 + sizeof(int) + 3;
constexpr std::size_t kBlockSize = 4096;
constexpr std::size_t kRecordsPerBlock = kBlockSize / kRecordSize;

void serialize(const Person& person, char* buffer, std::size_t max_len);
bool deserialize(const char* buffer, std::size_t max_len, Person& person);
void initialize_block(char* block);
std::size_t serialize_block(const std::vector<Person>& persons, char* block);
std::vector<Person> deserialize_block(const char* block);

// Slot-level helpers over any block-sized buffer (e.g. a DataFrame's).
// A slot is occupied iff its record's pid != 0; pid == 0 is the free-slot
// sentinel, so a Person with pid 0 can never be stored.
std::size_t record_count(const char* block);
std::optional<std::size_t> first_free_slot(const char* block);
bool get_record(const char* block, std::size_t slot, Person& person);
bool put_record(char* block, std::size_t slot, const Person& person);

}

#endif // EX07_PERSON_SERIALIZER_H
