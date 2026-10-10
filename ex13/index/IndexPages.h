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

// Separator page: count u32 + entries (key + child u32) + first child.
std::size_t separator_capacity(std::size_t key_width);
// Leaf page: count u32 + next-leaf u32 + first-overflow u32 + entries.
std::size_t leaf_capacity(std::size_t key_width);
// Overflow page: count u32 + next-overflow u32 + entries.
std::size_t overflow_capacity(std::size_t key_width);

// --- EntryPage: the sorted (key, rid) array shared by leaf and overflow ----

class EntryPage {
public:
    // binds the entry region: `entries_offset` bytes of page header
    // before the first entry, `capacity` entries maximum.
    void bind(char* buffer, std::size_t key_width, std::size_t entries_offset,
              std::size_t capacity);

    std::size_t count() const;
    std::size_t capacity() const { return capacity_; }

    // Stored count must fit the capacity: corrupted or foreign bytes are
    // a hard error, exactly like SlottedPage::attach.
    bool attach(std::string& error) const;

    // First entry index whose key is >= `key` (binary search; `key` is
    // encoded, exactly codec.width() bytes).
    std::size_t lower_bound(const std::string& key,
                            const KeyCodec& codec) const;

    // The data entry at slot i. Throws std::out_of_range for i >= count()
    // (the container-like convention).
    DataEntry get_entry(std::size_t i) const;

    // Sorted insert: memmoves the tail to keep order. Refuses a full
    // page with a named error; never writes a half-entry.
    bool insert_entry(const DataEntry& entry, const KeyCodec& codec,
                      std::string& error);

protected:
    std::size_t entry_offset(std::size_t i) const {
        return entries_offset_ + i * (key_width_ + 8);
    }
    void write_count(std::size_t count);

    char* buffer_ = nullptr;
    std::size_t key_width_ = 0;
    std::size_t entries_offset_ = 0;
    std::size_t capacity_ = 0;
};

// --- LeafPage: a data page of the index ------------------------------------

class LeafPage : public EntryPage {
public:
    void bind(char* buffer, std::size_t key_width);

    // format: zero entries, links as given. prev is not stored — the
    // chain is walked forward only.
    void format(std::uint32_t next_leaf, std::uint32_t first_overflow);

    bool attach(std::string& error) const;  // count check + EntryPage check

    std::uint32_t next_leaf() const;
    void set_next_leaf(std::uint32_t page_id);
    std::uint32_t first_overflow() const;
    // The one leaf mutation inserts perform post-build: hanging (or
    // re-pointing) the overflow chain.
    void set_first_overflow(std::uint32_t page_id);

private:
    static constexpr std::size_t kNextLeafOffset = 4;
    static constexpr std::size_t kFirstOverflowOffset = 8;
};

// --- OverflowPage: one sorted run hanging off a leaf ------------------------

class OverflowPage : public EntryPage {
public:
    void bind(char* buffer, std::size_t key_width);

    void format(std::uint32_t next_overflow);
    bool attach(std::string& error) const;  // count check + EntryPage check

    std::uint32_t next_overflow() const;
    void set_next_overflow(std::uint32_t page_id);

private:
    static constexpr std::size_t kNextOverflowOffset = 4;
};

// --- SeparatorPage: an index page (internal), frozen after build -----------

class SeparatorPage {
public:
    void bind(char* buffer, std::size_t key_width);

    // format: zero separators, first child = kNoPage.
    void format();
    bool attach(std::string& error) const;

    std::size_t count() const;
    std::size_t capacity() const { return capacity_; }

    std::uint32_t child(std::size_t i) const;      // child routed by separator i
    const char* separator_key(std::size_t i) const;
    // The i-th index entry — the named form of the (key, child) pair.
    IndexEntry entry(std::size_t i) const;
    std::uint32_t first_child() const;             // the leftmost child
    void set_first_child(std::uint32_t page_id);

    // Build-time writers: appends one index entry in ascending key
    // order. Refuses a full page.
    bool append(const char* key, std::uint32_t child, std::string& error);
    bool append(const IndexEntry& index_entry, std::string& error);

    // The descent rule, in one place: the child after the last separator
    // STRICTLY less than `key` — the leftmost leaf that may hold the key,
    // duplicates included (equal keys can lag one leaf behind their
    // separator, so `<=` would overshoot); when no separator is smaller,
    // the first child. Walkers move forward from there.
    std::uint32_t descend_step(const char* key, const KeyCodec& codec) const;

private:
    std::size_t entry_offset(std::size_t i) const {
        return kCountSize + i * (key_width_ + 4);
    }

    static constexpr std::size_t kCountSize = 4;

    char* buffer_ = nullptr;
    std::size_t key_width_ = 0;
    std::size_t capacity_ = 0;
};

}
