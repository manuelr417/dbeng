#pragma once

// IndexPages — the three page views over ISAM blocks, mirroring the
// SlottedPage discipline: non-owning views bound to a pinned frame's
// buffer, format/attach the only writers of structural fields, attach
// validating stored counts against capacity. The heap's one homogeneous
// page kind becomes three: SeparatorPage (index pages, frozen after
// build), LeafPage (data pages, sorted (key, rid) arrays with the leaf
// chain and overflow link), and OverflowPage (sorted spill chains).
// LeafPage and OverflowPage share their entry machinery through
// EntryPage. Entry erase is deliberately absent — ex13 has no caller
// for it; when a later exercise adds it, it leaves gaps (SlottedPage
// semantics), never compacts.

#include "index/KeyCodec.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace isam {

// An index-local row address: the (page, slot) of the record in the
// heap file. Kept index-local so index/ never needs heapfile/ headers.
struct Rid {
    std::uint32_t page_id = 0;
    std::uint32_t slot = 0;
};

// Row-address equality: same page and same slot.
inline bool operator==(const Rid& a, const Rid& b) {
    return a.page_id == b.page_id && a.slot == b.slot;
}

constexpr std::uint32_t kNoPage = 0xFFFFFFFFu;

// 4096-byte block size, taken from the buffer pool's frame size.
constexpr std::size_t kBlockSize = 4096;

// One DATA entry in its in-flight form — the lecture's "data entries":
// a pair of search key and rid, living at the leaf level. The encoded
// key is exactly codec.width() bytes (pages hold and compare bytes,
// only KeyCodec decodes); on a page the pair is laid out at
// entry_offset(i), and this struct is what callers hand in and get back.
struct DataEntry {
    std::string key;   // encoded key bytes, exactly codec.width() long
    Rid rid;
};

// One INDEX entry — the lecture's "index entries": a (search key, child
// page) pair that guides traffic in the separator pages above the
// leaves. A SeparatorPage is a page packed with these plus its first
// child: P0 | K1 | P1 | ... | Km | Pm.
struct IndexEntry {
    std::string key;   // encoded separator key, exactly codec.width() long
    std::uint32_t child = kNoPage;
};

// --- capacities ------------------------------------------------------------

// Entries a separator page holds: count u32 + entries (key + child u32)
// + first child u32.
// `key_width` — the fixed key width in bytes.
// Returns the maximum index-entry count per separator page.
std::size_t separator_capacity(std::size_t key_width);

// Entries a leaf page holds: count u32 + next-leaf u32 +
// first-overflow u32 + entries (key + rid).
// `key_width` — the fixed key width in bytes.
// Returns the maximum data-entry count per leaf page.
std::size_t leaf_capacity(std::size_t key_width);

// Entries an overflow page holds: count u32 + next-overflow u32 +
// entries (key + rid).
// `key_width` — the fixed key width in bytes.
// Returns the maximum data-entry count per overflow page.
std::size_t overflow_capacity(std::size_t key_width);

// --- EntryPage: the sorted (key, rid) array shared by leaf and overflow ----

class EntryPage {
public:
    // Binds the entry region of a page view.
    // `buffer` — the pinned frame's 4096-byte block.
    // `key_width` — the fixed key width in bytes.
    // `entries_offset` — page-header bytes before the first entry.
    // `capacity` — the maximum entry count this layout admits.
    void bind(char* buffer, std::size_t key_width, std::size_t entries_offset,
              std::size_t capacity);

    // The stored entry count (the u32 at offset 0).
    std::size_t count() const;

    // The bound maximum entry count.
    std::size_t capacity() const { return capacity_; }

    // Validates the stored count against the bound capacity: corrupted
    // or foreign bytes are a hard error, exactly like SlottedPage::attach.
    // Returns true when the page is usable; false with `error` naming
    // the mismatch otherwise.
    bool attach(std::string& error) const;

    // First entry index whose key is >= `key`, by binary search.
    // `key` — encoded key bytes, exactly codec.width() long.
    // `codec` — the codec whose compare defines the order.
    // Returns the first position with key >= `key` (count() when every
    // stored key is smaller).
    std::size_t lower_bound(const std::string& key,
                            const KeyCodec& codec) const;

    // The data entry at slot i.
    // `i` — the entry index, must be < count().
    // Returns the (key, rid) pair; throws std::out_of_range for
    // i >= count() (the container-like convention).
    DataEntry get_entry(std::size_t i) const;

    // Sorted insert: positions by lower_bound, memmoves the tail one
    // slot, writes the entry, bumps the count — order preserved, and
    // never a half-written entry (the full-page refusal is the only
    // failure).
    // `entry` — the (encoded key, rid) pair to store.
    // `codec` — the codec whose compare defines the order.
    // Returns true on success; false with `error` set when the page is
    // full.
    bool insert_entry(const DataEntry& entry, const KeyCodec& codec,
                      std::string& error);

protected:
    // Byte offset of entry i: header bytes + i * (key + rid) stride.
    std::size_t entry_offset(std::size_t i) const {
        return entries_offset_ + i * (key_width_ + 8);
    }

    // Writes the entry count (the u32 at offset 0).
    // `count` — the new entry count.
    void write_count(std::size_t count);

    char* buffer_ = nullptr;
    std::size_t key_width_ = 0;
    std::size_t entries_offset_ = 0;
    std::size_t capacity_ = 0;
};

// --- LeafPage: a data page of the index ------------------------------------

