#pragma once

// StaticHashIndex — a single-column static hash index over one heap
// file, stored in its own file of 4096-byte blocks: lecture 11's
// baseline scheme, the extensible index without its growth machinery.
// Structure, in three page castes:
//
//   directory pages  the fixed bucket array: `h(key) mod N` selects a
//                    slot, each slot holds the page id of one of the N
//                    primary buckets; the array never grows or shrinks
//   bucket pages     primary buckets: an overflow link and (key, rid)
//                    entries in arrival order — no local-depth field
//   overflow pages   per-bucket spill chains for entries a full bucket
//                    cannot take (the extensible module's OverflowPage
//                    reused verbatim)
//
// Block 0 is a header page: it identifies the file (magic, key type and
// width, table and column names) and holds the bucket count, the first
// directory block, the entry count, and the highest allocated block.
// Create allocates every primary bucket up front — N buckets from
// birth, empty ones included — so inserts only fill pages and chain
// overflow; nothing ever splits, doubles, merges, or deallocates. The
// directory is read once at open into an in-memory cache and written
// through on every change (the extensible module's D3 discipline) — a
// lookup pays one bucket pin, not a directory walk. The index never
// reads or writes heap records; callers hand in values and row
// addresses, and verify() judges rids through a caller-supplied
// occupancy callback.
//
// One encoding: keys move through index/KeyCodec only (the one key
// encoding shared with the ISAM module); errors follow the house
// convention — bool plus a cleared error out-parameter carrying a named
// message.

#include "bufpool/BufferManager.h"
#include "hashindex/HashPages.h"
#include "hashindex/StaticHashPages.h"
#include "schema/TableSchema.h"
#include "tuple/Column.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace hashindex {

class StaticHashIndex {
public:
    // Binds the index to a buffer manager, a table's schema, and the one
    // column it hashes. The schema must outlive the index.
    StaticHashIndex(bufman::BufferManager& manager,
                    const schema::TableSchema& schema,
                    std::size_t column_index);

    bool is_open() const { return open_; }
    std::size_t entry_count() const { return entry_count_; }
    std::uint32_t bucket_count() const { return bucket_count_; }
    const isam::KeyCodec& codec() const { return codec_; }

    // Creates a fresh, empty index file at `path` with exactly
    // `bucket_count` primary buckets (the header, the directory chain,
    // and every bucket page) and leaves it open. Refuses a bucket count
    // of zero, an existing non-empty file, an empty file that is not a
    // static hash index file, and a column with no record-able key
    // width.
    bool create(const std::string& path, std::uint32_t bucket_count,
                std::string& error);

    // Opens an existing index file, validating magic, key type and
    // width, and the table and column names against the bound schema,
    // and loads the directory chain into memory.
    bool open(const std::string& path, std::string& error);

    // Build: a fresh file filled from `entries` in one pass — create,
    // then one insert per entry (hashing needs no sort). Leaves the
    // index open. Refuses an existing non-empty file.
    bool build(const std::string& path, std::uint32_t bucket_count,
               const std::vector<std::pair<tuple::Column, Rid>>& entries,
               std::string& error);

    // Flushes and closes this index's file through the shared manager.
    bool close(std::string& error);

    // All entries whose key equals `key`, in page order (duplicates
    // contiguous). Routes through the fixed bucket array to the owning
    // bucket and scans it and its overflow chain.
    bool lookup(const tuple::Column& key, std::vector<Rid>& out,
                std::string& error);

    // Enters one (key, rid) pair: routes through the bucket array to
    // the owning bucket and stores there — a roomy bucket takes it, and
    // a full bucket spills into its overflow chain (the first page with
    // room, else a fresh page front-chained). No split, no new primary
    // bucket, no directory change: the bucket count is fixed at create.
    bool insert(const tuple::Column& key, Rid rid, std::string& error);

    // Removes the one entry matching (key, rid) exactly: the owning
    // bucket and its overflow chain. No merge, no deallocation, no
    // relocation — the vacated space stays as a gap the page may
    // refill. Fails with a named error when no stored entry matches the
    // pair.
    bool erase(const tuple::Column& key, Rid rid, std::string& error);

