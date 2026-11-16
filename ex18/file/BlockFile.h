#ifndef EX07_BLOCK_FILE_H
#define EX07_BLOCK_FILE_H

#include "bufpool/DataFrame.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace bufman {

// The file layer's block size: every file is a sequence of 4096-byte
// blocks, and read_block/write_block insist on exactly this buffer size.
constexpr std::size_t kBlockSize = 4096;

struct File {
    int fd = -1;
};

// Failures throw `common::Error` with a named message (an OS error keeps
// its errno text). The file layer offers block-level I/O only; the
// meaning of the bytes inside a block is the caller's concern (see
// heapfile/SlottedPage).
File open_for_append(const std::string& path);
File open_for_read(const std::string& path);
void close(File& file);
void read_block(File& file, std::uint64_t block_number,
                std::array<char, bufman::kBlockSize>& block);
void read_block(File& file, std::uint64_t block_number,
                char* buffer, std::size_t length);
void write_block(File& file, std::uint64_t block_number,
                 const char* buffer, std::size_t length);
std::uint64_t block_count(File& file);

}

#endif // EX07_BLOCK_FILE_H
