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
    // `manager` — the shared buffer pool; `schema` — the table's schema;
    // `column_index` — the position of the indexed column in the schema.
    // An out-of-range column index leaves the codec unusable (valid()
    // false), so create/open/build refuse later.
    IsamIndex(bufman::BufferManager& manager, const schema::TableSchema& schema,
              std::size_t column_index);

    // True while a file is open (create/build/open succeeded and close
    // has not run).
    bool is_open() const { return open_; }

    // The number of (key, rid) entries the header records.
    std::size_t entry_count() const { return entry_count_; }

    // The key codec bound to the indexed column (width, compare,
    // encode/decode) for callers that must speak the same encoding.
    const KeyCodec& codec() const { return codec_; }

    // Creates a fresh, empty index file at `path` (header block only)
    // and leaves it open. Refuses an existing non-empty file, an empty
    // file that is not an index file, and a column with no record-able
    // key width.
    // `path` — the index file to create.
    // Returns true with the index open and empty (no root, no leaves);
    // false with `error` naming the refusal (already-open index, unusable
    // key width, existing blocks, allocation or write failure), the file
    // closed again.
    bool create(const std::string& path, std::string& error);

    // Bulk build: a fresh file filled from `entries` in one pass —
    // sorted in memory, leaves filled left to right, separator levels
    // built bottom-up until one page (the root) holds a level. Leaves
    // the index open. Refuses an existing non-empty file.
    // `path` — the index file to create.
    // `entries` — the (value, rid) pairs collected from the heap, in any
    // order, duplicates allowed.
    // Returns true with the index open and entry_count() set; false
    // with `error` naming the failed phase (create refusal, key
    // encoding, page allocation), the file closed again.
    bool build(const std::string& path,
               const std::vector<std::pair<tuple::Column, Rid>>& entries,
               std::string& error);

    // Opens an existing index file, validating magic, key type and
    // width, and the table and column names against the bound schema.
    // `path` — the index file to open.
    // Returns true with the header state loaded (root, first leaf,
    // levels, last block, entry count); false with `error` naming the
    // cause (missing file, no header block, foreign magic, differing
    // key type or width, foreign table or column name), the file closed
    // again.
    bool open(const std::string& path, std::string& error);

    // Flushes and closes this index's file through the shared manager
    // and resets every piece of in-memory state.
    // Returns true on success; false with `error` set when the index is
    // not open or the underlying close fails.
    bool close(std::string& error);

    // All entries whose key equals `key`, in leaf/overflow traversal
    // order. Matching rids are not sorted by page or slot.
    // `key` — the value to match.
    // `out` — receives the matching rids (cleared first; empty when the
    // index holds no such key).
    // Returns true on success, including zero matches; false with
    // `error` set when the index is not open, the value's type differs
    // from the key type, or a page read fails.
    bool lookup(const tuple::Column& key, std::vector<Rid>& out,
                std::string& error);

    // Entries with lo <= key <= hi (bounds optional, inclusivity per
    // bound), in ascending key order, duplicates adjacent. Walks the
    // leaf chain from the first leaf that may hold `lo`.
    // `lo`, `hi` — the optional bounds (nullopt: open on that side).
    // `out` — receives the matching rids (cleared first; empty for an
    // empty index or an empty range).
    // Returns true on success; false with `error` set when the index is
    // not open, a bound's type differs from the key type, or a page
    // read fails.
    bool range(const std::optional<RangeBound>& lo,
               const std::optional<RangeBound>& hi, std::vector<Rid>& out,
               std::string& error);

    // Enters one (key, rid) pair: descends the frozen separators to the
    // owning leaf and inserts there, chaining a new overflow page when
    // the leaf and its overflow are full. Never restructures anything
    // above the leaves.
    // `key` — the value to index; `rid` — the heap row address.
    // Returns true on success (entry_count() grown, header rewritten);
    // false with `error` set when the index is not open, the value's
    // type differs from the key type, or a page read/write fails.
    bool insert(const tuple::Column& key, Rid rid, std::string& error);

    // Walks every entry (leaf chain, then each leaf's overflow chain)
    // and reports named findings: entries whose rid fails `occupied`
    // (dangling), pages whose entries are out of order, and a total that
    // disagrees with the header.
    // `occupied` — callback judging whether an rid points at a live heap
    // row; `entry_count` — receives the walked total.
    // `findings` — receives one named string per problem (cleared first;
    // empty when the index is sound).
    // Returns true when the walk completed (findings may still be
    // non-empty); false with `error` set when the index is not open or
    // a page read fails.
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

    // Serializes the in-memory header image into block 0's buffer.
    // `header` — the image to store; `frame` — the pinned header frame.
    void store_header(const Header& header, std::size_t frame);

    // Builds the header image from the members and writes it to block 0.
    // Returns true on success; false with `error` set when block 0
    // cannot be pinned.
    bool write_header(std::string& error);

    // Loads the header image from block 0. Names longer than the
    // on-disk maximum are left empty rather than trusted.
    // `header` — receives the loaded fields.
    // Returns true on success; false with `error` set when block 0
    // cannot be pinned.
    bool read_header(Header& header, std::string& error);

    // Pins a block and binds the requested page view to its frame.
    // `block` — the block id to pin; `page` — receives the bound view;
    // `frame` — receives the pinned frame (the caller must unpin).
    // Returns true on success; false with `error` set when the pin or
    // the view's attach fails (the frame is unpinned again).
    bool pin_separator(std::uint32_t block, SeparatorPage& page,
                       std::size_t& frame, std::string& error);
    // The LeafPage form of the pin-and-bind above.
    bool pin_leaf(std::uint32_t block, LeafPage& page, std::size_t& frame,
                  std::string& error);
    // The OverflowPage form of the pin-and-bind above.
    bool pin_overflow(std::uint32_t block, OverflowPage& page,
                      std::size_t& frame, std::string& error);

    // Descends from the root to the leaf that may hold `key_bytes`.
    // `key_bytes` — the encoded search key.
    // Returns the landing leaf's block id; kNoPage for an empty index
    // or a failed separator read (`error` set in the latter case).
    std::uint32_t descend(const char* key_bytes, std::string& error);

    // The recursive form (design D2): pin one separator, take its
    // child, recurse; `levels_remaining` counts separator pages still
    // to cross, and zero means `block` is the landing leaf.
    // `block` — the page to descend from; `key_bytes` — the encoded
    // search key.
    // Returns the landing leaf's block id, or kNoPage when a separator
    // read fails (`error` set).
    std::uint32_t descend_from(std::uint32_t block,
                               std::uint32_t levels_remaining,
                               const char* key_bytes, std::string& error);

    // Lookup walkers (design D3): the leaf chain and, per leaf, its
    // overflow chain. Each worker pins one page, acts, unpins, then
    // recurses — no frame is held across a call.
    // `block` — the page (or chain head) to scan; `key_bytes` — the
    // encoded target key; `out` — receives matching rids in walk order.
    // Returns true when the walk ended normally; false with `error` set
    // when a page read fails.
    bool scan_overflow_chain(std::uint32_t block,
                             const std::string& key_bytes,
                             std::vector<Rid>& out, std::string& error);
    // The leaf-chain form: scan the landing leaf and its overflow
    // chain, then continue into the next leaf while its first key is
    // still <= the target (duplicates span leaves).
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
    // Gathers one overflow chain in chain order, as sorted runs.
    // `block` — the chain head (kNoPage ends the walk); `out` —
    // receives every entry of every page, unmerged.
    // Returns true when the chain ended; false with `error` set when a
    // page read fails.
    bool gather_overflow_chain(std::uint32_t block,
                               std::vector<DataEntry>& out,
                               std::string& error);

    // Emits one leaf for a range scan: its entries merged with its
    // overflow runs, stable-sorted, filtered by the walk's bounds, in
    // key order.
    // `block` — the leaf to emit; `walk` — the encoded bounds and the
    // output rid list; `next_leaf` — receives the leaf's chain successor
    // (kNoPage at the end of the chain).
    // Returns true on success; false with `error` set when a page read
    // fails.
    bool emit_leaf_range(std::uint32_t block, RangeWalk& walk,
                         std::uint32_t& next_leaf, std::string& error);

    // Walks the leaf chain of a range scan: emit each leaf, then
    // recurse while the chain continues and the upper bound admits the
    // next leaf's first key (no upper bound: to the chain's end).
    // `block` — the leaf to start from (kNoPage ends the walk);
    // `walk` — the encoded bounds and the output rid list.
    // Returns true when the walk ended; false with `error` set when a
    // page read fails.
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
    // Checks one overflow chain of a leaf: per-page sortedness and
    // dangling rids (the shared check), then recursion on the chain
    // link.
    // `block` — the chain head (kNoPage ends the walk); `walk` — the
    // caller's callback and the findings/total accumulators.
    // Returns true when the chain ended; false with `error` set when a
    // page read fails.
    bool verify_overflow_chain(std::uint32_t block, VerifyWalk& walk,
                               std::string& error);

    // Walks the leaf chain of a verify pass: per-entry checks on each
    // leaf, the chain-order finding against the previous leaf's last
    // key, each leaf's overflow chain, then the next leaf. An empty
    // leaf carries the previous key forward.
    // `block` — the leaf to check (kNoPage ends the walk).
    // `previous_last_key` — the previous leaf's last key.
    // `has_previous` — whether a previous key exists yet.
    // `walk` — the caller's callback and the accumulators.
    // Returns true when the chain ended; false with `error` set when a
    // page read fails.
    bool verify_leaf_chain(std::uint32_t block,
                           const std::string& previous_last_key,
                           bool has_previous, VerifyWalk& walk,
                           std::string& error);

    // The first page of an overflow chain with room for one more entry.
    // `block` — the chain head.
    // Returns that page's block id; kNoPage when every page is full —
    // or when a read failed, which `error` (set only on failure)
    // distinguishes.
    std::uint32_t find_overflow_with_room(std::uint32_t block,
                                          std::string& error);

    // The full-leaf spill: insert into the first overflow page with
    // room; when none has room, allocate a page, chain it at the FRONT
    // of the leaf's chain, and re-point the leaf.
    // `leaf_block` — the owning leaf (re-pointed on a new front page);
    // `existing_chain` — the leaf's current overflow-chain head;
    // `key_bytes` — the encoded key; `rid` — the row address.
    // Returns true on success (entry_count() grown, header rewritten);
    // false with `error` set when a read, write, or allocation fails.
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
    // Encodes every (value, rid) pair once and sorts by the encoded
    // bytes.
    // `entries` — the pairs to index, in any order.
    // `sorted` — receives the encoded entries, ascending by key.
    // Returns true on success; false with `error` set when a value
    // cannot be encoded.
    bool encode_and_sort(
        const std::vector<std::pair<tuple::Column, Rid>>& entries,
        std::vector<DataEntry>& sorted, std::string& error);

    // Fills leaves left to right from the sorted entries and reports
    // each leaf's first key + block id for the levels above. Leaves are
    // linked afterwards by link_leaf_chain, so next_leaf stays empty
    // here; with no entries the list is empty and the index stays
    // empty (root and first leaf kNoPage).
    // `sorted` — the entries, ascending by key; `level` — receives one
    // FirstKey per leaf.
    // Returns true on success; false with `error` set when a page
    // cannot be allocated.
    bool build_leaves(const std::vector<DataEntry>& sorted,
                      std::vector<FirstKey>& level, std::string& error);

    // Links the leaf chain forward, one leaf per frame: leaf i's
    // next_leaf becomes leaf i+1's block; the last leaf ends the chain.
    // `level` — the leaves in build order; `index` — the leaf being
    // linked (the recursion cursor).
    // Returns true on success; false with `error` set when a leaf read
    // fails.
    bool link_leaf_chain(const std::vector<FirstKey>& level,
                         std::size_t index, std::string& error);

    // Builds one separator level from the pages below and recurses
    // until a level fits in one page — that page is the root.
    // Separators are the first key of each child page except the
    // leftmost; each separator page stores the first child of its chunk
    // (written last: the slot moves with the entry count) plus one
    // (key, child) entry per remaining child. levels_ grows once per
    // level actually built.
    // `level` — the pages below, as (first key, block) pairs.
    // Returns true on success (root_ set); false with `error` set when
    // a page cannot be allocated or an entry cannot be appended.
    bool build_separator_level(std::vector<FirstKey> level,
                               std::string& error);

    // Allocates the next block id beyond `last_block_` (never the
    // on-disk block count — dirty pages lag the disk).
    // `block` — receives the new block id; `frame` — receives the
    // pinned frame (the caller must unpin).
    // Returns true on success; false with `error` set when the 32-bit
    // id range is exhausted or the allocation fails.
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