    // Walks the bucket array, every primary bucket, and every overflow
    // chain, and reports named findings: entries whose rid fails
    // `occupied` (dangling), entries whose hash does not select their
    // owning slot (misplaced), and a walked total that disagrees with
    // the header. `entry_count` receives the walked count.
    bool verify(const std::function<bool(const Rid&)>& occupied,
                std::size_t& entry_count, std::vector<std::string>& findings,
                std::string& error);

private:
    // Header block (block 0) field offsets — the extensible header with
    // the bucket count in place of the global depth.
    struct Header {
        std::uint32_t magic = 0;
        std::uint32_t key_type = 0;
        std::uint32_t key_width = 0;
        std::uint32_t bucket_count = 0;
        std::uint32_t first_directory = kNoPage;
        std::uint32_t last_block = kNoPage;
        std::uint64_t entry_count = 0;
        std::string table_name;
        std::string column_name;
    };

    bool write_header(std::string& error);
    bool read_header(Header& header, std::string& error);
    void store_header(const Header& header, std::size_t frame);

    // The directory chain (D3): first_directory_ chains DirectoryPages
    // holding the slots; load_directory reads them all into directory_,
    // store_directory writes directory_ back through the chain. The
    // array holds exactly bucket_count_ slots for the file's lifetime.
    bool load_directory(std::string& error);
    bool store_directory(std::string& error);

    // Pins a block and binds the requested view; unpins on failure.
    bool pin_directory(std::uint32_t block, DirectoryPage& page,
                       std::size_t& frame, std::string& error);
    bool pin_bucket(std::uint32_t block, StaticBucketPage& page,
                    std::size_t& frame, std::string& error);
    bool pin_overflow(std::uint32_t block, OverflowPage& page,
                      std::size_t& frame, std::string& error);

    // Allocates the next block id beyond `last_block_`.
    bool alloc_next_block(std::uint32_t& block, std::size_t& frame,
                          std::string& error);

    // The bucket a key hashes to at this file's fixed bucket count:
    // `h(key) mod N` (the lecture's static routing; for a power-of-two
    // N it is the last-d-bits rule).
    std::uint32_t slot_for(const char* key_bytes) const;

    // Lookup workers: one bucket's overflow chain, then the bucket
    // itself. Each worker pins one page, collects, unpins.
    bool scan_overflow_chain(std::uint32_t block,
                             const std::string& key_bytes,
                             std::vector<Rid>& out, std::string& error);
    bool scan_bucket(std::uint32_t block, const std::string& key_bytes,
                     std::vector<Rid>& out, std::string& error);

    // Insert workers: the first overflow page of a chain with room for
    // one entry (kNoPage when all are full), and the spill that inserts
    // there or front-chains a new page.
    std::uint32_t find_overflow_with_room(std::uint32_t block,
                                          std::string& error);
    bool extend_overflow(std::uint32_t bucket_block,
                         std::uint32_t chain_head,
                         const std::string& key_bytes, Rid rid,
                         std::string& error);
    bool insert_into_bucket(std::uint32_t bucket_block,
                            const std::string& key_bytes, Rid rid,
                            std::string& error);

    // Erase workers: one page's exact (key, rid) search — the entry's
    // index, or count() when absent (both bucket and overflow pages are
    // HashEntryPages) — then the bucket page and its overflow chain.
    std::size_t find_exact(const HashEntryPage& page,
                           const std::string& key_bytes, Rid rid) const;
    bool erase_from_overflow(std::uint32_t block,
                             const std::string& key_bytes, Rid rid,
                             bool& found, std::string& error);
    bool erase_from_bucket(std::uint32_t block,
                           const std::string& key_bytes, Rid rid,
                           bool& found, std::string& error);

    // Verify workers: one overflow chain against its owning slot, then
    // the bucket walk itself.
    bool verify_overflow_chain(
        std::uint32_t block, const std::function<bool(const Rid&)>& occupied,
        std::uint32_t expected_slot, const std::string& label,
        std::size_t& walked, std::vector<std::string>& findings,
        std::string& error);

    bufman::BufferManager& manager_;
    const schema::TableSchema& schema_;
    std::size_t column_index_ = 0;
    isam::KeyCodec codec_;

    bufman::File file_{};
    std::string path_;
    bool open_ = false;

    std::uint32_t bucket_count_ = 0;
    std::uint32_t first_directory_ = kNoPage;
    std::uint32_t last_block_ = kNoPage;
    std::size_t entry_count_ = 0;
    std::vector<std::uint32_t> directory_;   // the in-memory bucket array
};

}
