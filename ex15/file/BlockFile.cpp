#include "file/BlockFile.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace bufman {
namespace {

void set_error(std::string& error, const std::string& action) {
    error = action + ": " + std::strerror(errno);
}

// write()/read() are allowed to transfer fewer bytes than requested (e.g. if
// a signal interrupts the call), so both helpers loop until the full block
// has been transferred, retrying on EINTR and treating a zero-byte result
// as an error rather than looping forever.
bool write_all(int fd, const char* buffer, std::size_t length, std::string& error) {
    std::size_t completed = 0;
    while (completed < length) {
        const ssize_t written = ::write(fd, buffer + completed, length - completed);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            if (written < 0) {
                set_error(error, "write");
            } else {
                error = "write: no progress";
            }
            return false;
        }
        completed += static_cast<std::size_t>(written);
    }
    return true;
}

bool read_all(int fd, char* buffer, std::size_t length, std::string& error) {
    std::size_t completed = 0;
    while (completed < length) {
        const ssize_t read_count = ::read(fd, buffer + completed, length - completed);
        if (read_count < 0 && errno == EINTR) {
            continue;
        }
        if (read_count <= 0) {
            if (read_count < 0) {
                set_error(error, "read");
            } else {
                error = "read: unexpected end of file";
            }
            return false;
        }
        completed += static_cast<std::size_t>(read_count);
    }
    return true;
}

bool seek_block(int fd, std::uint64_t block_number, std::string& error) {
    constexpr std::uint64_t max_off = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
    if (block_number > max_off / bufman::kBlockSize) {
        error = "block offset exceeds off_t range";
        return false;
    }
    const off_t offset = static_cast<off_t>(block_number * bufman::kBlockSize);
    if (::lseek(fd, offset, SEEK_SET) == static_cast<off_t>(-1)) {
        set_error(error, "lseek");
        return false;
    }
    return true;
}

bool file_size(int fd, off_t& size, std::string& error) {
    size = ::lseek(fd, 0, SEEK_END);
    if (size == static_cast<off_t>(-1)) {
        set_error(error, "lseek to end");
        return false;
    }
    if (size % static_cast<off_t>(bufman::kBlockSize) != 0) {
        error = "binary file size is not a multiple of 4096 bytes";
        return false;
    }
    return true;
}

// Invariant maintained by this module: the file is always made up of
// complete kBlockSize blocks. The file layer itself offers block-level I/O
// only; record layout is the caller's concern (see heapfile/SlottedPage).

}

bool open_for_append(const std::string& path, File& file, std::string& error) {
    error.clear();
    file.fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (file.fd < 0) {
        set_error(error, "open for append");
        return false;
    }
    return true;
}

bool open_for_read(const std::string& path, File& file, std::string& error) {
    error.clear();
    file.fd = ::open(path.c_str(), O_RDONLY);
    if (file.fd < 0) {
        set_error(error, "open for read");
        return false;
    }
    return true;
}

void close(File& file) {
    if (file.fd >= 0) {
        ::close(file.fd);
        file.fd = -1;
    }
}

std::uint64_t block_count(File& file, std::string& error) {
    error.clear();
    off_t size = 0;
    if (file.fd < 0 || !file_size(file.fd, size, error)) {
        return 0;
    }
    return static_cast<std::uint64_t>(size / static_cast<off_t>(bufman::kBlockSize));
}

bool read_block(File& file, std::uint64_t block_number,
                std::array<char, bufman::kBlockSize>& block,
                std::string& error) {
    return read_block(file, block_number, block.data(), block.size(), error);
}

bool read_block(File& file, std::uint64_t block_number,
                char* buffer, std::size_t length, std::string& error) {
    error.clear();
    if (file.fd < 0) {
        error = "read requested on a closed file";
        return false;
    }
    if (buffer == nullptr || length != bufman::kBlockSize) {
        error = "read_block requires a 4096-byte buffer";
        return false;
    }
    const std::uint64_t count = block_count(file, error);
    if (!error.empty()) {
        return false;
    }
    if (block_number >= count) {
        error = "requested block is beyond end of file";
        return false;
    }
    if (!seek_block(file.fd, block_number, error)) {
        return false;
    }
    return read_all(file.fd, buffer, length, error);
}

bool write_block(File& file, std::uint64_t block_number,
                 const char* buffer, std::size_t length, std::string& error) {
    error.clear();
    if (file.fd < 0) {
        error = "write requested on a closed file";
        return false;
    }
    if (buffer == nullptr || length != bufman::kBlockSize) {
        error = "write_block requires a 4096-byte buffer";
        return false;
    }
    if (!seek_block(file.fd, block_number, error)) {
        return false;
    }
    return write_all(file.fd, buffer, length, error);
}

}
