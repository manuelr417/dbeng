#include "file/BlockFile.h"

#include "common/Error.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace bufman {
namespace {

void fail(const std::string& action) {
    throw common::Error(action + ": " + std::strerror(errno));
}

// write()/read() are allowed to transfer fewer bytes than requested (e.g. if
// a signal interrupts the call), so both helpers loop until the full block
// has been transferred, retrying on EINTR and treating a zero-byte result
// as an error rather than looping forever.
void write_all(int fd, const char* buffer, std::size_t length) {
    std::size_t completed = 0;
    while (completed < length) {
        const ssize_t written = ::write(fd, buffer + completed, length - completed);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            if (written < 0) {
                fail("write");
            } else {
                throw common::Error("write: no progress");
            }
        }
        completed += static_cast<std::size_t>(written);
    }
}

void read_all(int fd, char* buffer, std::size_t length) {
    std::size_t completed = 0;
    while (completed < length) {
        const ssize_t read_count = ::read(fd, buffer + completed, length - completed);
        if (read_count < 0 && errno == EINTR) {
            continue;
        }
        if (read_count <= 0) {
            if (read_count < 0) {
                fail("read");
            } else {
                throw common::Error("read: unexpected end of file");
            }
        }
        completed += static_cast<std::size_t>(read_count);
    }
}

void seek_block(int fd, std::uint64_t block_number) {
    constexpr std::uint64_t max_off = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
    if (block_number > max_off / bufman::kBlockSize) {
        throw common::Error("block offset exceeds off_t range");
    }
    const off_t offset = static_cast<off_t>(block_number * bufman::kBlockSize);
    if (::lseek(fd, offset, SEEK_SET) == static_cast<off_t>(-1)) {
        fail("lseek");
    }
}

off_t file_size(int fd) {
    const off_t size = ::lseek(fd, 0, SEEK_END);
    if (size == static_cast<off_t>(-1)) {
        fail("lseek to end");
    }
    if (size % static_cast<off_t>(bufman::kBlockSize) != 0) {
        throw common::Error("binary file size is not a multiple of 4096 bytes");
    }
    return size;
}

// Invariant maintained by this module: the file is always made up of
// complete kBlockSize blocks. The file layer itself offers block-level I/O
// only; record layout is the caller's concern (see heapfile/SlottedPage).

}

File open_for_append(const std::string& path) {
    File file;
    file.fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (file.fd < 0) {
        fail("open for append");
    }
    return file;
}

File open_for_read(const std::string& path) {
    File file;
    file.fd = ::open(path.c_str(), O_RDONLY);
    if (file.fd < 0) {
        fail("open for read");
    }
    return file;
}

void close(File& file) {
    if (file.fd >= 0) {
        ::close(file.fd);
        file.fd = -1;
    }
}

std::uint64_t block_count(File& file) {
    if (file.fd < 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(file_size(file.fd) /
                                      static_cast<off_t>(bufman::kBlockSize));
}

void read_block(File& file, std::uint64_t block_number,
                std::array<char, bufman::kBlockSize>& block) {
    read_block(file, block_number, block.data(), block.size());
}

void read_block(File& file, std::uint64_t block_number,
                char* buffer, std::size_t length) {
    if (file.fd < 0) {
        throw common::Error("read requested on a closed file");
    }
    if (buffer == nullptr || length != bufman::kBlockSize) {
        throw common::Error("read_block requires a 4096-byte buffer");
    }
    if (block_number >= block_count(file)) {
        throw common::Error("requested block is beyond end of file");
    }
    seek_block(file.fd, block_number);
    read_all(file.fd, buffer, length);
}

void write_block(File& file, std::uint64_t block_number,
                 const char* buffer, std::size_t length) {
    if (file.fd < 0) {
        throw common::Error("write requested on a closed file");
    }
    if (buffer == nullptr || length != bufman::kBlockSize) {
        throw common::Error("write_block requires a 4096-byte buffer");
    }
    seek_block(file.fd, block_number);
    write_all(file.fd, buffer, length);
}

}
