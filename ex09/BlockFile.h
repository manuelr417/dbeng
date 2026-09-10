#ifndef EX07_BLOCK_FILE_H
#define EX07_BLOCK_FILE_H

#include "PersonSerializer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace bufman {

struct File {
    int fd = -1;
};

// Every function that takes an `error` string clears it on entry and fills
// it in only on failure, so after the call an empty `error` means success.
// Callers may therefore reuse one `error` string across calls.
bool open_for_append(const std::string& path, File& file, std::string& error);
bool open_for_read(const std::string& path, File& file, std::string& error);
void close(File& file);
bool append_records(File& file, const char* records, std::size_t length, std::string& error);
bool read_block(File& file, std::uint64_t block_number,
                std::array<char, bufman::kBlockSize>& block,
                std::string& error);
bool read_block(File& file, std::uint64_t block_number,
                char* buffer, std::size_t length, std::string& error);
bool write_block(File& file, std::uint64_t block_number,
                 const char* buffer, std::size_t length, std::string& error);
std::uint64_t block_count(File& file, std::string& error);
std::uint64_t record_count(File& file, std::string& error);

}

#endif // EX07_BLOCK_FILE_H
