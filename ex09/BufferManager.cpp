#include "BufferManager.h"

#include <functional>

namespace bufman {

static_assert(kPageSize == kBlockSize, "frames and file blocks must match");

std::size_t BufferManager::PageKeyHash::operator()(const PageKey& key) const {
    const std::size_t h1 = std::hash<int>{}(key.fd);
    const std::size_t h2 = std::hash<std::uint64_t>{}(key.block_number);
    return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
}

void BufferManager::init(std::size_t pool_size, std::unique_ptr<ReplacementPolicy> policy) {
    for (bufman::File& file : files_) {
        bufman::close(file);
    }
    files_.clear();
    page_table_.clear();
    frame_page_.assign(pool_size, std::nullopt);
    pool_.init(pool_size);
    policy_ = std::move(policy);
    if (policy_) {
        policy_->init(pool_size);
    }
}

bool BufferManager::open_file(const std::string& path, bufman::File& file, std::string& error) {
    bufman::File opened;
    if (!bufman::open_for_append(path, opened, error)) {
        return false;
    }
    files_.push_back(opened);
    file = opened;
    return true;
}

bool BufferManager::open_file_read(const std::string& path, bufman::File& file, std::string& error) {
    bufman::File opened;
    if (!bufman::open_for_read(path, opened, error)) {
        return false;
    }
    files_.push_back(opened);
    file = opened;
    return true;
}

// A frame to reuse: never-used frames first (they never cost a write-back),
// then the policy's pick among the page-holding, unpinned frames.
std::optional<std::size_t> BufferManager::find_victim_frame() const {
    for (std::size_t i = 0; i < frame_page_.size(); ++i) {
        if (!frame_page_[i]) {
            return i;
        }
    }
    std::vector<std::size_t> candidates;
    for (std::size_t i = 0; i < frame_page_.size(); ++i) {
        if (frame_page_[i] && pool_.frame(i).pin_count == 0) {
            candidates.push_back(i);
        }
    }
    return policy_->pick_victim(candidates);
}

bufman::File* BufferManager::find_file(int fd) {
    for (bufman::File& file : files_) {
        if (file.fd == fd) {
            return &file;
        }
    }
    return nullptr;
}

// Shared by pin (miss) and alloc_page: write the frame's page back if it is
// dirty, then forget it so the frame is free for a new page.
bool BufferManager::evict_frame(std::size_t frame, std::string& error) {
    const std::optional<PageKey>& old = frame_page_[frame];
    if (old) {
        DataFrame& data = pool_.frame(frame);
        if (data.dirty) {
            bufman::File* old_file = find_file(old->fd);
            if (old_file == nullptr) {
                error = "victim page's file is not open";
                return false;
            }
            if (!bufman::write_block(*old_file, old->block_number, data.buffer,
                                        kBlockSize, error)) {
                return false;
            }
        }
        page_table_.erase(*old);
        policy_->on_remove(frame);
        frame_page_[frame] = std::nullopt;
    }
    return true;
}

bool BufferManager::pin(bufman::File& file, std::uint64_t block_number,
                        std::size_t& frame, std::string& error) {
    error.clear();
    if (!policy_ || pool_.size() == 0) {
        error = "buffer manager is not initialized";
        return false;
    }

    const PageKey key{file.fd, block_number};
    const auto hit = page_table_.find(key);
    if (hit != page_table_.end()) {
        frame = hit->second;
        pool_.pin(frame);
        policy_->on_access(frame);
        return true;
    }

    const std::optional<std::size_t> victim = find_victim_frame();
    if (!victim) {
        error = "all frames pinned";
        return false;
    }
    frame = *victim;

    if (!evict_frame(frame, error)) {
        return false;
    }

    DataFrame& data = pool_.frame(frame);
    if (!bufman::read_block(file, block_number, data.buffer, kBlockSize, error)) {
        data.dirty = false;
        return false;
    }
    frame_page_[frame] = key;
    page_table_[key] = frame;
    data.dirty = false;
    pool_.pin(frame);
    policy_->on_load(frame);
    return true;
}

bool BufferManager::alloc_page(bufman::File& file, std::uint64_t block_number,
                               std::size_t& frame, std::string& error) {
    error.clear();
    if (!policy_ || pool_.size() == 0) {
        error = "buffer manager is not initialized";
        return false;
    }
    if (file.fd < 0) {
        error = "alloc requested on a closed file";
        return false;
    }

    const PageKey key{file.fd, block_number};
    if (page_table_.find(key) != page_table_.end()) {
        error = "block is already resident in the pool";
        return false;
    }
    const std::uint64_t blocks = block_count(file, error);
    if (!error.empty()) {
        return false;
    }
    if (block_number < blocks) {
        error = "block already exists on disk";
        return false;
    }

    const std::optional<std::size_t> victim = find_victim_frame();
    if (!victim) {
        error = "all frames pinned";
        return false;
    }
    frame = *victim;

    if (!evict_frame(frame, error)) {
        return false;
    }

    DataFrame& data = pool_.frame(frame);
    initialize_block(data.buffer);
    frame_page_[frame] = key;
    page_table_[key] = frame;
    data.dirty = true;
    pool_.pin(frame);
    policy_->on_load(frame);
    return true;
}

bool BufferManager::resident(const bufman::File& file, std::uint64_t block_number) const {
    return page_table_.count(PageKey{file.fd, block_number}) > 0;
}

void BufferManager::unpin(std::size_t frame, bool was_dirty, std::string& error) {
    error.clear();
    if (was_dirty) {
        pool_.set_dirty(frame, true);
    }
    pool_.unpin(frame);
}

DataFrame& BufferManager::frame(std::size_t index) {
    return pool_.frame(index);
}

const DataFrame& BufferManager::frame(std::size_t index) const {
    return pool_.frame(index);
}

bool BufferManager::flush(std::size_t frame, std::string& error) {
    error.clear();
    if (frame >= pool_.size()) {
        error = "frame index out of range";
        return false;
    }
    const std::optional<PageKey>& entry = frame_page_[frame];
    DataFrame& data = pool_.frame(frame);
    if (!entry || !data.dirty) {
        return true;
    }
    bufman::File* file = find_file(entry->fd);
    if (file == nullptr) {
        error = "frame's file is not open";
        return false;
    }
    if (!bufman::write_block(*file, entry->block_number, data.buffer, kBlockSize, error)) {
        return false;
    }
    pool_.set_dirty(frame, false);
    return true;
}

bool BufferManager::flush_all(std::string& error) {
    error.clear();
    for (std::size_t i = 0; i < pool_.size(); ++i) {
        if (!flush(i, error)) {
            return false;
        }
    }
    return true;
}

void BufferManager::close_all(std::string& error) {
    // Flush failures are reported through `error`, but teardown continues:
    // the fds are going away either way.
    flush_all(error);
    for (bufman::File& file : files_) {
        bufman::close(file);
    }
    files_.clear();
    page_table_.clear();
    frame_page_.assign(pool_.size(), std::nullopt);
    if (policy_) {
        policy_->init(pool_.size());
    }
}

}
