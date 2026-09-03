#ifndef EX07_BLOCK_FILE_H
#define EX07_BLOCK_FILE_H

#include "Person.h"
#include "PersonSerializer.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace block_file {

struct File {
    int fd = -1;
};

bool open_for_append(const std::string& path, File& file, std::string& error);
bool open_for_read(const std::string& path, File& file, std::string& error);
void close(File& file);
bool append_records(File& file, const std::vector<Person>& people, std::string& error);
bool read_block(File& file, std::uint64_t block_number,
                std::array<char, person_serializer::kBlockSize>& block,
                std::string& error);
std::uint64_t block_count(File& file, std::string& error);

}

#endif // EX07_BLOCK_FILE_H
