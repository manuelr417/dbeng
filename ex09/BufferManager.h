#ifndef EX09_BUFFER_MANAGER_H
#define EX09_BUFFER_MANAGER_H

#include "BlockFile.h"
#include "BufferPool.h"
#include "ReplacementPolicy.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bufman {

// Sits between queries and disk: owns a BufferPool and moves 4096-byte
// blocks between files and frames through BlockFile. Tracks which
// (file, block) lives in which frame, evicts via an injected
// ReplacementPolicy, and writes dirty victims back before reuse.
class BufferManager {
public:
    void init(std::size_t pool_size, std::unique_ptr<ReplacementPolicy> policy);
    bool open_file(const std::string& path, bufman::File& file, std::string& error);
    bool open_file_read(const std::string& path, bufman::File& file, std::string& error);
    bool pin(bufman::File& file, std::uint64_t block_number,
             std::size_t& frame, std::string& error);
    // Creates a brand-new page: takes a frame, zeroes its buffer, registers
    // it as (file, block_number) and marks it dirty so it reaches disk on
    // write-back. Refuses blocks that are resident or already on disk.
    bool alloc_page(bufman::File& file, std::uint64_t block_number,
                    std::size_t& frame, std::string& error);
    // True if the page is currently resident in the pool (a pin would hit).
    bool resident(const bufman::File& file, std::uint64_t block_number) const;

    void unpin(std::size_t frame, bool was_dirty, std::string& error);
    DataFrame& frame(std::size_t index);
    const DataFrame& frame(std::size_t index) const;
    bool flush(std::size_t frame, std::string& error);
    bool flush_all(std::string& error);
    void close_all(std::string& error);

private:
    struct PageKey {
        int fd;
        std::uint64_t block_number;
        bool operator==(const PageKey&) const = default;
    };
    struct PageKeyHash {
        std::size_t operator()(const PageKey& key) const;
    };

    std::optional<std::size_t> find_victim_frame() const;
    bool evict_frame(std::size_t frame, std::string& error);
    bufman::File* find_file(int fd);

    BufferPool pool_;
    std::unique_ptr<ReplacementPolicy> policy_;
    std::unordered_map<PageKey, std::size_t, PageKeyHash> page_table_;
    std::vector<std::optional<PageKey>> frame_page_;
    std::vector<bufman::File> files_;
};

}

#endif // EX09_BUFFER_MANAGER_H
