#pragma once

// HashPages — the page views over extensible-hash blocks, mirroring the
// IndexPages discipline: non-owning views bound to a pinned frame's
// buffer, format/attach the only writers of structural fields, attach
// validating stored counts against capacity. The lecture's structure in
// three page kinds: DirectoryPage (the slot array: last-d-bits of the
// hash -> bucket page), BucketPage (a primary bucket: local depth, its
// overflow link, (key, rid) entries in arrival order), and OverflowPage
// (one spill chain page). Bucket and Overflow share their entry
// machinery through HashEntryPage — unsorted, unlike the ISAM entry
// pages: hash buckets hold entries as they arrived and scan linearly.
//
// One encoding: keys move through index/KeyCodec only (the one key
// encoding both index kinds share); a row address is index/KeyCodec's
// module's Rid, so both index kinds speak one row-address vocabulary.

#include "index/IndexPages.h"
#include "index/KeyCodec.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace hashindex {

using isam::DataEntry;
using isam::Rid;

constexpr std::uint32_t kNoPage = isam::kNoPage;
constexpr std::size_t kBlockSize = isam::kBlockSize;

// The hash of one encoded key: the lecture's "binary pattern". Integer
// keys hash to their own bit pattern (the slides' h(4) = 100); Double
// and String keys hash with FNV-1a 32-bit over the encoded bytes.
// Deterministic and equality-faithful: the same key always hashes the
// same, distinct keys almost never collide.
std::uint32_t hash_key(const isam::KeyCodec& codec, const char* key_bytes);

// --- capacities -------------------------------------------------------------

// Directory page: count u32 + next-directory u32 + u32 slots.
std::size_t directory_capacity();
// Bucket page: count u32 + local-depth u32 + first-overflow u32 + entries.
std::size_t bucket_capacity(std::size_t key_width);
// Overflow page: count u32 + next-overflow u32 + entries.
std::size_t overflow_capacity(std::size_t key_width);

// --- HashEntryPage: the (key, rid) array shared by bucket and overflow ------

class HashEntryPage {
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

    // The entry at slot i. Throws std::out_of_range for i >= count()
    // (the container-like convention).
    DataEntry get_entry(std::size_t i) const;

    // Arrival-order append: the entry lands at the end. Refuses a full
    // page with a named error; never writes a half-entry.
    bool append_entry(const DataEntry& entry, std::string& error);

    // Removes the entry at slot i, shifting the tail down so the
    // remaining entries keep their arrival order. Throws
    // std::out_of_range for i >= count(). The vacated space stays as a
    // gap — nothing compacts.
    void remove_entry(std::size_t i);

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

// --- DirectoryPage: the slot array ------------------------------------------

class DirectoryPage {
public:
    void bind(char* buffer);

    // format: zero slots, next-directory = kNoPage.
    void format();
    bool attach(std::string& error) const;

    std::size_t count() const;
    std::size_t capacity() const { return capacity_; }

    std::uint32_t slot(std::size_t i) const;   // the bucket page at slot i
    void set_slot(std::size_t i, std::uint32_t page_id);

    std::uint32_t next_directory() const;
    void set_next_directory(std::uint32_t page_id);

private:
    static constexpr std::size_t kCountSize = 4;
    static constexpr std::size_t kNextOffset = 4;

    char* buffer_ = nullptr;
    std::size_t capacity_ = 0;
};

// --- BucketPage: a primary bucket -------------------------------------------

class BucketPage : public HashEntryPage {
public:
    void bind(char* buffer, std::size_t key_width);

    // format: zero entries, local depth and overflow link as given.
    void format(std::uint32_t local_depth, std::uint32_t first_overflow);

    bool attach(std::string& error) const;  // count check + HashEntryPage check

    std::uint32_t local_depth() const;
    void set_local_depth(std::uint32_t depth);
    std::uint32_t first_overflow() const;
    void set_first_overflow(std::uint32_t page_id);

private:
    static constexpr std::size_t kLocalDepthOffset = 4;
    static constexpr std::size_t kFirstOverflowOffset = 8;
};

// --- OverflowPage: one spill page hanging off a bucket -----------------------

class OverflowPage : public HashEntryPage {
public:
    void bind(char* buffer, std::size_t key_width);

    void format(std::uint32_t next_overflow);
    bool attach(std::string& error) const;  // count check + HashEntryPage check

    std::uint32_t next_overflow() const;
    void set_next_overflow(std::uint32_t page_id);

private:
    static constexpr std::size_t kNextOverflowOffset = 4;
};

}
