#pragma once

// IsamIndex — a single-column ISAM index over one heap file, stored in
// its own file of 4096-byte blocks. Structure, in two castes:
//
//   index pages    separator pages, built bottom-up from the leaves'
//                  first keys during bulk load, FROZEN from then on —
//                  no insert ever touches one (that is what makes this
//                  ISAM and not a B+tree)
//   data pages     leaf pages holding sorted (key, rid) entries, chained
//                  left to right for range scans, plus per-leaf overflow
//                  chains that absorb post-build inserts
//
// Block 0 is a header page: it identifies the file (magic, key type and
// width, table and column names) and holds the root, first leaf, level
// count, entry count, and highest allocated block. The index never reads
// or writes heap records; callers hand in values and row addresses, and
// verify() judges rids through a caller-supplied occupancy callback.
//
// One encoding: keys move through KeyCodec only, entries through the
// IndexPages views only. Errors follow the house convention — bool plus
// a cleared error out-parameter carrying a named message.

#include "bufpool/BufferManager.h"
#include "index/IndexPages.h"
#include "index/KeyCodec.h"
#include "schema/TableSchema.h"
#include "tuple/Column.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace isam {

// One end of a range: a key value plus whether the bound includes it.
struct RangeBound {
    tuple::Column value;
    bool inclusive = true;
};

class IsamIndex {
public:
    // Binds the index to a buffer manager, a table's schema, and the one
    // column it orders. The schema must outlive the index.
    IsamIndex(bufman::BufferManager& manager, const schema::TableSchema& schema,
              std::size_t column_index);

    bool is_open() const { return open_; }
    std::size_t entry_count() const { return entry_count_; }
    const KeyCodec& codec() const { return codec_; }

    // Creates a fresh, empty index file at `path` (header block only)
    // and leaves it open. Refuses an existing non-empty file, an empty
    // file that is not an index file, and a column with no record-able
    // key width.
    bool create(const std::string& path, std::string& error);

    // Bulk build: a fresh file filled from `entries` in one pass —
    // sorted in memory, leaves filled left to right, separator levels
    // built bottom-up until one page (the root) holds a level. Leaves
    // the index open. Refuses an existing non-empty file.
    bool build(const std::string& path,
               const std::vector<std::pair<tuple::Column, Rid>>& entries,
               std::string& error);

    // Opens an existing index file, validating magic, key type and
    // width, and the table and column names against the bound schema.
    bool open(const std::string& path, std::string& error);

    // Flushes and closes this index's file through the shared manager.
    bool close(std::string& error);

    // All entries whose key equals `key`, in ascending rid-file order
    // (duplicates are contiguous by key).
    bool lookup(const tuple::Column& key, std::vector<Rid>& out,
                std::string& error);

    // Entries with lo <= key <= hi (bounds optional, inclusivity per
    // bound), in ascending key order, duplicates adjacent. Walks the
    // leaf chain from the first leaf that may hold `lo`.
    bool range(const std::optional<RangeBound>& lo,
               const std::optional<RangeBound>& hi, std::vector<Rid>& out,
               std::string& error);

    // Enters one (key, rid) pair: descends the frozen separators to the
    // owning leaf and inserts there, chaining a new overflow page when
    // the leaf and its overflow are full. Never restructures anything
    // above the leaves.
    bool insert(const tuple::Column& key, Rid rid, std::string& error);

    // Walks every entry (leaf chain, then each leaf's overflow chain)
    // and reports named findings: entries whose rid fails `occupied`
    // (dangling), pages whose entries are out of order, and a total that
    // disagrees with the header. `entry_count` receives the walked count.
    bool verify(const std::function<bool(const Rid&)>& occupied,
                std::size_t& entry_count, std::vector<std::string>& findings,
                std::string& error);

private:
    // Header block (block 0) field offsets.
    struct Header {
        std::uint32_t magic = 0;
        std::uint32_t key_type = 0;
        std::uint32_t key_width = 0;
        std::uint32_t root = kNoPage;
        std::uint32_t first_leaf = kNoPage;
        std::uint32_t levels = 1;
        std::uint32_t last_block = kNoPage;
        std::uint64_t entry_count = 0;
        std::string table_name;
        std::string column_name;
    };

    bool write_header(std::string& error);
    bool read_header(Header& header, std::string& error);
    void store_header(const Header& header, std::size_t frame);

