#include "index/IndexPages.h"

#include <cstring>
#include <stdexcept>

namespace isam {

namespace {

std::uint32_t read_u32(const char* buffer, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

void write_u32(char* buffer, std::size_t offset, std::uint32_t value) {
    std::memcpy(buffer + offset, &value, sizeof(value));
}

}

// --- capacities ------------------------------------------------------------

std::size_t separator_capacity(std::size_t key_width) {
    // count u32 + entries (key + child u32) + first child u32
    return (kBlockSize - 8) / (key_width + 4);
}

std::size_t leaf_capacity(std::size_t key_width) {
    // count u32 + next-leaf u32 + first-overflow u32 + entries (key + rid)
    return (kBlockSize - 12) / (key_width + 8);
}

std::size_t overflow_capacity(std::size_t key_width) {
    // count u32 + next-overflow u32 + entries (key + rid)
    return (kBlockSize - 8) / (key_width + 8);
}

// --- EntryPage --------------------------------------------------------------

void EntryPage::bind(char* buffer, std::size_t key_width,
                     std::size_t entries_offset, std::size_t capacity) {
    buffer_ = buffer;
    key_width_ = key_width;
    entries_offset_ = entries_offset;
    capacity_ = capacity;
}

std::size_t EntryPage::count() const {
    return read_u32(buffer_, 0);
}

void EntryPage::write_count(std::size_t count) {
    write_u32(buffer_, 0, static_cast<std::uint32_t>(count));
}

bool EntryPage::attach(std::string& error) const {
    const std::size_t stored = count();
    if (stored > capacity_) {
        error = "page header holds " + std::to_string(stored) +
                " entries, capacity is " + std::to_string(capacity_);
        return false;
    }
    return true;
}

std::size_t EntryPage::lower_bound(const std::string& key,
                                   const KeyCodec& codec) const {
    std::size_t lo = 0;
    std::size_t hi = count();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const char* mid_key = buffer_ + entry_offset(mid);
        if (codec.compare(mid_key, key.data()) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

DataEntry EntryPage::get_entry(std::size_t i) const {
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

bool EntryPage::insert_entry(const DataEntry& entry, const KeyCodec& codec,
                             std::string& error) {
    const std::size_t n = count();
    if (n >= capacity_) {
        error = "page is full (" + std::to_string(capacity_) + " entries)";
        return false;
    }
    // The entry's key arrives already encoded to the codec's fixed
    // width; the entry is written in one pass after the tail memmove,
    // so a refusal (above) is the only failure and nothing is
    // half-written.
    const std::size_t pos = lower_bound(entry.key, codec);
    const std::size_t stride = key_width_ + 8;
    if (pos < n) {
        std::memmove(buffer_ + entry_offset(pos + 1),
                     buffer_ + entry_offset(pos), (n - pos) * stride);
    }
    char* raw = buffer_ + entry_offset(pos);
    std::memcpy(raw, entry.key.data(), key_width_);
    std::memcpy(raw + key_width_, &entry.rid.page_id, 4);
    std::memcpy(raw + key_width_ + 4, &entry.rid.slot, 4);
    write_count(n + 1);
    return true;
}

void EntryPage::remove_entry(std::size_t i) {
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

// --- LeafPage ---------------------------------------------------------------

void LeafPage::bind(char* buffer, std::size_t key_width) {
    EntryPage::bind(buffer, key_width, 12, leaf_capacity(key_width));
}

void LeafPage::format(std::uint32_t next_leaf, std::uint32_t first_overflow) {
    write_u32(buffer_, 0, 0);
    write_u32(buffer_, kNextLeafOffset, next_leaf);
    write_u32(buffer_, kFirstOverflowOffset, first_overflow);
}

bool LeafPage::attach(std::string& error) const {
    return EntryPage::attach(error);
}

std::uint32_t LeafPage::next_leaf() const {
    return read_u32(buffer_, kNextLeafOffset);
}

void LeafPage::set_next_leaf(std::uint32_t page_id) {
    write_u32(buffer_, kNextLeafOffset, page_id);
}

std::uint32_t LeafPage::first_overflow() const {
    return read_u32(buffer_, kFirstOverflowOffset);
}

void LeafPage::set_first_overflow(std::uint32_t page_id) {
    write_u32(buffer_, kFirstOverflowOffset, page_id);
}

// --- OverflowPage -----------------------------------------------------------

void OverflowPage::bind(char* buffer, std::size_t key_width) {
    EntryPage::bind(buffer, key_width, 8, overflow_capacity(key_width));
}

void OverflowPage::format(std::uint32_t next_overflow) {
    write_u32(buffer_, 0, 0);
    write_u32(buffer_, kNextOverflowOffset, next_overflow);
}

bool OverflowPage::attach(std::string& error) const {
    return EntryPage::attach(error);
}

std::uint32_t OverflowPage::next_overflow() const {
    return read_u32(buffer_, kNextOverflowOffset);
}

void OverflowPage::set_next_overflow(std::uint32_t page_id) {
    write_u32(buffer_, kNextOverflowOffset, page_id);
}

// --- SeparatorPage ----------------------------------------------------------

void SeparatorPage::bind(char* buffer, std::size_t key_width) {
    buffer_ = buffer;
    key_width_ = key_width;
    capacity_ = separator_capacity(key_width);
}

void SeparatorPage::format() {
    write_u32(buffer_, 0, 0);
    write_u32(buffer_, kCountSize, kNoPage);
}

bool SeparatorPage::attach(std::string& error) const {
    const std::size_t stored = count();
    if (stored > capacity_) {
        error = "separator page holds " + std::to_string(stored) +
                " separators, capacity is " + std::to_string(capacity_);
        return false;
    }
    return true;
}

std::size_t SeparatorPage::count() const {
    return read_u32(buffer_, 0);
}

std::uint32_t SeparatorPage::child(std::size_t i) const {
    return read_u32(buffer_, entry_offset(i) + key_width_);
}

const char* SeparatorPage::separator_key(std::size_t i) const {
    return buffer_ + entry_offset(i);
}

IndexEntry SeparatorPage::entry(std::size_t i) const {
    IndexEntry index_entry;
    index_entry.key.resize(key_width_);
    const char* raw = buffer_ + entry_offset(i);
    std::memcpy(index_entry.key.data(), raw, key_width_);
    std::memcpy(&index_entry.child, raw + key_width_, 4);
    return index_entry;
}

std::uint32_t SeparatorPage::first_child() const {
    // The first child sits right after the entry array.
    return read_u32(buffer_, kCountSize + count() * (key_width_ + 4));
}

void SeparatorPage::set_first_child(std::uint32_t page_id) {
    write_u32(buffer_, kCountSize + count() * (key_width_ + 4), page_id);
}

bool SeparatorPage::append(const char* key, std::uint32_t child_id,
                           std::string& error) {
    const std::size_t n = count();
    if (n >= capacity_) {
        error = "separator page is full (" + std::to_string(capacity_) +
                " separators)";
        return false;
    }
    char* entry = buffer_ + entry_offset(n);
    std::memcpy(entry, key, key_width_);
    std::memcpy(entry + key_width_, &child_id, 4);
    write_u32(buffer_, 0, static_cast<std::uint32_t>(n + 1));
    // The first-child slot lives right after the entry array; the append
    // moved it one slot up, so re-initialize it.
    write_u32(buffer_, kCountSize + (n + 1) * (key_width_ + 4), kNoPage);
    return true;
}

bool SeparatorPage::append(const IndexEntry& index_entry,
                           std::string& error) {
    return append(index_entry.key.data(), index_entry.child, error);
}

std::uint32_t SeparatorPage::descend_step(const char* key,
                                          const KeyCodec& codec) const {
    // Last separator strictly less than key: binary search.
    std::size_t lo = 0;
    std::size_t hi = count();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (codec.compare(separator_key(mid), key) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return first_child();
    }
    return child(lo - 1);
}

}
