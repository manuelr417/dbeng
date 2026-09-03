#include "BlockFile.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace block_file {
namespace {

void set_error(std::string& error, const std::string& action) {
    error = action + ": " + std::strerror(errno);
}

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
    if (block_number > max_off / person_serializer::kBlockSize) {
        error = "block offset exceeds off_t range";
        return false;
    }
    const off_t offset = static_cast<off_t>(block_number * person_serializer::kBlockSize);
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
    if (size % static_cast<off_t>(person_serializer::kBlockSize) != 0) {
        error = "binary file size is not a multiple of 4096 bytes";
        return false;
    }
    return true;
}

}

bool open_for_append(const std::string& path, File& file, std::string& error) {
    file.fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (file.fd < 0) {
        set_error(error, "open for append");
        return false;
    }
    return true;
}

bool open_for_read(const std::string& path, File& file, std::string& error) {
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
    off_t size = 0;
    if (file.fd < 0 || !file_size(file.fd, size, error)) {
        return 0;
    }
    return static_cast<std::uint64_t>(size / static_cast<off_t>(person_serializer::kBlockSize));
}

bool read_block(File& file, std::uint64_t block_number,
                std::array<char, person_serializer::kBlockSize>& block,
                std::string& error) {
    const std::uint64_t count = block_count(file, error);
    if (!error.empty() && file.fd >= 0) {
        return false;
    }
    if (block_number >= count) {
        error = "requested block is beyond end of file";
        return false;
    }
    if (!seek_block(file.fd, block_number, error)) {
        return false;
    }
    return read_all(file.fd, block.data(), block.size(), error);
}

bool append_records(File& file, const std::vector<Person>& people, std::string& error) {
    if (file.fd < 0) {
        error = "append requested on a closed file";
        return false;
    }
    if (people.empty()) {
        return true;
    }

    off_t size = 0;
    if (!file_size(file.fd, size, error)) {
        return false;
    }

    std::array<char, person_serializer::kBlockSize> block{};
    std::uint64_t current_block = static_cast<std::uint64_t>(size / static_cast<off_t>(person_serializer::kBlockSize));
    std::size_t slot = 0;
    if (current_block > 0) {
        --current_block;
        if (!seek_block(file.fd, current_block, error) ||
            !read_all(file.fd, block.data(), block.size(), error)) {
            return false;
        }
        const auto existing = person_serializer::deserialize_block(block.data());
        slot = existing.size();
        if (slot == person_serializer::kRecordsPerBlock) {
            ++current_block;
            slot = 0;
            person_serializer::initialize_block(block.data());
        }
    } else {
        person_serializer::initialize_block(block.data());
    }

    for (const Person& person : people) {
        if (slot == person_serializer::kRecordsPerBlock) {
            if (!seek_block(file.fd, current_block, error) ||
                !write_all(file.fd, block.data(), block.size(), error)) {
                return false;
            }
            ++current_block;
            slot = 0;
            person_serializer::initialize_block(block.data());
        }
        person_serializer::serialize(person, block.data() + slot * person_serializer::kRecordSize,
                                      person_serializer::kRecordSize);
        ++slot;
    }

    if (!seek_block(file.fd, current_block, error) ||
        !write_all(file.fd, block.data(), block.size(), error)) {
        return false;
    }
    return true;
}

}
