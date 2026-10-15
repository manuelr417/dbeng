#include "index/IsamIndex.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace isam {

namespace {

// The identification record at the fixed offsets of block 0. Magic is
// "ISAM" read little-endian, matching the ex11 header-page pattern
// ("TUP1"): a foreign file is a magic error, not an arithmetic surprise.
constexpr std::uint32_t kIndexMagic = 0x4D415349u;  // "ISAM"
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kKeyTypeOffset = 4;
constexpr std::size_t kKeyWidthOffset = 8;
constexpr std::size_t kTableNameLengthOffset = 12;
constexpr std::size_t kTableNameOffset = 16;
constexpr std::size_t kMaxNameLength = 200;
constexpr std::size_t kColumnNameLengthOffset =
    kTableNameOffset + kMaxNameLength;              // 216
constexpr std::size_t kColumnNameOffset =
    kColumnNameLengthOffset + 4;                    // 220
constexpr std::size_t kRootOffset =
    kColumnNameOffset + kMaxNameLength;             // 420
constexpr std::size_t kFirstLeafOffset = kRootOffset + 4;
constexpr std::size_t kLevelsOffset = kRootOffset + 8;
constexpr std::size_t kEntryCountOffset = kRootOffset + 12;
constexpr std::size_t kLastBlockOffset = kRootOffset + 20;

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

// Reads the u64 at `offset` (native endian by memcpy).
// `buffer` — the block bytes; `offset` — byte offset of the field.
// Returns the field value.
std::uint64_t read_u64(const char* buffer, std::size_t offset) {
    std::uint64_t value = 0;
    std::memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

// Writes the u64 `value` at `offset` (native endian by memcpy).
// `buffer` — the block bytes; `offset` — byte offset of the field.
void write_u64(char* buffer, std::size_t offset, std::uint64_t value) {
    std::memcpy(buffer + offset, &value, sizeof(value));
}

// Releases a pin without disturbing `error`, which on failure paths
// already carries the real cause.
void release(bufman::BufferManager& manager, std::size_t frame,
             bool was_dirty) {
    std::string unpin_error;
    manager.unpin(frame, was_dirty, unpin_error);
}

// The equals-scan shared by leaf and overflow pages (D4): both are
// EntryPages holding sorted (key, rid) arrays, so one helper serves
// both. Appends the rid of every entry whose key equals `key_bytes`.
void collect_equals(const EntryPage& page, const KeyCodec& codec,
                    const std::string& key_bytes, std::vector<Rid>& out) {
    for (std::size_t i = page.lower_bound(key_bytes, codec);
         i < page.count(); ++i) {
        const DataEntry entry = page.get_entry(i);
        if (codec.compare(entry.key.data(), key_bytes.data()) != 0) {
            break;
        }
        out.push_back(entry.rid);
    }
}

// The per-entry integrity checks shared by leaf and overflow pages
// (D7): sortedness within the page and rid occupancy. `label` names
// the page in findings ("leaf 3", "overflow 5"). Returns the number of
// entries checked, so the walk's running total stays with the walker.
std::size_t check_entries(const EntryPage& page, const KeyCodec& codec,
                          const std::string& label,
                          const std::function<bool(const Rid&)>& occupied,
                          std::vector<std::string>& findings) {
    std::size_t checked = 0;
    for (std::size_t i = 0; i < page.count(); ++i) {
        const DataEntry entry = page.get_entry(i);
        if (i > 0) {
            const DataEntry previous = page.get_entry(i - 1);
            if (codec.compare(previous.key.data(), entry.key.data()) > 0) {
                findings.push_back(label + ": entries out of order near key " +
                                   codec.display(entry.key.data()));
            }
        }
        if (!occupied(entry.rid)) {
            findings.push_back("dangling entry: key " +
                               codec.display(entry.key.data()) + " rid (" +
                               std::to_string(entry.rid.page_id) + "," +
                               std::to_string(entry.rid.slot) + ")");
        }
        checked += 1;
    }
    return checked;
}

}  // namespace

// Binds the index to a manager, schema, and column, and derives the
// key codec from the column's type and declared width.
// `manager` — the shared buffer pool; `schema` — the table's schema;
// `column_index` — the position of the indexed column.
IsamIndex::IsamIndex(bufman::BufferManager& manager,
                     const schema::TableSchema& schema,
                     std::size_t column_index)
    : manager_(manager), schema_(schema), column_index_(column_index) {
    if (column_index < schema.column_count()) {
        const schema::ColumnSchema& column = schema.column_info(column_index);
        codec_ = KeyCodec(column.column_type(),
                          column.column_type() == tuple::ColumnType::String
                              ? column.column_size()
                              : 0);
    }
}

// --- header -----------------------------------------------------------------

// Serializes the header image into the pinned block 0 at the fixed
// offsets: magic, key type and width, the two names with lengths, and
// the structural fields.
// `header` — the image to store; `frame` — the pinned header frame.
void IsamIndex::store_header(const Header& header, std::size_t frame) {
    char* buffer = manager_.frame(frame).buffer;  // the pinned header frame
    write_u32(buffer, kMagicOffset, kIndexMagic);
    write_u32(buffer, kKeyTypeOffset,
              static_cast<std::uint32_t>(codec_.key_type()));
    write_u32(buffer, kKeyWidthOffset,
              static_cast<std::uint32_t>(codec_.width()));
    write_u32(buffer, kTableNameLengthOffset,
              static_cast<std::uint32_t>(header.table_name.size()));
    std::memcpy(buffer + kTableNameOffset, header.table_name.data(),
                header.table_name.size());
    write_u32(buffer, kColumnNameLengthOffset,
              static_cast<std::uint32_t>(header.column_name.size()));
    std::memcpy(buffer + kColumnNameOffset, header.column_name.data(),
                header.column_name.size());
    write_u32(buffer, kRootOffset, header.root);
    write_u32(buffer, kFirstLeafOffset, header.first_leaf);
    write_u32(buffer, kLevelsOffset, header.levels);
    write_u64(buffer, kEntryCountOffset, header.entry_count);
    write_u32(buffer, kLastBlockOffset, header.last_block);
}

// Builds the header image from the members and writes it to block 0.
// `error` — receives a named message on failure.
// Returns true on success; false when block 0 cannot be pinned.
bool IsamIndex::write_header(std::string& error) {
    std::size_t frame = 0;
    if (!manager_.pin(file_, 0, frame, error)) {
        return false;
    }
    Header header;
    header.root = root_;
    header.first_leaf = first_leaf_;
    header.levels = levels_;
    header.last_block = last_block_;
    header.entry_count = entry_count_;
    header.table_name = schema_.table_name();
    header.column_name = schema_.column_info(column_index_).column_name();
    store_header(header, frame);
    release(manager_, frame, true);
    return true;
}

// Loads the header image from block 0; names longer than the on-disk
// maximum are left empty rather than trusted.
// `header` — receives the loaded fields; `error` — receives a named
// message on failure.
// Returns true on success; false when block 0 cannot be pinned.
bool IsamIndex::read_header(Header& header, std::string& error) {
    std::size_t frame = 0;
    if (!manager_.pin(file_, 0, frame, error)) {
        return false;
    }
    const char* buffer = manager_.frame(frame).buffer;
    header.magic = read_u32(buffer, kMagicOffset);
    header.key_type = read_u32(buffer, kKeyTypeOffset);
    header.key_width = read_u32(buffer, kKeyWidthOffset);
    header.root = read_u32(buffer, kRootOffset);
    header.first_leaf = read_u32(buffer, kFirstLeafOffset);
    header.levels = read_u32(buffer, kLevelsOffset);
    header.last_block = read_u32(buffer, kLastBlockOffset);
    header.entry_count = read_u64(buffer, kEntryCountOffset);
    const std::uint32_t name_length = read_u32(buffer, kTableNameLengthOffset);
    const std::uint32_t column_length =
        read_u32(buffer, kColumnNameLengthOffset);
    if (name_length <= kMaxNameLength) {
        header.table_name.assign(buffer + kTableNameOffset, name_length);
    }
    if (column_length <= kMaxNameLength) {
        header.column_name.assign(buffer + kColumnNameOffset, column_length);
    }
    release(manager_, frame, false);
    return true;
}

// --- lifecycle ----------------------------------------------------------------

// Creates a fresh, empty index file: refuses anything but a missing or
// zero-block path, formats the empty header state, allocates block 0,
// and leaves the index open.
// `path` — the index file to create; `error` — receives a named
// message on failure.
// Returns true with the index open; false with the file closed again.
bool IsamIndex::create(const std::string& path, std::string& error) {
    error.clear();
    if (open_) {
        error = "cannot create \"" + path + "\": this index is already open";
        return false;
    }
    if (!codec_.valid()) {
        error = "cannot create \"" + path +
                "\": the column has no usable key width (undeclared String)";
        return false;
    }
    // Fresh file only: an existing non-empty path is a refusal, an
    // existing empty path is allowed (open_for_append creates missing).
    bufman::File file;
    if (!manager_.open_file(path, file, error)) {
        return false;
    }
    const std::uint64_t blocks = bufman::block_count(file, error);
    if (!error.empty()) {
        std::string close_error;
        manager_.close_file(file, close_error);
        return false;
    }
    if (blocks > 0) {
        error = "cannot create \"" + path + "\": the file already holds " +
                std::to_string(blocks) + " block(s)";
        std::string close_error;
        manager_.close_file(file, close_error);
        return false;
    }

    file_ = file;
    path_ = path;
    root_ = kNoPage;
    first_leaf_ = kNoPage;
    levels_ = 1;
    entry_count_ = 0;

    std::size_t frame = 0;
    if (!manager_.alloc_page(file_, 0, frame, error)) {
        std::string close_error;
        manager_.close_file(file_, close_error);
        file_ = bufman::File{};
        return false;
    }
    last_block_ = 0;  // the header block exists from here on
    const bool wrote = write_header(error);
    release(manager_, frame, wrote);
    if (!wrote) {
        std::string close_error;
        manager_.close_file(file_, close_error);
        file_ = bufman::File{};
        last_block_ = kNoPage;
        return false;
    }
    open_ = true;
    return true;
}

// Encodes every (value, rid) pair once and sorts by the encoded bytes
// (D5). At exercise scale the sort fits comfortably in memory; an
// external run-merge is a later concern.
bool IsamIndex::encode_and_sort(
    const std::vector<std::pair<tuple::Column, Rid>>& entries,
    std::vector<DataEntry>& sorted, std::string& error) {
    sorted.clear();
    sorted.reserve(entries.size());
    for (const auto& [value, rid] : entries) {
        DataEntry entry;
        entry.key.resize(codec_.width());
        if (!codec_.encode(value, entry.key.data(), error)) {
            return false;
        }
        entry.rid = rid;
        sorted.push_back(std::move(entry));
    }
    std::sort(sorted.begin(), sorted.end(),
              [this](const DataEntry& a, const DataEntry& b) {
                  return codec_.compare(a.key.data(), b.key.data()) < 0;
              });
    return true;
}

// Fills leaves left to right from the sorted entries and returns each
// leaf's first key + block id for the levels above (D5). Leaves are
// linked afterwards by link_leaf_chain, so next_leaf stays empty here.
// With no entries the list is empty and the index stays empty (root
// and first leaf are kNoPage).
bool IsamIndex::build_leaves(const std::vector<DataEntry>& sorted,
                             std::vector<FirstKey>& level,
                             std::string& error) {
    level.clear();
    for (std::size_t start = 0; start < sorted.size();
         start += leaf_capacity(codec_.width())) {
        const std::size_t end =
            std::min(start + leaf_capacity(codec_.width()), sorted.size());
        std::uint32_t block = 0;
        std::size_t frame = 0;
        if (!alloc_next_block(block, frame, error)) {
            return false;
        }
        LeafPage leaf;
        leaf.bind(manager_.frame(frame).buffer, codec_.width());
        leaf.format(kNoPage, kNoPage);
        for (std::size_t i = start; i < end; ++i) {
            if (!leaf.insert_entry(sorted[i], codec_, error)) {
                release(manager_, frame, true);
                return false;  // unreachable: fresh pages never fill early
            }
        }
        release(manager_, frame, true);
        level.push_back(FirstKey{sorted[start].key, block});
    }
    if (!level.empty()) {
        first_leaf_ = level.front().block;
    }
    return true;
}

// Links the leaf chain forward, one leaf per frame (D5): leaf i's
// next_leaf is leaf i+1's block; the last leaf ends the chain.
bool IsamIndex::link_leaf_chain(const std::vector<FirstKey>& level,
                                std::size_t index, std::string& error) {
    if (index + 1 >= level.size()) {
        return true;  // the last leaf ends the chain
    }
    std::size_t frame = 0;
    LeafPage leaf;
    if (!pin_leaf(level[index].block, leaf, frame, error)) {
        return false;
    }
    leaf.set_next_leaf(level[index + 1].block);
    release(manager_, frame, true);
    return link_leaf_chain(level, index + 1, error);
}

// Builds one separator level from the pages below and recurses until a
// level fits in one page — that page is the root (D5). Separators are
// the first key of each child page except the leftmost; each separator
// page stores the first child of its chunk (written last: the slot
// moves with the entry count) plus one (key, child) entry per
// remaining child. levels_ grows once per level actually built.
bool IsamIndex::build_separator_level(std::vector<FirstKey> level,
                                      std::string& error) {
    if (level.size() <= 1) {
        // One page holds this level: it is the root (an empty index has
        // none, and a single leaf is its own root).
        root_ = level.empty() ? kNoPage : level.front().block;
        return true;
    }
    ++levels_;
    std::vector<FirstKey> next_level;
    for (std::size_t start = 0; start < level.size();
         start += separator_capacity(codec_.width())) {
        const std::size_t end =
            std::min(start + separator_capacity(codec_.width()),
                     level.size());
        std::uint32_t block = 0;
        std::size_t frame = 0;
        if (!alloc_next_block(block, frame, error)) {
            return false;
        }
        SeparatorPage page;
        page.bind(manager_.frame(frame).buffer, codec_.width());
        page.format();
        for (std::size_t i = start + 1; i < end; ++i) {
            // One index entry per child: the lecture's K/P pair.
            if (!page.append(IndexEntry{level[i].key, level[i].block},
                             error)) {
                release(manager_, frame, true);
                return false;
            }
        }
        page.set_first_child(level[start].block);
        release(manager_, frame, true);
        next_level.push_back(FirstKey{level[start].key, block});
    }
    return build_separator_level(std::move(next_level), error);
}

// The bulk build as four named phases (D5): create the file, encode
// and sort, fill the leaves, link the chain, grow separator levels —
// then the header. The close-on-failure dance lives here, once (D8).
// `path` — the index file to create; `entries` — the (value, rid)
// pairs, any order; `error` — receives a named message on failure.
// Returns true with the index open and entry_count_ set; false with
// the index closed again.
bool IsamIndex::build(const std::string& path,
                      const std::vector<std::pair<tuple::Column, Rid>>& entries,
                      std::string& error) {
    if (!create(path, error)) {
        return false;
    }

    // The whole build as four named phases (D5): encode and sort,
    // fill the leaves, link the chain, grow separator levels. The
    // close-on-failure dance lives here, once (D8).
    std::vector<DataEntry> sorted;
    std::vector<FirstKey> leaves;
    bool ok = encode_and_sort(entries, sorted, error) &&
              build_leaves(sorted, leaves, error) &&
              link_leaf_chain(leaves, 0, error) &&
              build_separator_level(std::move(leaves), error);
    if (ok) {
        entry_count_ = sorted.size();
        ok = write_header(error);
    }
    if (!ok) {
        std::string close_error;
        close(close_error);
        return false;
    }
    return true;
}

// Opens an existing index file: probes read-only (a missing path must
// be reported as such, not created), then validates the header's magic,
// key type and width, and table and column names against the bound
// schema before adopting the header state.
// `path` — the index file to open; `error` — receives a named message
// on failure.
// Returns true with the header state loaded; false with the file
// closed again.
bool IsamIndex::open(const std::string& path, std::string& error) {
    error.clear();
    if (open_) {
        error = "cannot open \"" + path + "\": this index is already open";
        return false;
    }
    if (!codec_.valid()) {
        error = "cannot open \"" + path +
                "\": the column has no usable key width (undeclared String)";
        return false;
    }
    // Probe read-only first: manager_.open_file would create a missing
    // file, and a missing path must be reported as such.
    bufman::File probe;
    if (!bufman::open_for_read(path, probe, error)) {
        error = "cannot open \"" + path + "\": " + error;
        return false;
    }
    bufman::close(probe);

    if (!manager_.open_file(path, file_, error)) {
        return false;
    }
    const std::uint64_t blocks = bufman::block_count(file_, error);
    if (error.empty() && blocks == 0) {
        error = "cannot open \"" + path + "\": the file has no header block";
    }

    Header header;
    if (error.empty()) {
        read_header(header, error);
    }
    if (error.empty()) {
        if (header.magic != kIndexMagic) {
            error = "cannot open \"" + path + "\": not an index file";
        } else if (header.key_type !=
                   static_cast<std::uint32_t>(codec_.key_type())) {
            error = "cannot open \"" + path + "\": key type differs";
        } else if (header.key_width != codec_.width()) {
            error = "cannot open \"" + path + "\": key width " +
                    std::to_string(header.key_width) + " differs from " +
                    std::to_string(codec_.width());
        } else if (header.table_name != schema_.table_name()) {
            error = "cannot open \"" + path + "\": index file is for table '" +
                    header.table_name + "'";
        } else if (header.column_name !=
                   schema_.column_info(column_index_).column_name()) {
            error = "cannot open \"" + path + "\": index file is for column '" +
                    header.column_name + "'";
        }
    }
    if (!error.empty()) {
        std::string close_error;
        manager_.close_file(file_, close_error);
        file_ = bufman::File{};
        return false;
    }
    root_ = header.root;
    first_leaf_ = header.first_leaf;
    levels_ = header.levels;
    last_block_ = header.last_block;
    entry_count_ = static_cast<std::size_t>(header.entry_count);
    path_ = path;
    open_ = true;
    return true;
}

// Flushes and closes this index's file, then resets every piece of
// in-memory state.
// `error` — receives a named message on failure.
// Returns true on success; false when the index is not open or the
// underlying close fails.
bool IsamIndex::close(std::string& error) {
    if (!open_) {
        error = "index is not open";
        return false;
    }
    manager_.close_file(file_, error);
    open_ = false;
    file_ = bufman::File{};
    root_ = kNoPage;
    first_leaf_ = kNoPage;
    levels_ = 1;
    last_block_ = kNoPage;
    entry_count_ = 0;
    return error.empty();
}

// --- pin helpers ----------------------------------------------------------------

// Pins `block` and binds a separator view to its frame.
// `block` — the block id; `page` — receives the bound view; `frame` —
// receives the pinned frame (the caller must unpin).
// Returns true on success; false with `error` set and the frame
// unpinned when the pin or the view's attach fails.
bool IsamIndex::pin_separator(std::uint32_t block, SeparatorPage& page,
                              std::size_t& frame, std::string& error) {
    if (!manager_.pin(file_, block, frame, error)) {
        return false;
    }
    page.bind(manager_.frame(frame).buffer, codec_.width());
    if (!page.attach(error)) {
        release(manager_, frame, false);
        return false;
    }
    return true;
}

// Pins `block` and binds a leaf view to its frame (same contract as
// pin_separator).
bool IsamIndex::pin_leaf(std::uint32_t block, LeafPage& page,
                         std::size_t& frame, std::string& error) {
    if (!manager_.pin(file_, block, frame, error)) {
        return false;
    }
    page.bind(manager_.frame(frame).buffer, codec_.width());
    if (!page.attach(error)) {
        release(manager_, frame, false);
        return false;
    }
    return true;
}

// Pins `block` and binds an overflow view to its frame (same contract
// as pin_separator).
bool IsamIndex::pin_overflow(std::uint32_t block, OverflowPage& page,
                             std::size_t& frame, std::string& error) {
    if (!manager_.pin(file_, block, frame, error)) {
        return false;
    }
    page.bind(manager_.frame(frame).buffer, codec_.width());
    if (!page.attach(error)) {
        release(manager_, frame, false);
        return false;
    }
    return true;
}

// --- descent ---------------------------------------------------------------------

// Descends from the root, crossing levels_ - 1 separator pages to land
// on a leaf (levels_ == 1 means the root IS a leaf).
// `key_bytes` — the encoded search key; `error` — receives a named
// message on failure.
// Returns the landing leaf's block id; kNoPage for an empty index or a
// failed separator read.
std::uint32_t IsamIndex::descend(const char* key_bytes,
                                 std::string& error) {
    if (root_ == kNoPage) {
        return kNoPage;  // empty index: no leaf exists
    }
    // levels_ counts the leaf level too: cross levels_ - 1 separator
    // pages, then land on a leaf. levels_ == 1 means the root IS a leaf.
    return descend_from(root_, levels_ - 1, key_bytes, error);
}

// The recursive descent (D2): pin one separator, take the child after
// the last separator strictly below the key, unpin, recurse. The base
// case is the leaf — the walk never holds a frame across the call.
std::uint32_t IsamIndex::descend_from(std::uint32_t block,
                                      std::uint32_t levels_remaining,
                                      const char* key_bytes,
                                      std::string& error) {
    if (levels_remaining == 0) {
        return block;  // the landing leaf
    }
    std::size_t frame = 0;
    SeparatorPage page;
    if (!pin_separator(block, page, frame, error)) {
        return kNoPage;
    }
    const std::uint32_t child = page.descend_step(key_bytes, codec_);
    release(manager_, frame, false);
    return descend_from(child, levels_remaining - 1, key_bytes, error);
}

// --- lookups ----------------------------------------------------------------------

// One overflow chain of a leaf: scan each page for equals, then
// recurse on the chain link (D3). Pin window: one page.
bool IsamIndex::scan_overflow_chain(std::uint32_t block,
                                    const std::string& key_bytes,
                                    std::vector<Rid>& out,
                                    std::string& error) {
    if (block == kNoPage) {
        return true;  // the chain has ended
    }
    std::size_t frame = 0;
    OverflowPage page;
    if (!pin_overflow(block, page, frame, error)) {
        return false;
    }
    collect_equals(page, codec_, key_bytes, out);
    const std::uint32_t next = page.next_overflow();
    release(manager_, frame, false);
    return scan_overflow_chain(next, key_bytes, out, error);
}

// The leaf chain of one lookup (D3): scan the landing leaf and its
// overflow chain, then continue into the next leaf while its first key
// is still <= the target — duplicates are contiguous but can span
// leaves, and the landing leaf may end below the key.
bool IsamIndex::scan_leaf_chain(std::uint32_t block,
                                const std::string& key_bytes,
                                std::vector<Rid>& out, std::string& error) {
    if (block == kNoPage) {
        return true;  // the chain has ended
    }
    std::size_t frame = 0;
    LeafPage leaf;
    if (!pin_leaf(block, leaf, frame, error)) {
        return false;
    }
    collect_equals(leaf, codec_, key_bytes, out);
    const std::uint32_t next_leaf = leaf.next_leaf();
    const std::uint32_t ovf = leaf.first_overflow();
    release(manager_, frame, false);
    if (!scan_overflow_chain(ovf, key_bytes, out, error)) {
        return false;
    }
    if (next_leaf == kNoPage) {
        return true;
    }
    // The step-guard: continue only while the next leaf still reaches
    // the key.
    std::size_t next_frame = 0;
    LeafPage next;
    if (!pin_leaf(next_leaf, next, next_frame, error)) {
        return false;
    }
    bool may_continue = false;
    if (next.count() > 0) {
        const DataEntry first = next.get_entry(0);
        may_continue =
            codec_.compare(first.key.data(), key_bytes.data()) <= 0;
    }
    release(manager_, next_frame, false);
    if (!may_continue) {
        return true;
    }
    return scan_leaf_chain(next_leaf, key_bytes, out, error);
}

// Equality lookup: encode the key, descend to the landing leaf, and
// walk the leaf chain collecting matches.
// `key` — the value to match; `out` — receives the rids (cleared
// first); `error` — receives a named message on failure.
// Returns true on success, including zero matches.
bool IsamIndex::lookup(const tuple::Column& key, std::vector<Rid>& out,
                       std::string& error) {
    out.clear();
    if (!open_) {
        error = "index is not open";
        return false;
    }
    std::string key_bytes(codec_.width(), '\0');
    if (!codec_.encode(key, key_bytes.data(), error)) {
        return false;
    }
    const std::uint32_t landing = descend(key_bytes.data(), error);
    if (!error.empty()) {
        return false;
    }
    return scan_leaf_chain(landing, key_bytes, out, error);
}

// One overflow chain as sorted runs: gather every entry of every page
// in chain order (D3). The caller stable-sorts the merge.
bool IsamIndex::gather_overflow_chain(std::uint32_t block,
                                      std::vector<DataEntry>& out,
                                      std::string& error) {
    if (block == kNoPage) {
        return true;  // the chain has ended
    }
    std::size_t frame = 0;
    OverflowPage page;
    if (!pin_overflow(block, page, frame, error)) {
        return false;
    }
    for (std::size_t i = 0; i < page.count(); ++i) {
        out.push_back(page.get_entry(i));
    }
    const std::uint32_t next = page.next_overflow();
    release(manager_, frame, false);
    return gather_overflow_chain(next, out, error);
}

// One leaf of a range scan (D3): the leaf's entries merged with its
// overflow runs, stable-sorted, filtered by the bounds, emitted in key
// order. Reports the leaf's chain successor through `next_leaf`.
bool IsamIndex::emit_leaf_range(std::uint32_t block, RangeWalk& walk,
                                std::uint32_t& next_leaf,
                                std::string& error) {
    std::size_t frame = 0;
    LeafPage leaf;
    if (!pin_leaf(block, leaf, frame, error)) {
        return false;
    }
    std::vector<DataEntry> merged;
    for (std::size_t i = 0; i < leaf.count(); ++i) {
        merged.push_back(leaf.get_entry(i));
    }
    next_leaf = leaf.next_leaf();
    const std::uint32_t ovf = leaf.first_overflow();
    release(manager_, frame, false);
    if (!gather_overflow_chain(ovf, merged, error)) {
        return false;
    }
    std::stable_sort(merged.begin(), merged.end(),
                     [this](const DataEntry& a, const DataEntry& b) {
                         return codec_.compare(a.key.data(),
                                               b.key.data()) < 0;
                     });
    for (const DataEntry& entry : merged) {
        if (walk.lo_bytes) {
            const int cmp =
                codec_.compare(entry.key.data(), walk.lo_bytes->data());
            if (cmp < 0 || (cmp == 0 && !walk.lo_inclusive)) {
                continue;
            }
        }
        if (walk.hi_bytes) {
            const int cmp =
                codec_.compare(entry.key.data(), walk.hi_bytes->data());
            if (cmp > 0 || (cmp == 0 && !walk.hi_inclusive)) {
                continue;
            }
        }
        walk.out->push_back(entry.rid);
    }
    return true;
}

// The leaf-chain walk of a range scan (D3): emit each leaf, then
// recurse while the chain continues and the upper bound admits the
// next leaf's first key.
bool IsamIndex::scan_leaf_range(std::uint32_t block, RangeWalk& walk,
                                std::string& error) {
    if (block == kNoPage) {
        return true;  // the chain has ended
    }
    std::uint32_t next_leaf = kNoPage;
    if (!emit_leaf_range(block, walk, next_leaf, error)) {
        return false;
    }
    if (next_leaf == kNoPage) {
        return true;
    }
    // The step-guard: stop when the next leaf starts beyond the upper
    // bound. Without a bound the walk runs to the end of the chain.
    if (walk.hi_bytes) {
        std::size_t next_frame = 0;
        LeafPage next;
        if (!pin_leaf(next_leaf, next, next_frame, error)) {
            return false;
        }
        bool beyond = true;
        if (next.count() > 0) {
            const DataEntry first = next.get_entry(0);
            const int cmp =
                codec_.compare(first.key.data(), walk.hi_bytes->data());
            beyond = cmp > 0 || (cmp == 0 && !walk.hi_inclusive);
        }
        release(manager_, next_frame, false);
        if (beyond) {
            return true;
        }
    }
    return scan_leaf_range(next_leaf, walk, error);
}

// Range scan: encode the optional bounds, start at the leaf after the
// last separator strictly below the lower bound (the first leaf for an
// open lower bound), and walk the chain under the bounds.
// `lo`, `hi` — the optional bounds with per-bound inclusivity.
// `out` — receives the matching rids (cleared first).
// `error` — receives a named message on failure.
// Returns true on success, including an empty result.
bool IsamIndex::range(const std::optional<RangeBound>& lo,
                      const std::optional<RangeBound>& hi,
                      std::vector<Rid>& out, std::string& error) {
    out.clear();
    if (!open_) {
        error = "index is not open";
        return false;
    }
    if (first_leaf_ == kNoPage) {
        return true;  // empty index: an empty range
    }

    std::string lo_bytes;
    if (lo) {
        lo_bytes.resize(codec_.width());
        if (!codec_.encode(lo->value, lo_bytes.data(), error)) {
            return false;
        }
    }
    std::string hi_bytes;
    if (hi) {
        hi_bytes.resize(codec_.width());
        if (!codec_.encode(hi->value, hi_bytes.data(), error)) {
            return false;
        }
    }

    RangeWalk walk;
    walk.lo_bytes = lo ? &lo_bytes : nullptr;
    walk.lo_inclusive = lo && lo->inclusive;
    walk.hi_bytes = hi ? &hi_bytes : nullptr;
    walk.hi_inclusive = hi && hi->inclusive;
    walk.out = &out;

    // Start: the leaf after the last separator strictly below the lower
    // bound (the leftmost leaf that may hold it), or the first leaf for
    // an open lower bound.
    std::uint32_t current = first_leaf_;
    if (lo) {
        current = descend(lo_bytes.data(), error);
        if (!error.empty()) {
            return false;
        }
        if (current == kNoPage) {
            return true;
        }
    }
    return scan_leaf_range(current, walk, error);
}

// --- insert -----------------------------------------------------------------------

// Allocates the next dense block id (last_block_ + 1, never the
// on-disk block count: dirty pages reach disk only on eviction/flush).
// `block` — receives the new id; `frame` — receives the pinned frame
// (the caller must unpin); `error` — receives a named message on
// failure.
// Returns true on success; false when the id range is exhausted or the
// allocation fails.
bool IsamIndex::alloc_next_block(std::uint32_t& block, std::size_t& frame,
                                 std::string& error) {
    // Pages are allocated densely and never removed, so the next id is
    // the highest allocated plus one — the on-disk block count cannot be
    // used: dirty pages reach disk only on eviction/flush.
    const std::uint32_t next = last_block_ + 1;
    if (next == kNoPage) {
        error = "index would need block id " + std::to_string(next) +
                ", beyond the 32-bit page id range";
        return false;
    }
    if (!manager_.alloc_page(file_, next, frame, error)) {
        return false;
    }
    last_block_ = next;
    block = next;
    return true;
}

// The first page of an overflow chain with room for one more entry, or
// kNoPage when every page is full (D6). Tail-recursive: pin, test,
// unpin, recurse on the chain link.
std::uint32_t IsamIndex::find_overflow_with_room(std::uint32_t block,
                                                 std::string& error) {
    if (block == kNoPage) {
        return kNoPage;  // no page in the chain has room
    }
    std::size_t frame = 0;
    OverflowPage page;
    if (!pin_overflow(block, page, frame, error)) {
        return kNoPage;
    }
    const bool has_room = page.count() < page.capacity();
    const std::uint32_t next = page.next_overflow();
    release(manager_, frame, false);
    if (has_room) {
        return block;
    }
    return find_overflow_with_room(next, error);
}

// Enters one (key, rid) pair: an empty index grows its first leaf
// (also the root); otherwise descend to the owning leaf and insert
// sorted in place, or spill into its overflow chain when full.
// `key` — the value to index; `rid` — the heap row address.
// `error` — receives a named message on failure.
// Returns true on success (entry_count_ grown, header rewritten).
bool IsamIndex::insert(const tuple::Column& key, Rid rid, std::string& error) {
    if (!open_) {
        error = "index is not open";
        return false;
    }
    std::string key_bytes(codec_.width(), '\0');
    if (!codec_.encode(key, key_bytes.data(), error)) {
        return false;
    }

    // An empty index grows its first leaf; it is simultaneously the root
    // (levels_ == 1). Existing leaves never split and separators stay
    // frozen. Insert into the owning leaf while it has room; otherwise
    // spill into its overflow chain.
    if (root_ == kNoPage) {
        std::size_t frame = 0;
        std::uint32_t block = 0;
        if (!alloc_next_block(block, frame, error)) {
            return false;
        }
        LeafPage leaf;
        leaf.bind(manager_.frame(frame).buffer, codec_.width());
        leaf.format(kNoPage, kNoPage);
        if (!leaf.insert_entry(DataEntry{key_bytes, rid}, codec_, error)) {
            release(manager_, frame, true);
            return false;
        }
        release(manager_, frame, true);
        root_ = block;
        first_leaf_ = block;
        levels_ = 1;
        entry_count_ = 1;
        return write_header(error);
    }

    const std::uint32_t leaf_block = descend(key_bytes.data(), error);
    if (!error.empty()) {
        return false;
    }
    if (leaf_block == kNoPage) {
        error = "index has no leaves to insert into";
        return false;
    }

    std::size_t frame = 0;
    LeafPage leaf;
    if (!pin_leaf(leaf_block, leaf, frame, error)) {
        return false;
    }
    if (leaf.count() < leaf.capacity()) {
        const bool inserted =
            leaf.insert_entry(DataEntry{key_bytes, rid}, codec_, error);
        release(manager_, frame, inserted);
        if (!inserted) {
            return false;
        }
        entry_count_ += 1;
        return write_header(error);
    }

    // Leaf full: spill into the leaf's overflow chain. Entries stay
    // sorted per page, so each leaf's answer remains a merge of
    // sorted runs.
    const std::uint32_t existing_chain = leaf.first_overflow();
    release(manager_, frame, false);
    return insert_into_overflow(leaf_block, existing_chain, key_bytes, rid,
                                error);
}

// The full-leaf spill (D6): insert into the first overflow page with
// room; when none has room, allocate a page, chain it at the FRONT of
// the leaf's chain, and re-point the leaf.
bool IsamIndex::insert_into_overflow(std::uint32_t leaf_block,
                                     std::uint32_t existing_chain,
                                     const std::string& key_bytes, Rid rid,
                                     std::string& error) {
    const std::uint32_t target =
        find_overflow_with_room(existing_chain, error);
    if (!error.empty()) {
        return false;
    }
    if (target != kNoPage) {
        // Room found in an existing overflow page: insert there.
        std::size_t target_frame = 0;
        OverflowPage page;
        if (!pin_overflow(target, page, target_frame, error)) {
            return false;
        }
        const bool inserted =
            page.insert_entry(DataEntry{key_bytes, rid}, codec_, error);
        release(manager_, target_frame, inserted);
        if (!inserted) {
            return false;
        }
        entry_count_ += 1;
        return write_header(error);
    }

    // No page has room: a new page takes the front of the chain.
    std::size_t new_frame = 0;
    std::uint32_t new_block = 0;
    if (!alloc_next_block(new_block, new_frame, error)) {
        return false;
    }
    OverflowPage page;
    page.bind(manager_.frame(new_frame).buffer, codec_.width());
    page.format(existing_chain);  // next = the old front
    if (!page.insert_entry(DataEntry{key_bytes, rid}, codec_, error)) {
        release(manager_, new_frame, true);
        return false;
    }
    release(manager_, new_frame, true);
    // Re-point the leaf at the new front page.
    std::size_t link_frame = 0;
    if (!manager_.pin(file_, leaf_block, link_frame, error)) {
        return false;
    }
    LeafPage link;
    link.bind(manager_.frame(link_frame).buffer, codec_.width());
    link.set_first_overflow(new_block);
    release(manager_, link_frame, true);
    entry_count_ += 1;
    return write_header(error);
}

// --- verify -----------------------------------------------------------------------

// One overflow chain of a leaf (D7): per-page sortedness and dangling
// rids via the shared check, then recurse on the chain link.
bool IsamIndex::verify_overflow_chain(std::uint32_t block, VerifyWalk& walk,
                                      std::string& error) {
    if (block == kNoPage) {
        return true;  // the chain has ended
    }
    std::size_t frame = 0;
    OverflowPage page;
    if (!pin_overflow(block, page, frame, error)) {
        return false;
    }
    walk.entry_count +=
        check_entries(page, codec_, "overflow " + std::to_string(block),
                      walk.occupied, walk.findings);
    const std::uint32_t next = page.next_overflow();
    release(manager_, frame, false);
    return verify_overflow_chain(next, walk, error);
}

// The leaf chain of a verify walk (D7): per-entry checks on the leaf,
// the chain-order finding against the previous leaf's last key, then
// the overflow chain and the next leaf. An empty leaf carries the
// previous key forward, exactly as the loop form did.
bool IsamIndex::verify_leaf_chain(std::uint32_t block,
                                  const std::string& previous_last_key,
                                  bool has_previous, VerifyWalk& walk,
                                  std::string& error) {
    if (block == kNoPage) {
        return true;  // the chain has ended
    }
    std::size_t frame = 0;
    LeafPage leaf;
    if (!pin_leaf(block, leaf, frame, error)) {
        return false;
    }
    walk.entry_count +=
        check_entries(leaf, codec_, "leaf " + std::to_string(block),
                      walk.occupied, walk.findings);
    if (has_previous) {
        // Chain order: no key may precede the previous leaf's last.
        for (std::size_t i = 0; i < leaf.count(); ++i) {
            const DataEntry entry = leaf.get_entry(i);
            if (codec_.compare(previous_last_key.data(), entry.key.data()) >
                0) {
                walk.findings.push_back(
                    "leaf chain out of key order at leaf " +
                    std::to_string(block) + ", key " +
                    codec_.display(entry.key.data()));
            }
        }
    }
    std::string last_key;
    bool has_last = false;
    if (leaf.count() > 0) {
        last_key = leaf.get_entry(leaf.count() - 1).key;
        has_last = true;
    }
    const std::uint32_t ovf = leaf.first_overflow();
    const std::uint32_t next_leaf = leaf.next_leaf();
    release(manager_, frame, false);
    if (!verify_overflow_chain(ovf, walk, error)) {
        return false;
    }
    return verify_leaf_chain(next_leaf,
                             has_last ? last_key : previous_last_key,
                             has_last || has_previous, walk, error);
}

// Full verification: walks the leaf chain and every overflow page,
// checks per-page sortedness, leaf-chain key order, and every rid
// against `occupied` (dangling entries named by key and row id), and
// compares the walked total against the header.
// `occupied` — callback judging whether an rid points at a live heap
// row; `entry_count` — receives the walked total; `findings` —
// receives one named string per problem (cleared first).
// `error` — receives a named message on failure.
// Returns true when the walk completed (findings may still be
// non-empty); false when the index is not open or a page read fails.
bool IsamIndex::verify(const std::function<bool(const Rid&)>& occupied,
                       std::size_t& entry_count,
                       std::vector<std::string>& findings,
                       std::string& error) {
    entry_count = 0;
    findings.clear();
    if (!open_) {
        error = "index is not open";
        return false;
    }
    // Order checks: entries sorted within each page, and the leaf
    // chain's first keys non-decreasing across leaves. Overflow keys
    // are deliberately NOT ordered against their leaf — an insert into
    // a full leaf lands in overflow regardless of value (6 can sit
    // behind a leaf ending in 9), so a cross-structure check would flag
    // legal data. Dangling rids are checked everywhere.
    VerifyWalk walk{occupied, findings, entry_count};
    if (!verify_leaf_chain(first_leaf_, "", false, walk, error)) {
        return false;
    }
    if (entry_count != entry_count_) {
        findings.push_back("walked " + std::to_string(entry_count) +
                           " entries, the header claims " +
                           std::to_string(entry_count_));
    }
    return true;
}

}
