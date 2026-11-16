#pragma once

// ExtensibleHashIndex — a single-column extensible hash index over one
// heap file, stored in its own file of 4096-byte blocks (lecture 11).
// Structure, in three page castes:
//
//   directory pages  the slot array: the last `global depth` bits of a
//                    key's hash select a slot, each slot holds a bucket
//                    page id; the directory doubles when a split at the
//                    global depth needs more slots
//   bucket pages     primary buckets: a local depth, an overflow link,
//                    and (key, rid) entries in arrival order
//   overflow pages   per-bucket spill chains for entries a split cannot
//                    redistribute (duplicate-crowded buckets)
//
// Block 0 is a header page: it identifies the file (magic, key type and
// width, table and column names) and holds the global depth, the first
// directory block, the entry count, and the highest allocated block.
// The directory is read once at open into an in-memory cache and
// written through on every change (D3) — a lookup pays one bucket pin,
// not a directory walk. The index never reads or writes heap records;
// callers hand in values and row addresses, and verify() judges rids
// through a caller-supplied occupancy callback.
//
// One encoding: keys move through index/KeyCodec only (the one key
// encoding shared with the ISAM module); failures throw `common::Error`
// carrying the same named messages the bool-plus-error convention used.

#include "bufpool/BufferManager.h"
#include "common/PinnedPage.h"
#include "hashindex/HashPages.h"
#include "schema/TableSchema.h"
#include "tuple/Column.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace hashindex {

class ExtensibleHashIndex {
public:
    // Binds the index to a buffer manager, a table's schema, and the one
    // column it hashes. The schema must outlive the index.
    ExtensibleHashIndex(bufman::BufferManager& manager,
                        const schema::TableSchema& schema,
                        std::size_t column_index);

    bool is_open() const { return open_; }
    std::size_t entry_count() const { return entry_count_; }
    std::uint32_t global_depth() const { return global_depth_; }
    const isam::KeyCodec& codec() const { return codec_; }

    // Creates a fresh, empty index file at `path` (header block and one
    // directory block) and leaves it open. Throws for an existing
    // non-empty file, an empty file that is not a hash index file, and
    // a column with no record-able key width.
    void create(const std::string& path);

    // Opens an existing index file, validating magic, key type and
    // width, and the table and column names against the bound schema,
    // and loads the directory chain into memory.
    void open(const std::string& path);

    // Build: a fresh file filled from `entries` in one pass — create,
    // then one insert per entry (hashing needs no sort). Leaves the
    // index open. Throws for an existing non-empty file.
    void build(const std::string& path,
               const std::vector<std::pair<tuple::Column, Rid>>& entries);

    // Flushes and closes this index's file through the shared manager.
    void close();

    // All entries whose key equals `key`, in page order (duplicates
    // contiguous). Routes through the directory to the owning bucket
    // and scans it and its overflow chain.
    void lookup(const tuple::Column& key, std::vector<Rid>& out);

    // Enters one (key, rid) pair: routes through the directory to the
    // owning bucket and stores there — a roomy bucket takes it, a
    // bucket with an overflow chain spills into the chain, and a full
    // chainless bucket splits (D5): entries redistribute by bit `local
    // depth` of their hashes, the directory doubles exactly when the
    // splitting bucket's local depth equals the global depth, and a
    // split that cannot separate (one side empty) is abandoned to a
    // fresh overflow page. A bucket that owns an overflow chain never
    // splits again.
    void insert(const tuple::Column& key, Rid rid);

    // Removes the one entry matching (key, rid) exactly: the owning
    // bucket and its overflow chain. No merge, no shrink, no depth
    // change — the vacated space stays as a gap the page may refill,
    // and an emptied bucket stays allocated. Throws when no stored
    // entry matches the pair.
    void erase(const tuple::Column& key, Rid rid);