    // Pins a block and binds the requested view; unpins on failure.
    bool pin_separator(std::uint32_t block, SeparatorPage& page,
                       std::size_t& frame, std::string& error);
    bool pin_leaf(std::uint32_t block, LeafPage& page, std::size_t& frame,
                  std::string& error);
    bool pin_overflow(std::uint32_t block, OverflowPage& page,
                      std::size_t& frame, std::string& error);

    // Descends from the root to the leaf that may hold `key_bytes` and
    // returns its block id (kNoPage for an empty index).
    std::uint32_t descend(const char* key_bytes, std::string& error);

    // The recursive form (design D2): pin one separator, take its
    // child, recurse; `levels_remaining` counts separator pages still
    // to cross, and zero means `block` is the landing leaf.
    std::uint32_t descend_from(std::uint32_t block,
                               std::uint32_t levels_remaining,
                               const char* key_bytes, std::string& error);

    // Lookup walkers (design D3): the leaf chain and, per leaf, its
    // overflow chain. Each worker pins one page, acts, unpins, then
    // recurses — no frame is held across a call.
    bool scan_overflow_chain(std::uint32_t block,
                             const std::string& key_bytes,
                             std::vector<Rid>& out, std::string& error);
    bool scan_leaf_chain(std::uint32_t block, const std::string& key_bytes,
                         std::vector<Rid>& out, std::string& error);

    // Range-scan workers (design D3): gather one overflow chain as
    // sorted runs, emit one leaf merged with its runs, and walk the
    // leaf chain until the upper bound says stop.
    struct RangeWalk {
        const std::string* lo_bytes = nullptr;  // null: no lower bound
        bool lo_inclusive = true;
        const std::string* hi_bytes = nullptr;  // null: no upper bound
        bool hi_inclusive = true;
        std::vector<Rid>* out = nullptr;
    };
    bool gather_overflow_chain(std::uint32_t block,
                               std::vector<DataEntry>& out,
                               std::string& error);
    bool emit_leaf_range(std::uint32_t block, RangeWalk& walk,
                         std::uint32_t& next_leaf, std::string& error);
    bool scan_leaf_range(std::uint32_t block, RangeWalk& walk,
                         std::string& error);

    // Verify workers (design D7): walk the leaf chain carrying the
    // previous leaf's last key, walking each leaf's overflow chain on
    // the way. Per-entry checks live in the shared check_entries
    // helper; the walk context carries the caller's callback and the
    // findings/total accumulators.
    struct VerifyWalk {
        const std::function<bool(const Rid&)>& occupied;
        std::vector<std::string>& findings;
        std::size_t& entry_count;
    };
    bool verify_overflow_chain(std::uint32_t block, VerifyWalk& walk,
                               std::string& error);
    bool verify_leaf_chain(std::uint32_t block,
                           const std::string& previous_last_key,
                           bool has_previous, VerifyWalk& walk,
                           std::string& error);

    // Insert workers (design D6): find the first overflow page of a
    // chain with room for one entry (kNoPage when all are full), and
    // the full-leaf spill that inserts there or front-chains a new page.
    std::uint32_t find_overflow_with_room(std::uint32_t block,
                                          std::string& error);
    bool insert_into_overflow(std::uint32_t leaf_block,
                              std::uint32_t existing_chain,
                              const std::string& key_bytes, Rid rid,
                              std::string& error);

    // Build workers (design D5): one page's first key + block id,
    // carried upward from leaves through separator levels.
    struct FirstKey {
        std::string key;
        std::uint32_t block = kNoPage;
    };
    bool encode_and_sort(
        const std::vector<std::pair<tuple::Column, Rid>>& entries,
        std::vector<DataEntry>& sorted, std::string& error);
    bool build_leaves(const std::vector<DataEntry>& sorted,
                      std::vector<FirstKey>& level, std::string& error);
    bool link_leaf_chain(const std::vector<FirstKey>& level,
                         std::size_t index, std::string& error);
    bool build_separator_level(std::vector<FirstKey> level,
                               std::string& error);

    // Allocates the next block id beyond `last_block_`.
    bool alloc_next_block(std::uint32_t& block, std::size_t& frame,
                          std::string& error);

    bufman::BufferManager& manager_;
    const schema::TableSchema& schema_;
    std::size_t column_index_ = 0;
    KeyCodec codec_;

    bufman::File file_{};
    std::string path_;
    bool open_ = false;

    std::uint32_t root_ = kNoPage;
    std::uint32_t first_leaf_ = kNoPage;
    std::uint32_t levels_ = 1;
    std::uint32_t last_block_ = kNoPage;
    std::size_t entry_count_ = 0;
};

}
