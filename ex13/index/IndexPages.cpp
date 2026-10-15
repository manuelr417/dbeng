#include "index/IndexPages.h"

#include <cstring>
#include <stdexcept>

namespace isam {

namespace {

// Reads the u32 at `offset` (native endian by memcpy).
// `buffer` — the block bytes; `offset` — byte offset of the field.
// Returns the field value.
std::uint32_t read_u32(const char* buffer, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

// Writes the u32 `value` at `offset` (native endian by memcpy).
// `buffer` — the block bytes; `offset` — byte offset of the field.
void write_u32(char* buffer, std::size_t offset, std::uint32_t value) {
    std::memcpy(buffer + offset, &value, sizeof(value));
}

}

// --- capacities ------------------------------------------------------------

// Maximum index entries per separator page.
// `key_width` — the fixed key width in bytes.
// Returns the capacity from the layout: count u32 + entries (key +
// child u32) + first child u32.
std::size_t separator_capacity(std::size_t key_width) {
    // count u32 + entries (key + child u32) + first child u32
    return (kBlockSize - 8) / (key_width + 4);
}

// Maximum data entries per leaf page.
// `key_width` — the fixed key width in bytes.
// Returns the capacity from the layout: count u32 + next-leaf u32 +
// first-overflow u32 + entries (key + rid).
std::size_t leaf_capacity(std::size_t key_width) {
    // count u32 + next-leaf u32 + first-overflow u32 + entries (key + rid)
    return (kBlockSize - 12) / (key_width + 8);
}

// Maximum data entries per overflow page.
// `key_width` — the fixed key width in bytes.
// Returns the capacity from the layout: count u32 + next-overflow u32 +
// entries (key + rid).
std::size_t overflow_capacity(std::size_t key_width) {
    // count u32 + next-overflow u32 + entries (key + rid)
    return (kBlockSize - 8) / (key_width + 8);
}

// --- EntryPage --------------------------------------------------------------

// Binds the entry region: a header of `entries_offset` bytes, then at
// most `capacity` entries of (key + rid).
// `buffer` — the pinned block; `key_width` — the key width;
// `entries_offset` — header bytes before the first entry; `capacity` —
// the maximum entry count.
void EntryPage::bind(char* buffer, std::size_t key_width,
                     std::size_t entries_offset, std::size_t capacity) {
    buffer_ = buffer;
    key_width_ = key_width;
    entries_offset_ = entries_offset;
    capacity_ = capacity;
}

// The stored entry count (the u32 at offset 0).
std::size_t EntryPage::count() const {
    return read_u32(buffer_, 0);
}

// Writes the entry count (the u32 at offset 0).
// `count` — the new entry count.
void EntryPage::write_count(std::size_t count) {
    write_u32(buffer_, 0, static_cast<std::uint32_t>(count));
}

// Validates the stored count against the bound capacity.
// Returns true when usable; false with `error` naming the mismatch.
bool EntryPage::attach(std::string& error) const {
    const std::size_t stored = count();
    if (stored > capacity_) {
        error = "page header holds " + std::to_string(stored) +
                " entries, capacity is " + std::to_string(capacity_);
        return false;
    }
    return true;
}

// First entry index whose key is >= `key`, by binary search.
// `key` — encoded key bytes; `codec` — the order-defining codec.
// Returns the first position with key >= `key` (count() when all
// stored keys are smaller).
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

// Copies out entry i, rejecting an out-of-range index.
// `i` — the entry index.
// Returns the (key, rid) pair; throws std::out_of_range past the end.
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

// Sorted insert: positions by lower_bound, memmoves the tail one slot,
// writes the entry, bumps the count.
// `entry` — the (encoded key, rid) pair; `codec` — the order-defining
// codec.
// Returns true on success; false with `error` set when the page is
// full (the only failure — nothing is half-written).
bool EntryPage::insert_entry(const DataEntry& entry, const KeyCodec& codec,
                             std::string& error) {
    const std::size_t n = count();
    if (n >= capacity_) {
        error = "page is full (" + std::to_string(capacity_) + " entries)";
        return false;
    }
    // The entry's key arrives already encoded to the codec's fixed
    // width.
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

// --- LeafPage ---------------------------------------------------------------

// Binds a leaf view: 12 header bytes (count, next leaf, first overflow)
// and the leaf capacity.
// `buffer` — the pinned block; `key_width` — the key width.
void LeafPage::bind(char* buffer, std::size_t key_width) {
    EntryPage::bind(buffer, key_width, 12, leaf_capacity(key_width));
}

// Initializes the page: zero entries, links as given.
// `next_leaf` — the chain successor; `first_overflow` — the
// overflow-chain head (kNoPage ends each).
void LeafPage::format(std::uint32_t next_leaf, std::uint32_t first_overflow) {
    write_u32(buffer_, 0, 0);
    write_u32(buffer_, kNextLeafOffset, next_leaf);
    write_u32(buffer_, kFirstOverflowOffset, first_overflow);
}

// Validates the stored count against the leaf capacity.
// Returns true when usable; false with `error` naming the mismatch.
bool LeafPage::attach(std::string& error) const {
    return EntryPage::attach(error);
}

// The chain successor (kNoPage at the end of the chain).
std::uint32_t LeafPage::next_leaf() const {
    return read_u32(buffer_, kNextLeafOffset);
}

// Re-points the chain successor.
// `page_id` — the new next-leaf block id.
void LeafPage::set_next_leaf(std::uint32_t page_id) {
    write_u32(buffer_, kNextLeafOffset, page_id);
}

// The head of this leaf's overflow chain (kNoPage when none).
std::uint32_t LeafPage::first_overflow() const {
    return read_u32(buffer_, kFirstOverflowOffset);
}

// Re-points the overflow-chain head.
// `page_id` — the new first-overflow block id.
void LeafPage::set_first_overflow(std::uint32_t page_id) {
    write_u32(buffer_, kFirstOverflowOffset, page_id);
}

// --- OverflowPage -----------------------------------------------------------

// Binds an overflow view: 8 header bytes (count, next overflow) and the
// overflow capacity.
// `buffer` — the pinned block; `key_width` — the key width.
void OverflowPage::bind(char* buffer, std::size_t key_width) {
    EntryPage::bind(buffer, key_width, 8, overflow_capacity(key_width));
}

// Initializes the page: zero entries, chain link as given.
// `next_overflow` — the next run in the chain (kNoPage ends it).
void OverflowPage::format(std::uint32_t next_overflow) {
    write_u32(buffer_, 0, 0);
    write_u32(buffer_, kNextOverflowOffset, next_overflow);
}

// Validates the stored count against the overflow capacity.
// Returns true when usable; false with `error` naming the mismatch.
bool OverflowPage::attach(std::string& error) const {
    return EntryPage::attach(error);
}

// The next run in the chain (kNoPage at the end).
std::uint32_t OverflowPage::next_overflow() const {
    return read_u32(buffer_, kNextOverflowOffset);
}

// Re-points the chain link.
// `page_id` — the new next-overflow block id.
void OverflowPage::set_next_overflow(std::uint32_t page_id) {
    write_u32(buffer_, kNextOverflowOffset, page_id);
}

// --- SeparatorPage ----------------------------------------------------------

// Binds a separator view: count u32, index entries of (key + child),
// the first child after the array, and the separator capacity.
// `buffer` — the pinned block; `key_width` — the key width.
void SeparatorPage::bind(char* buffer, std::size_t key_width) {
    buffer_ = buffer;
    key_width_ = key_width;
    capacity_ = separator_capacity(key_width);
}

// Initializes the page: zero separators, first child = kNoPage.
void SeparatorPage::format() {
    write_u32(buffer_, 0, 0);
    write_u32(buffer_, kCountSize, kNoPage);
}

// Validates the stored separator count against the capacity.
// Returns true when usable; false with `error` naming the mismatch.
bool SeparatorPage::attach(std::string& error) const {
    const std::size_t stored = count();
    if (stored > capacity_) {
        error = "separator page holds " + std::to_string(stored) +
                " separators, capacity is " + std::to_string(capacity_);
        return false;
    }
    return true;
}

// The stored separator count.
std::size_t SeparatorPage::count() const {
    return read_u32(buffer_, 0);
}

// The child routed to by separator i.
// `i` — the separator index.
// Returns the child page id (the entry's P).
std::uint32_t SeparatorPage::child(std::size_t i) const {
    return read_u32(buffer_, entry_offset(i) + key_width_);
}

// Raw encoded key bytes of separator i.
// `i` — the separator index.
// Returns a pointer into the pinned block, key_width bytes long.
const char* SeparatorPage::separator_key(std::size_t i) const {
    return buffer_ + entry_offset(i);
}

// Copies out index entry i as its named (key, child) form.
// `i` — the separator index.
// Returns the IndexEntry.
IndexEntry SeparatorPage::entry(std::size_t i) const {
    IndexEntry index_entry;
    index_entry.key.resize(key_width_);
    const char* raw = buffer_ + entry_offset(i);
    std::memcpy(index_entry.key.data(), raw, key_width_);
    std::memcpy(&index_entry.child, raw + key_width_, 4);
    return index_entry;
}

// The leftmost child (P0), stored right after the entry array.
// Returns the child page id.
std::uint32_t SeparatorPage::first_child() const {
    // The first child sits right after the entry array.
    return read_u32(buffer_, kCountSize + count() * (key_width_ + 4));
}

// Writes the leftmost child (P0).
// `page_id` — the first-child block id; call after all appends, the
// slot moves with the entry count.
void SeparatorPage::set_first_child(std::uint32_t page_id) {
    write_u32(buffer_, kCountSize + count() * (key_width_ + 4), page_id);
}

// Appends one index entry at the end and re-initializes the
// first-child slot the append moved up.
// `key` — encoded separator key, key_width bytes; `child_id` — the
// child routed to by this separator.
// Returns true on success; false with `error` set when full.
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

// The IndexEntry form of append: same contract.
// `index_entry` — the (key, child) pair to append.
// Returns as the raw-bytes form above.
bool SeparatorPage::append(const IndexEntry& index_entry,
                           std::string& error) {
    return append(index_entry.key.data(), index_entry.child, error);
}

// The descent step: the child after the last separator strictly less
// than `key`, else the first child.
// `key` — encoded key bytes; `codec` — the order-defining codec.
// Returns the child page id to descend into.
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