    // Walks the directory, every distinct bucket, and every overflow
    // chain, and reports named findings: entries whose rid fails
    // `occupied` (dangling), entries (or routing slots) whose low
    // local-depth bits disagree with the bucket's pattern (misplaced),
    // a bucket whose local depth exceeds the global depth, and a walked
    // total that disagrees with the header. `entry_count` receives the
    // walked count.
    void verify(const std::function<bool(const Rid&)>& occupied,
                std::size_t& entry_count, std::vector<std::string>& findings);

private:
    // Header block (block 0) field offsets.
    struct Header {
        std::uint32_t magic = 0;
        std::uint32_t key_type = 0;
        std::uint32_t key_width = 0;
        std::uint32_t global_depth = 1;
        std::uint32_t first_directory = kNoPage;
        std::uint32_t last_block = kNoPage;
        std::uint64_t entry_count = 0;
        std::string table_name;
        std::string column_name;
    };

    void write_header();
    void read_header(Header& header);
    void store_header(const Header& header, char* buffer);

    // The directory chain (D3): first_directory_ chains DirectoryPages
    // holding the slots; load_directory reads them all into directory_,
    // store_directory writes directory_ back through the chain,
    // extending it when a doubling needs more blocks.
    void load_directory();
    void store_directory();

    // Pins a block, binds the requested view, and attaches the page;
    // the guard owns the pin from here to the caller's scope end.
    common::PinnedPage<DirectoryPage> pin_directory(std::uint32_t block);
    common::PinnedPage<BucketPage> pin_bucket(std::uint32_t block);
    common::PinnedPage<OverflowPage> pin_overflow(std::uint32_t block);

    // Allocates the next block id beyond `last_block_`.
    void alloc_next_block(std::uint32_t& block, std::size_t& frame);

    // The directory slot a key hashes to at the current global depth:
    // the last `global depth` bits of the key's hash.
    std::uint32_t slot_for(const char* key_bytes) const;

    // Lookup workers (D6): one bucket's overflow chain, then the bucket
    // itself. Each worker pins one page and collects.
    void scan_overflow_chain(std::uint32_t block,
                             const std::string& key_bytes,
                             std::vector<Rid>& out);
    void scan_bucket(std::uint32_t block, const std::string& key_bytes,
                     std::vector<Rid>& out);

    // Insert workers (D5): the first overflow page of a chain with room
    // for one entry (kNoPage when all are full), the spill that inserts
    // there or front-chains a new page, and the split of a full
    // chainless bucket.
    std::uint32_t find_overflow_with_room(std::uint32_t block);
    void extend_overflow(std::uint32_t bucket_block,
                         std::uint32_t chain_head,
                         const std::string& key_bytes, Rid rid);
    void split_bucket(std::uint32_t bucket_block,
                      const std::string& key_bytes, Rid rid);
    void insert_into_bucket(std::uint32_t bucket_block,
                            const std::string& key_bytes, Rid rid);

    // Erase workers (D6): one page's exact (key, rid) search — the
    // entry's index, or count() when absent (both bucket and overflow
    // pages are HashEntryPages) — then the bucket page and its
    // overflow chain.
    std::size_t find_exact(const HashEntryPage& page,
                           const std::string& key_bytes, Rid rid) const;
    void erase_from_overflow(std::uint32_t block,
                             const std::string& key_bytes, Rid rid,
                             bool& found);
    void erase_from_bucket(std::uint32_t block,
                           const std::string& key_bytes, Rid rid,
                           bool& found);

    // Verify workers (D11): one overflow chain against its bucket's
    // expected pattern, then the bucket walk itself.
    void verify_overflow_chain(
        std::uint32_t block, const std::function<bool(const Rid&)>& occupied,
        std::uint32_t mask, std::uint32_t expected, const std::string& label,
        std::size_t& walked, std::vector<std::string>& findings);

    bufman::BufferManager& manager_;
    const schema::TableSchema& schema_;
    std::size_t column_index_ = 0;
    isam::KeyCodec codec_;

    bufman::File file_{};
    std::string path_;
    bool open_ = false;

    std::uint32_t global_depth_ = 1;
    std::uint32_t first_directory_ = kNoPage;
    std::uint32_t last_block_ = kNoPage;
    std::size_t entry_count_ = 0;
    std::vector<std::uint32_t> directory_;   // the in-memory directory
};

}
