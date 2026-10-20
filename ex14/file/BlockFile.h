#ifndef EX07_BLOCK_FILE_H
#define EX07_BLOCK_FILE_H

#include "bufpool/DataFrame.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace bufman {

// The file layer's block size: every file is a sequence of 4096-byte
// blocks, and read_block/write_block insist on exactly this buffer size.
constexpr std::size_t kBlockSize = 4096;

struct File {
    int fd = -1;
};

// Every function that takes an `error` string clears it on entry and fills
// it in only on failure, so after the call an empty `error` means success.
// Callers may therefore reuse one `error` string across calls. The file
// layer offers block-level I/O only; the meaning of the bytes inside a
// block is the caller's concern (see heapfile/SlottedPage).
bool open_for_append(const std::string& path, File& file, std::string& error);
bool open_for_read(const std::string& path, File& file, std::string& error);
void close(File& file);
bool read_block(File& file, std::uint64_t block_number,
                std::array<char, bufman::kBlockSize>& block,
                std::string& error);
bool read_block(File& file, std::uint64_t block_number,
                char* buffer, std::size_t length, std::string& error);
bool write_block(File& file, std::uint64_t block_number,
                 const char* buffer, std::size_t length, std::string& error);
std::uint64_t block_count(File& file, std::string& error);

}

#endif // EX07_BLOCK_FILE_H
