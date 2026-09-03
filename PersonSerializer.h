#ifndef EX07_PERSON_SERIALIZER_H
#define EX07_PERSON_SERIALIZER_H

#include "Person.h"

#include <cstddef>
#include <vector>

namespace person_serializer {

constexpr std::size_t kRecordSize = sizeof(int) + 10 + sizeof(int) + 3;
constexpr std::size_t kBlockSize = 4096;
constexpr std::size_t kRecordsPerBlock = kBlockSize / kRecordSize;

void serialize(const Person& person, char* buffer, std::size_t max_len);
bool deserialize(const char* buffer, std::size_t max_len, Person& person);
void initialize_block(char* block);
std::size_t serialize_block(const std::vector<Person>& persons, char* block);
std::vector<Person> deserialize_block(const char* block);

}

#endif // EX07_PERSON_SERIALIZER_H