class LeafPage : public EntryPage {
public:
    // Binds a leaf view: 12 header bytes (count, next leaf, first
    // overflow) and the leaf capacity.
    // `buffer` — the pinned frame's block; `key_width` — the key width.
    void bind(char* buffer, std::size_t key_width);

    // Initializes the page: zero entries, links as given. prev is not
    // stored — the chain is walked forward only.
    // `next_leaf` — the chain successor (kNoPage ends the chain).
    // `first_overflow` — the overflow-chain head (kNoPage when none).
    void format(std::uint32_t next_leaf, std::uint32_t first_overflow);

    // Validates the page: the stored count must fit the leaf capacity
    // (the EntryPage check).
    // Returns true when usable; false with `error` naming the mismatch.
    bool attach(std::string& error) const;

    // The chain successor (kNoPage at the end of the chain).
    std::uint32_t next_leaf() const;
    // Re-points the chain successor.
    // `page_id` — the new next-leaf block id.
    void set_next_leaf(std::uint32_t page_id);
    // The head of this leaf's overflow chain (kNoPage when none).
    std::uint32_t first_overflow() const;
    // Updates the overflow-chain head when an insert adds a new overflow
    // page. Inserts also modify the leaf's entries while it has room.
    // `page_id` — the new first-overflow block id.
    void set_first_overflow(std::uint32_t page_id);

private:
    static constexpr std::size_t kNextLeafOffset = 4;
    static constexpr std::size_t kFirstOverflowOffset = 8;
};

// --- OverflowPage: one sorted run hanging off a leaf ------------------------

class OverflowPage : public EntryPage {
public:
    // Binds an overflow view: 8 header bytes (count, next overflow) and
    // the overflow capacity.
    // `buffer` — the pinned frame's block; `key_width` — the key width.
    void bind(char* buffer, std::size_t key_width);

    // Initializes the page: zero entries, chain link as given.
    // `next_overflow` — the next run in the chain (kNoPage ends it).
    void format(std::uint32_t next_overflow);

    // Validates the page: the stored count must fit the overflow
    // capacity (the EntryPage check).
    // Returns true when usable; false with `error` naming the mismatch.
    bool attach(std::string& error) const;

    // The next run in the chain (kNoPage at the end).
    std::uint32_t next_overflow() const;
    // Re-points the chain link.
    // `page_id` — the new next-overflow block id.
    void set_next_overflow(std::uint32_t page_id);

private:
    static constexpr std::size_t kNextOverflowOffset = 4;
};

// --- SeparatorPage: an index page (internal), frozen after build -----------

class SeparatorPage {
public:
    // Binds a separator view: count u32, index entries of
    // (key + child u32), the first child after the array, and the
    // separator capacity.
    // `buffer` — the pinned frame's block; `key_width` — the key width.
    void bind(char* buffer, std::size_t key_width);

    // Initializes the page: zero separators, first child = kNoPage.
    void format();

    // Validates the stored separator count against the capacity.
    // Returns true when usable; false with `error` naming the mismatch.
    bool attach(std::string& error) const;

    // The stored separator count.
    std::size_t count() const;
    // The bound maximum separator count.
    std::size_t capacity() const { return capacity_; }

    // The child routed to by separator i (the entry's P).
    // `i` — the separator index, must be < count().
    // Returns the child page id.
    std::uint32_t child(std::size_t i) const;
    // Raw encoded key bytes of separator i (the entry's K).
    // `i` — the separator index, must be < count().
    // Returns a pointer into the pinned block, key_width bytes long,
    // valid only while the page stays pinned and unmutated.
    const char* separator_key(std::size_t i) const;
    // The i-th index entry — the named form of the (key, child) pair.
    // `i` — the separator index, must be < count().
    // Returns a copy of the (key, child) pair.
    IndexEntry entry(std::size_t i) const;
    // The leftmost child (P0), stored after the entry array.
    // Returns the child page id.
    std::uint32_t first_child() const;
    // Writes the leftmost child; call after all appends — the slot
    // moves with the entry count.
    // `page_id` — the first-child block id.
    void set_first_child(std::uint32_t page_id);

    // Build-time writer: appends one index entry at the end (ascending
    // key order is the caller's duty) and re-initializes the
    // first-child slot the append moved up.
    // `key` — encoded separator key, key_width bytes.
    // `child` — the child page routed to by this separator.
    // Returns true on success; false with `error` set when full.
    bool append(const char* key, std::uint32_t child, std::string& error);
    // The IndexEntry form of append.
    // `index_entry` — the (key, child) pair to append.
    // Returns as the raw-bytes form above.
    bool append(const IndexEntry& index_entry, std::string& error);

    // The descent rule, in one place: the child after the last separator
    // STRICTLY less than `key` — the leftmost leaf that may hold the key,
    // duplicates included (equal keys can lag one leaf behind their
    // separator, so `<=` would overshoot); when no separator is smaller,
    // the first child. Walkers move forward from there.
    // `key` — encoded key bytes; `codec` — the order-defining codec.
    // Returns the child page id to descend into.
    std::uint32_t descend_step(const char* key, const KeyCodec& codec) const;

private:
    // Byte offset of index entry i: count u32 + i * (key + child) stride.
    std::size_t entry_offset(std::size_t i) const {
        return kCountSize + i * (key_width_ + 4);
    }

    static constexpr std::size_t kCountSize = 4;

    char* buffer_ = nullptr;
    std::size_t key_width_ = 0;
    std::size_t capacity_ = 0;
};

}
