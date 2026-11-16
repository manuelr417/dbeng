#ifndef EX09_BUFFER_MANAGER_H
#define EX09_BUFFER_MANAGER_H

#include "bufpool/BufferPool.h"
#include "file/BlockFile.h"
#include "policy/ReplacementPolicy.h"

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
    bufman::File open_file(const std::string& path);
    bufman::File open_file_read(const std::string& path);
    void pin(bufman::File& file, std::uint64_t block_number, std::size_t& frame);
    // Creates a brand-new page: takes a frame, zeroes its buffer, registers
    // it as (file, block_number) and marks it dirty so it reaches disk on
    // write-back. Throws when the block is already on disk.
    void alloc_page(bufman::File& file, std::uint64_t block_number,
                    std::size_t& frame);
    // True if the page is currently resident in the pool (a pin would hit).
    bool resident(const bufman::File& file, std::uint64_t block_number) const;

    void unpin(std::size_t frame, bool was_dirty);
    DataFrame& frame(std::size_t index);
    const DataFrame& frame(std::size_t index) const;
    void flush(std::size_t frame);
    void flush_all();
    void close_all();
    // Flushes the dirty pages of this one file, drops its entries from
    // the page table and frame map, closes its descriptor, and removes it
    // from the registry. Other files' pages and descriptors are untouched.
    // Throws when a frame of the file is still pinned.
    void close_file(File& file);
    // True if a file with this path is currently open through this manager.
    bool is_open(const std::string& path) const;

private:
    struct PageKey {
        int fd;
        std::uint64_t block_number;
        bool operator==(const PageKey&) const = default;
    };
    struct PageKeyHash {
        std::size_t operator()(const PageKey& key) const;
    };
    struct OpenFile {
        bufman::File file;
        std::string path;
    };

    std::optional<std::size_t> find_victim_frame() const;
    void evict_frame(std::size_t frame);
    bufman::File* find_file(int fd);

    BufferPool pool_;
    std::unique_ptr<ReplacementPolicy> policy_;
    std::unordered_map<PageKey, std::size_t, PageKeyHash> page_table_;
    std::vector<std::optional<PageKey>> frame_page_;
    std::vector<OpenFile> files_;
};

}

#endif // EX09_BUFFER_MANAGER_H
