#include "hashindex/HashPages.h"

#include <cstring>
#include <stdexcept>

namespace hashindex {

namespace {

std::uint32_t read_u32(const char* buffer, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

void write_u32(char* buffer, std::size_t offset, std::uint32_t value) {
    std::memcpy(buffer + offset, &value, sizeof(value));
}

// FNV-1a 32-bit over `length` bytes — the hash for every non-integer
// key type, needing no tuned parameters (unlike the slides' a*k+b).
constexpr std::uint32_t kFnvOffset = 2166136261u;
constexpr std::uint32_t kFnvPrime = 16777619u;

std::uint32_t fnv_1a(const char* bytes, std::size_t length) {
    std::uint32_t hash = kFnvOffset;
    for (std::size_t i = 0; i < length; ++i) {
        hash ^= static_cast<unsigned char>(bytes[i]);
        hash *= kFnvPrime;
    }
    return hash;
}

}

// --- the hash ----------------------------------------------------------------

std::uint32_t hash_key(const isam::KeyCodec& codec, const char* key_bytes) {
    if (codec.key_type() == isam::KeyType::Integer) {
        // The lecture's identity: the value's own bit pattern, so the
        // slides' worked examples (h(4) = 100, h(20) = 10100) hold.
        return read_u32(key_bytes, 0);
    }
    return fnv_1a(key_bytes, codec.width());
}

// --- capacities -------------------------------------------------------------

std::size_t directory_capacity() {
    // count u32 + next-directory u32 + u32 slots
    return (kBlockSize - 8) / 4;
}

std::size_t bucket_capacity(std::size_t key_width) {
    // count u32 + local-depth u32 + first-overflow u32 + entries (key + rid)
    return (kBlockSize - 12) / (key_width + 8);
}

std::size_t overflow_capacity(std::size_t key_width) {
    // count u32 + next-overflow u32 + entries (key + rid)
    return (kBlockSize - 8) / (key_width + 8);
}

// --- HashEntryPage ------------------------------------------------------------

void HashEntryPage::bind(char* buffer, std::size_t key_width,
                         std::size_t entries_offset, std::size_t capacity) {
    buffer_ = buffer;
    key_width_ = key_width;
    entries_offset_ = entries_offset;
    capacity_ = capacity;
}

std::size_t HashEntryPage::count() const {
    return read_u32(buffer_, 0);
}

void HashEntryPage::write_count(std::size_t count) {
    write_u32(buffer_, 0, static_cast<std::uint32_t>(count));
}

bool HashEntryPage::attach(std::string& error) const {
    const std::size_t stored = count();
    if (stored > capacity_) {
        error = "page header holds " + std::to_string(stored) +
                " entries, capacity is " + std::to_string(capacity_);
        return false;
    }
    return true;
}

DataEntry HashEntryPage::get_entry(std::size_t i) const {
    if (i >= count()) {
        throw std::out_of_range("entry index " + std::to_string(i) +
                                " in a page holding " +
                                std::to_string(count()) + " entries");
    }
    DataEntry entry;
    entry.key.resize(key_width_);
    const char* raw = buffer_ + entry_offset(i);
    std::memcpy(entry.key.data(), raw, key_width_);
    std::memcpy(&entry.rid.page_id, raw + key_width_, 4);
    std::memcpy(&entry.rid.slot, raw + key_width_ + 4, 4);
    return entry;
}

bool HashEntryPage::append_entry(const DataEntry& entry, std::string& error) {
    const std::size_t n = count();
    if (n >= capacity_) {
        error = "page is full (" + std::to_string(capacity_) + " entries)";
        return false;
    }
    // The entry's key arrives already encoded to the codec's fixed
    // width; the entry is written in one pass, so a refusal (above) is
    // the only failure and nothing is half-written.
    char* raw = buffer_ + entry_offset(n);
    std::memcpy(raw, entry.key.data(), key_width_);
    std::memcpy(raw + key_width_, &entry.rid.page_id, 4);
    std::memcpy(raw + key_width_ + 4, &entry.rid.slot, 4);
    write_count(n + 1);
    return true;
}

void HashEntryPage::remove_entry(std::size_t i) {
    if (i >= count()) {
        throw std::out_of_range("entry index " + std::to_string(i) +
                                " in a page holding " +
                                std::to_string(count()) + " entries");
    }
    const std::size_t stride = key_width_ + 8;
    std::memmove(buffer_ + entry_offset(i), buffer_ + entry_offset(i + 1),
                 (count() - i - 1) * stride);
    write_count(count() - 1);
}

// --- DirectoryPage --------------------------------------------------------------

void DirectoryPage::bind(char* buffer) {
    buffer_ = buffer;
    capacity_ = directory_capacity();
}

void DirectoryPage::format() {
    write_u32(buffer_, 0, 0);
    write_u32(buffer_, kNextOffset, kNoPage);
}

bool DirectoryPage::attach(std::string& error) const {
    const std::size_t stored = count();
    if (stored > capacity_) {
        error = "directory page holds " + std::to_string(stored) +
                " slots, capacity is " + std::to_string(capacity_);
        return false;
    }
    return true;
}

std::size_t DirectoryPage::count() const {
    return read_u32(buffer_, 0);
}

std::uint32_t DirectoryPage::slot(std::size_t i) const {
    return read_u32(buffer_, kCountSize + i * 4 + 4);
}

void DirectoryPage::set_slot(std::size_t i, std::uint32_t page_id) {
    write_u32(buffer_, kCountSize + i * 4 + 4, page_id);
}

std::uint32_t DirectoryPage::next_directory() const {
    return read_u32(buffer_, kNextOffset);
}

void DirectoryPage::set_next_directory(std::uint32_t page_id) {
    write_u32(buffer_, kNextOffset, page_id);
}

// --- BucketPage -----------------------------------------------------------------

void BucketPage::bind(char* buffer, std::size_t key_width) {
    HashEntryPage::bind(buffer, key_width, 12, bucket_capacity(key_width));
}

void BucketPage::format(std::uint32_t local_depth,
                        std::uint32_t first_overflow) {
    write_u32(buffer_, 0, 0);
    write_u32(buffer_, kLocalDepthOffset, local_depth);
    write_u32(buffer_, kFirstOverflowOffset, first_overflow);
}

bool BucketPage::attach(std::string& error) const {
    return HashEntryPage::attach(error);
}

std::uint32_t BucketPage::local_depth() const {
    return read_u32(buffer_, kLocalDepthOffset);
}

void BucketPage::set_local_depth(std::uint32_t depth) {
    write_u32(buffer_, kLocalDepthOffset, depth);
}

std::uint32_t BucketPage::first_overflow() const {
    return read_u32(buffer_, kFirstOverflowOffset);
}

void BucketPage::set_first_overflow(std::uint32_t page_id) {
    write_u32(buffer_, kFirstOverflowOffset, page_id);
}

// --- OverflowPage ------------------------------------------------------------------

void OverflowPage::bind(char* buffer, std::size_t key_width) {
    HashEntryPage::bind(buffer, key_width, 8, overflow_capacity(key_width));
}

void OverflowPage::format(std::uint32_t next_overflow) {
    write_u32(buffer_, 0, 0);
    write_u32(buffer_, kNextOverflowOffset, next_overflow);
}

bool OverflowPage::attach(std::string& error) const {
    return HashEntryPage::attach(error);
}

std::uint32_t OverflowPage::next_overflow() const {
    return read_u32(buffer_, kNextOverflowOffset);
}

void OverflowPage::set_next_overflow(std::uint32_t page_id) {
    write_u32(buffer_, kNextOverflowOffset, page_id);
}

}
