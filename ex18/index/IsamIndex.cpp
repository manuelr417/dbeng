#include "index/IsamIndex.h"

#include "common/Error.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>

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

// Block 0, the header block, holds fixed-offset fields read and written
// as raw bytes — no page view exists for it, so the pin guard binds the
// frame's buffer alone.
class RawBlock {
public:
    void bind(char* buffer) { buffer_ = buffer; }

private:
    char* buffer_ = nullptr;
};

std::uint32_t read_u32(const char* buffer, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

void write_u32(char* buffer, std::size_t offset, std::uint32_t value) {
    std::memcpy(buffer + offset, &value, sizeof(value));
}

std::uint64_t read_u64(const char* buffer, std::size_t offset) {
    std::uint64_t value = 0;
    std::memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

void write_u64(char* buffer, std::size_t offset, std::uint64_t value) {
    std::memcpy(buffer + offset, &value, sizeof(value));
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
                findings.push_back(std::format(
                    "{}: entries out of order near key {}", label,
                    codec.display(entry.key.data())));
            }
        }
        if (!occupied(entry.rid)) {
            findings.push_back(std::format(
                "dangling entry: key {} rid ({},{})",
                codec.display(entry.key.data()), entry.rid.page_id,
                entry.rid.slot));
        }
        checked += 1;
    }
    return checked;
}

}  // namespace

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

void IsamIndex::store_header(const Header& header, char* buffer) {
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

void IsamIndex::write_header() {
    std::size_t frame = 0;
    manager_.pin(file_, 0, frame);
    common::PinnedPage<RawBlock> block(manager_, frame);
    Header header;
    header.root = root_;
    header.first_leaf = first_leaf_;
    header.levels = levels_;
    header.last_block = last_block_;
    header.entry_count = entry_count_;
    header.table_name = schema_.table_name();
    header.column_name = schema_.column_info(column_index_).column_name();
    store_header(header, block.buffer());
    block.mark_dirty();
}

void IsamIndex::read_header(Header& header) {
    std::size_t frame = 0;
    manager_.pin(file_, 0, frame);
    common::PinnedPage<RawBlock> block(manager_, frame);
    const char* buffer = block.buffer();
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
}

// --- lifecycle ----------------------------------------------------------------

void IsamIndex::create(const std::string& path) {
    if (open_) {
        throw common::Error(std::format(
            "cannot create \"{}\": this index is already open", path));
    }
    if (!codec_.valid()) {
        throw common::Error(std::format(
            "cannot create \"{}\": the column has no usable key width "
            "(undeclared String)",
            path));
    }
    // Fresh file only: an existing non-empty path is a refusal, an
    // existing empty path is allowed (open_for_append creates missing).
    bufman::File file = manager_.open_file(path);
    std::uint64_t blocks = 0;
    try {
        blocks = bufman::block_count(file);
    } catch (...) {
        manager_.close_file(file);
        throw;
    }
    if (blocks > 0) {
        manager_.close_file(file);
        throw common::Error(std::format(
            "cannot create \"{}\": the file already holds {} block(s)", path,
            blocks));
    }

    file_ = file;
    path_ = path;
    root_ = kNoPage;
    first_leaf_ = kNoPage;
    levels_ = 1;
    entry_count_ = 0;

    std::size_t frame = 0;
    try {
        manager_.alloc_page(file_, 0, frame);
    } catch (...) {
        manager_.close_file(file_);
        file_ = bufman::File{};
        throw;
    }
    last_block_ = 0;  // the header block exists from here on
    try {
        common::PinnedPage<RawBlock> header_block(manager_, frame);
        write_header();
    } catch (...) {
        manager_.close_file(file_);
        file_ = bufman::File{};
        last_block_ = kNoPage;
        throw;
    }
    open_ = true;
}

// Encodes every (value, rid) pair once and sorts by the encoded bytes
// (D5). At exercise scale the sort fits comfortably in memory; an
// external run-merge is a later concern.
void IsamIndex::encode_and_sort(
    const std::vector<std::pair<tuple::Column, Rid>>& entries,
    std::vector<DataEntry>& sorted) {
    sorted.clear();
    sorted.reserve(entries.size());
    for (const auto& [value, rid] : entries) {
        DataEntry entry;
        entry.key.resize(codec_.width());
        codec_.encode(value, entry.key.data());
        entry.rid = rid;
        sorted.push_back(std::move(entry));
    }
    std::sort(sorted.begin(), sorted.end(),
              [this](const DataEntry& a, const DataEntry& b) {
                  return codec_.compare(a.key.data(), b.key.data()) < 0;
              });
}

// Fills leaves left to right from the sorted entries and returns each
// leaf's first key + block id for the levels above (D5). Leaves are
// linked afterwards by link_leaf_chain, so next_leaf stays empty here.
// With no entries the list is empty and the index stays empty (root
// and first leaf are kNoPage).
void IsamIndex::build_leaves(const std::vector<DataEntry>& sorted,
                             std::vector<FirstKey>& level) {
    level.clear();
    for (std::size_t start = 0; start < sorted.size();
         start += leaf_capacity(codec_.width())) {
        const std::size_t end =
            std::min(start + leaf_capacity(codec_.width()), sorted.size());
        std::uint32_t block = 0;
        {
            std::size_t frame = 0;
            alloc_next_block(block, frame);
            common::PinnedPage<LeafPage> leaf(manager_, frame,
                                              codec_.width());
            leaf->format(kNoPage, kNoPage);
            for (std::size_t i = start; i < end; ++i) {
                // Unreachable throw: fresh pages never fill early.
                leaf->insert_entry(sorted[i], codec_);
            }
            leaf.mark_dirty();
        }
        level.push_back(FirstKey{sorted[start].key, block});
    }
    if (!level.empty()) {
        first_leaf_ = level.front().block;
    }
}

// Links the leaf chain forward, one leaf per frame (D5): leaf i's
// next_leaf is leaf i+1's block; the last leaf ends the chain.
void IsamIndex::link_leaf_chain(const std::vector<FirstKey>& level,
                                std::size_t index) {
    if (index + 1 >= level.size()) {
        return;  // the last leaf ends the chain
    }
    {
        common::PinnedPage<LeafPage> leaf = pin_leaf(level[index].block);
        leaf->set_next_leaf(level[index + 1].block);
        leaf.mark_dirty();
    }
    link_leaf_chain(level, index + 1);
}

// Builds one separator level from the pages below and recurses until a
// level fits in one page — that page is the root (D5). Separators are
// the first key of each child page except the leftmost; each separator
// page stores the first child of its chunk (written last: the slot
// moves with the entry count) plus one (key, child) entry per
// remaining child. levels_ grows once per level actually built.
void IsamIndex::build_separator_level(std::vector<FirstKey> level) {
    if (level.size() <= 1) {
        // One page holds this level: it is the root (an empty index has
        // none, and a single leaf is its own root).
        root_ = level.empty() ? kNoPage : level.front().block;
        return;
    }
    ++levels_;
    std::vector<FirstKey> next_level;
    for (std::size_t start = 0; start < level.size();
         start += separator_capacity(codec_.width())) {
        const std::size_t end =
            std::min(start + separator_capacity(codec_.width()),
                     level.size());
        std::uint32_t block = 0;
        {
            std::size_t frame = 0;
            alloc_next_block(block, frame);
            common::PinnedPage<SeparatorPage> page(manager_, frame,
                                                   codec_.width());
            page->format();
            for (std::size_t i = start + 1; i < end; ++i) {
                // One index entry per child: the lecture's K/P pair.
                // Unreachable throw: a fresh page never fills early.
                page->append(IndexEntry{level[i].key, level[i].block});
            }
            page->set_first_child(level[start].block);
            page.mark_dirty();
        }
        next_level.push_back(FirstKey{level[start].key, block});
    }
    build_separator_level(std::move(next_level));
}

void IsamIndex::build(
    const std::string& path,
    const std::vector<std::pair<tuple::Column, Rid>>& entries) {
    create(path);

    // The whole build as four named phases (D5): encode and sort,
    // fill the leaves, link the chain, grow separator levels. The
    // close-on-failure dance lives here, once (D8).
    std::vector<DataEntry> sorted;
    std::vector<FirstKey> leaves;
    try {
        encode_and_sort(entries, sorted);
        build_leaves(sorted, leaves);
        link_leaf_chain(leaves, 0);
        build_separator_level(std::move(leaves));
        entry_count_ = sorted.size();
        write_header();
    } catch (...) {
        try {
            close();
        } catch (...) {
            // Best effort: the build error is the one that propagates.
        }
        throw;
    }
}

void IsamIndex::open(const std::string& path) {
    if (open_) {
        throw common::Error(
            std::format("cannot open \"{}\": this index is already open",
                        path));
    }
    if (!codec_.valid()) {
        throw common::Error(std::format(
            "cannot open \"{}\": the column has no usable key width "
            "(undeclared String)",
            path));
    }
    // Probe read-only first: manager_.open_file would create a missing
    // file, and a missing path must be reported as such.
    bufman::File probe;
    try {
        probe = bufman::open_for_read(path);
    } catch (const common::Error& e) {
        throw common::Error(
            std::format("cannot open \"{}\": {}", path, e.what()));
    }
    bufman::close(probe);

    file_ = manager_.open_file(path);
    // Every failure from here through validation closes the file before
    // propagating: a refused open must not leave the file in the manager.
    Header header;
    try {
        const std::uint64_t blocks = bufman::block_count(file_);
        if (blocks == 0) {
            throw common::Error(std::format(
                "cannot open \"{}\": the file has no header block", path));
        }
        read_header(header);
        if (header.magic != kIndexMagic) {
            throw common::Error(
                std::format("cannot open \"{}\": not an index file", path));
        } else if (header.key_type !=
                   static_cast<std::uint32_t>(codec_.key_type())) {
            throw common::Error(
                std::format("cannot open \"{}\": key type differs", path));
        } else if (header.key_width != codec_.width()) {
            throw common::Error(std::format(
                "cannot open \"{}\": key width {} differs from {}", path,
                header.key_width, codec_.width()));
        } else if (header.table_name != schema_.table_name()) {
            throw common::Error(std::format(
                "cannot open \"{}\": index file is for table '{}'", path,
                header.table_name));
        } else if (header.column_name !=
                   schema_.column_info(column_index_).column_name()) {
            throw common::Error(std::format(
                "cannot open \"{}\": index file is for column '{}'", path,
                header.column_name));
        }
    } catch (...) {
        manager_.close_file(file_);
        file_ = bufman::File{};
        throw;
    }
    root_ = header.root;
    first_leaf_ = header.first_leaf;
    levels_ = header.levels;
    last_block_ = header.last_block;
    entry_count_ = static_cast<std::size_t>(header.entry_count);
    path_ = path;
    open_ = true;
}

void IsamIndex::close() {
    if (!open_) {
        throw common::Error("index is not open");
    }
    manager_.close_file(file_);
    open_ = false;
    file_ = bufman::File{};
    root_ = kNoPage;
    first_leaf_ = kNoPage;
    levels_ = 1;
    last_block_ = kNoPage;
    entry_count_ = 0;
}

// --- pin helpers ----------------------------------------------------------------

common::PinnedPage<SeparatorPage> IsamIndex::pin_separator(
    std::uint32_t block) {
    std::size_t frame = 0;
    manager_.pin(file_, block, frame);
    common::PinnedPage<SeparatorPage> page(manager_, frame, codec_.width());
    page->attach();
    return page;
}

common::PinnedPage<LeafPage> IsamIndex::pin_leaf(std::uint32_t block) {
    std::size_t frame = 0;
    manager_.pin(file_, block, frame);
    common::PinnedPage<LeafPage> page(manager_, frame, codec_.width());
    page->attach();
    return page;
}

common::PinnedPage<OverflowPage> IsamIndex::pin_overflow(
    std::uint32_t block) {
    std::size_t frame = 0;
    manager_.pin(file_, block, frame);
    common::PinnedPage<OverflowPage> page(manager_, frame, codec_.width());
    page->attach();
    return page;
}

// --- descent ---------------------------------------------------------------------

std::uint32_t IsamIndex::descend(const char* key_bytes) {
    if (root_ == kNoPage) {
        return kNoPage;  // empty index: no leaf exists
    }
    // levels_ counts the leaf level too: cross levels_ - 1 separator
    // pages, then land on a leaf. levels_ == 1 means the root IS a leaf.
    return descend_from(root_, levels_ - 1, key_bytes);
}

// The recursive descent (D2): pin one separator, take the child after
// the last separator strictly below the key, recurse. The base case is
// the leaf.
std::uint32_t IsamIndex::descend_from(std::uint32_t block,
                                      std::uint32_t levels_remaining,
                                      const char* key_bytes) {
    if (levels_remaining == 0) {
        return block;  // the landing leaf
    }
    std::uint32_t child = kNoPage;
    {
        common::PinnedPage<SeparatorPage> page = pin_separator(block);
        child = page->descend_step(key_bytes, codec_);
    }
    return descend_from(child, levels_remaining - 1, key_bytes);
}

// --- lookups ----------------------------------------------------------------------

// One overflow chain of a leaf: scan each page for equals, then
// recurse on the chain link (D3). Pin window: one page.
void IsamIndex::scan_overflow_chain(std::uint32_t block,
                                    const std::string& key_bytes,
                                    std::vector<Rid>& out) {
    if (block == kNoPage) {
        return;  // the chain has ended
    }
    std::uint32_t next = kNoPage;
    {
        common::PinnedPage<OverflowPage> page = pin_overflow(block);
        collect_equals(*page, codec_, key_bytes, out);
        next = page->next_overflow();
    }
    scan_overflow_chain(next, key_bytes, out);
}

// The leaf chain of one lookup (D3): scan the landing leaf and its
// overflow chain, then continue into the next leaf while its first key
// is still <= the target — duplicates are contiguous but can span
// leaves, and the landing leaf may end below the key.
void IsamIndex::scan_leaf_chain(std::uint32_t block,
                                const std::string& key_bytes,
                                std::vector<Rid>& out) {
    if (block == kNoPage) {
        return;  // the chain has ended
    }
    std::uint32_t next_leaf = kNoPage;
    std::uint32_t ovf = kNoPage;
    {
        common::PinnedPage<LeafPage> leaf = pin_leaf(block);
        collect_equals(*leaf, codec_, key_bytes, out);
        next_leaf = leaf->next_leaf();
        ovf = leaf->first_overflow();
    }
    scan_overflow_chain(ovf, key_bytes, out);
    if (next_leaf == kNoPage) {
        return;
    }
    // The step-guard: continue only while the next leaf still reaches
    // the key.
    bool may_continue = false;
    {
        common::PinnedPage<LeafPage> next = pin_leaf(next_leaf);
        if (next->count() > 0) {
            const DataEntry first = next->get_entry(0);
            may_continue =
                codec_.compare(first.key.data(), key_bytes.data()) <= 0;
        }
    }
    if (!may_continue) {
        return;
    }
    scan_leaf_chain(next_leaf, key_bytes, out);
}

void IsamIndex::lookup(const tuple::Column& key, std::vector<Rid>& out) {
    out.clear();
    if (!open_) {
        throw common::Error("index is not open");
    }
    std::string key_bytes(codec_.width(), '\0');
    codec_.encode(key, key_bytes.data());
    const std::uint32_t landing = descend(key_bytes.data());
    scan_leaf_chain(landing, key_bytes, out);
}

// One overflow chain as sorted runs: gather every entry of every page
// in chain order (D3). The caller stable-sorts the merge.
void IsamIndex::gather_overflow_chain(std::uint32_t block,
                                      std::vector<DataEntry>& out) {
    if (block == kNoPage) {
        return;  // the chain has ended
    }
    std::uint32_t next = kNoPage;
    {
        common::PinnedPage<OverflowPage> page = pin_overflow(block);
        for (std::size_t i = 0; i < page->count(); ++i) {
            out.push_back(page->get_entry(i));
        }
        next = page->next_overflow();
    }
    gather_overflow_chain(next, out);
}

// One leaf of a range scan (D3): the leaf's entries merged with its
// overflow runs, stable-sorted, filtered by the bounds, emitted in key
// order. Reports the leaf's chain successor through `next_leaf`.
void IsamIndex::emit_leaf_range(std::uint32_t block, RangeWalk& walk,
                                std::uint32_t& next_leaf) {
    std::vector<DataEntry> merged;
    std::uint32_t ovf = kNoPage;
    {
        common::PinnedPage<LeafPage> leaf = pin_leaf(block);
        for (std::size_t i = 0; i < leaf->count(); ++i) {
            merged.push_back(leaf->get_entry(i));
        }
        next_leaf = leaf->next_leaf();
        ovf = leaf->first_overflow();
    }
    gather_overflow_chain(ovf, merged);
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
}

// The leaf-chain walk of a range scan (D3): emit each leaf, then
// recurse while the chain continues and the upper bound admits the
// next leaf's first key.
void IsamIndex::scan_leaf_range(std::uint32_t block, RangeWalk& walk) {
    if (block == kNoPage) {
        return;  // the chain has ended
    }
    std::uint32_t next_leaf = kNoPage;
    emit_leaf_range(block, walk, next_leaf);
    if (next_leaf == kNoPage) {
        return;
    }
    // The step-guard: stop when the next leaf starts beyond the upper
    // bound. Without a bound the walk runs to the end of the chain.
    if (walk.hi_bytes) {
        bool beyond = true;
        {
            common::PinnedPage<LeafPage> next = pin_leaf(next_leaf);
            if (next->count() > 0) {
                const DataEntry first = next->get_entry(0);
                const int cmp =
                    codec_.compare(first.key.data(), walk.hi_bytes->data());
                beyond = cmp > 0 || (cmp == 0 && !walk.hi_inclusive);
            }
        }
        if (beyond) {
            return;
        }
    }
    scan_leaf_range(next_leaf, walk);
}

void IsamIndex::range(const std::optional<RangeBound>& lo,
                      const std::optional<RangeBound>& hi,
                      std::vector<Rid>& out) {
    out.clear();
    if (!open_) {
        throw common::Error("index is not open");
    }
    if (first_leaf_ == kNoPage) {
        return;  // empty index: an empty range
    }

    std::string lo_bytes;
    if (lo) {
        lo_bytes.resize(codec_.width());
        codec_.encode(lo->value, lo_bytes.data());
    }
    std::string hi_bytes;
    if (hi) {
        hi_bytes.resize(codec_.width());
        codec_.encode(hi->value, hi_bytes.data());
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
        current = descend(lo_bytes.data());
        if (current == kNoPage) {
            return;  // empty index: an empty range
        }
    }
    scan_leaf_range(current, walk);
}

// --- insert -----------------------------------------------------------------------

void IsamIndex::alloc_next_block(std::uint32_t& block, std::size_t& frame) {
    // Pages are allocated densely and never removed, so the next id is
    // the highest allocated plus one — the on-disk block count cannot be
    // used: dirty pages reach disk only on eviction/flush.
    const std::uint32_t next = last_block_ + 1;
    if (next == kNoPage) {
        throw common::Error(std::format(
            "index would need block id {}, beyond the 32-bit page id range",
            next));
    }
    manager_.alloc_page(file_, next, frame);
    last_block_ = next;
    block = next;
}

// The first page of an overflow chain with room for one more entry, or
// kNoPage when every page is full (D6). Tail-recursive: pin, test,
// recurse on the chain link.
std::uint32_t IsamIndex::find_overflow_with_room(std::uint32_t block) {
    if (block == kNoPage) {
        return kNoPage;  // no page in the chain has room
    }
    std::uint32_t next = kNoPage;
    {
        common::PinnedPage<OverflowPage> page = pin_overflow(block);
        if (page->count() < page->capacity()) {
            return block;
        }
        next = page->next_overflow();
    }
    return find_overflow_with_room(next);
}

void IsamIndex::insert(const tuple::Column& key, Rid rid) {
    if (!open_) {
        throw common::Error("index is not open");
    }
    std::string key_bytes(codec_.width(), '\0');
    codec_.encode(key, key_bytes.data());

    // An empty index grows its first leaf; it is simultaneously the root
    // (levels_ == 1). Leaves are otherwise frozen: the descent lands on
    // the owning leaf and the entry spills into its overflow chain.
    if (root_ == kNoPage) {
        std::uint32_t block = 0;
        {
            std::size_t frame = 0;
            alloc_next_block(block, frame);
            common::PinnedPage<LeafPage> leaf(manager_, frame,
                                              codec_.width());
            leaf->format(kNoPage, kNoPage);
            // Unreachable throw: a fresh page never fills early.
            leaf->insert_entry(DataEntry{key_bytes, rid}, codec_);
            leaf.mark_dirty();
        }
        root_ = block;
        first_leaf_ = block;
        levels_ = 1;
        entry_count_ = 1;
        write_header();
        return;
    }

    const std::uint32_t leaf_block = descend(key_bytes.data());
    if (leaf_block == kNoPage) {
        throw common::Error("index has no leaves to insert into");
    }

    bool inserted = false;
    std::uint32_t existing_chain = kNoPage;
    {
        common::PinnedPage<LeafPage> leaf = pin_leaf(leaf_block);
        if (leaf->count() < leaf->capacity()) {
            leaf->insert_entry(DataEntry{key_bytes, rid}, codec_);
            leaf.mark_dirty();
            inserted = true;
        } else {
            existing_chain = leaf->first_overflow();
        }
    }
    if (inserted) {
        entry_count_ += 1;
        write_header();
        return;
    }

    // Leaf full: spill into the leaf's overflow chain. Entries stay
    // sorted per page, so each leaf's answer remains a merge of
    // sorted runs.
    insert_into_overflow(leaf_block, existing_chain, key_bytes, rid);
}

// The full-leaf spill (D6): insert into the first overflow page with
// room; when none has room, allocate a page, chain it at the FRONT of
// the leaf's chain, and re-point the leaf.
void IsamIndex::insert_into_overflow(std::uint32_t leaf_block,
                                     std::uint32_t existing_chain,
                                     const std::string& key_bytes, Rid rid) {
    const std::uint32_t target = find_overflow_with_room(existing_chain);
    if (target != kNoPage) {
        // Room found in an existing overflow page: insert there.
        {
            common::PinnedPage<OverflowPage> page = pin_overflow(target);
            page->insert_entry(DataEntry{key_bytes, rid}, codec_);
            page.mark_dirty();
        }
        entry_count_ += 1;
        write_header();
        return;
    }

    // No page has room: a new page takes the front of the chain.
    std::uint32_t new_block = 0;
    {
        std::size_t new_frame = 0;
        alloc_next_block(new_block, new_frame);
        common::PinnedPage<OverflowPage> page(manager_, new_frame,
                                              codec_.width());
        page->format(existing_chain);  // next = the old front
        // Unreachable throw: a fresh page never fills early.
        page->insert_entry(DataEntry{key_bytes, rid}, codec_);
        page.mark_dirty();
    }
    // Re-point the leaf at the new front page.
    {
        common::PinnedPage<LeafPage> link = pin_leaf(leaf_block);
        link->set_first_overflow(new_block);
        link.mark_dirty();
    }
    entry_count_ += 1;
    write_header();
}

// --- erase ------------------------------------------------------------------

// One page's exact (key, rid) removal: both leaves and overflow pages
// are sorted EntryPages, so one helper serves both. False (a clean
// answer, not an error) when this page does not hold the pair.
bool IsamIndex::erase_from_page(EntryPage& page,
                                const std::string& key_bytes, Rid rid) {
    const std::size_t n = page.count();
    for (std::size_t i = page.lower_bound(key_bytes, codec_); i < n; ++i) {
        const DataEntry entry = page.get_entry(i);
        if (codec_.compare(entry.key.data(), key_bytes.data()) != 0) {
            break;  // sorted: the equal-key run has ended
        }
        if (entry.rid == rid) {
            page.remove_entry(i);
            return true;
        }
    }
    return false;
}

// One overflow chain (the landing leaf's spills): stop as soon as the
// pair is found and removed; the vacated space stays as a gap.
void IsamIndex::erase_from_overflow(std::uint32_t block,
                                    const std::string& key_bytes, Rid rid,
                                    bool& found) {
    if (found || block == kNoPage) {
        return;
    }
    std::uint32_t next = kNoPage;
    {
        common::PinnedPage<OverflowPage> page = pin_overflow(block);
        found = erase_from_page(*page, key_bytes, rid);
        next = page->next_overflow();
        if (found) {
            page.mark_dirty();
            return;
        }
    }
    erase_from_overflow(next, key_bytes, rid, found);
}

// The leaf-chain walk with the lookup's step-guard: erase reaches
// exactly the pages lookup would scan, so a removed pair is a pair no
// lookup would have answered.
void IsamIndex::erase_from_leaf(std::uint32_t block,
                                const std::string& key_bytes, Rid rid,
                                bool& found) {
    if (found || block == kNoPage) {
        return;
    }
    std::uint32_t ovf = kNoPage;
    std::uint32_t next_leaf = kNoPage;
    {
        common::PinnedPage<LeafPage> leaf = pin_leaf(block);
        found = erase_from_page(*leaf, key_bytes, rid);
        ovf = leaf->first_overflow();
        next_leaf = leaf->next_leaf();
        if (found) {
            leaf.mark_dirty();
            return;
        }
    }
    erase_from_overflow(ovf, key_bytes, rid, found);
    if (found || next_leaf == kNoPage) {
        return;
    }
    bool may_continue = false;
    {
        common::PinnedPage<LeafPage> next = pin_leaf(next_leaf);
        if (next->count() > 0) {
            const DataEntry first = next->get_entry(0);
            may_continue =
                codec_.compare(first.key.data(), key_bytes.data()) <= 0;
        }
    }
    erase_from_leaf(next_leaf, key_bytes, rid, found);
}

void IsamIndex::erase(const tuple::Column& key, Rid rid) {
    if (!open_) {
        throw common::Error("index is not open");
    }
    std::string key_bytes(codec_.width(), '\0');
    codec_.encode(key, key_bytes.data());
    if (root_ == kNoPage) {
        throw common::Error(std::format(
            "no entry for key {}", codec_.display(key_bytes.data())));
    }
    const std::uint32_t landing = descend(key_bytes.data());
    bool found = false;
    erase_from_leaf(landing, key_bytes, rid, found);
    if (!found) {
        throw common::Error(
            std::format("no entry for key {} rid ({},{})",
                        codec_.display(key_bytes.data()), rid.page_id,
                        rid.slot));
    }
    entry_count_ -= 1;
    write_header();
}

// --- verify -----------------------------------------------------------------------

// One overflow chain of a leaf (D7): per-page sortedness and dangling
// rids via the shared check, then recurse on the chain link.
void IsamIndex::verify_overflow_chain(std::uint32_t block, VerifyWalk& walk) {
    if (block == kNoPage) {
        return;  // the chain has ended
    }
    std::uint32_t next = kNoPage;
    {
        common::PinnedPage<OverflowPage> page = pin_overflow(block);
        walk.entry_count +=
            check_entries(*page, codec_,
                          std::format("overflow {}", block),
                          walk.occupied, walk.findings);
        next = page->next_overflow();
    }
    verify_overflow_chain(next, walk);
}

// The leaf chain of a verify walk (D7): per-entry checks on the leaf,
// the chain-order finding against the previous leaf's last key, then
// the overflow chain and the next leaf. An empty leaf carries the
// previous key forward, exactly as the loop form did.
void IsamIndex::verify_leaf_chain(std::uint32_t block,
                                  const std::string& previous_last_key,
                                  bool has_previous, VerifyWalk& walk) {
    if (block == kNoPage) {
        return;  // the chain has ended
    }
    std::string last_key;
    bool has_last = false;
    std::uint32_t ovf = kNoPage;
    std::uint32_t next_leaf = kNoPage;
    {
        common::PinnedPage<LeafPage> leaf = pin_leaf(block);
        walk.entry_count +=
            check_entries(*leaf, codec_, std::format("leaf {}", block),
                          walk.occupied, walk.findings);
        if (has_previous) {
            // Chain order: no key may precede the previous leaf's last.
            for (std::size_t i = 0; i < leaf->count(); ++i) {
                const DataEntry entry = leaf->get_entry(i);
                if (codec_.compare(previous_last_key.data(),
                                   entry.key.data()) > 0) {
                    walk.findings.push_back(std::format(
                        "leaf chain out of key order at leaf {}, key {}",
                        block, codec_.display(entry.key.data())));
                }
            }
        }
        if (leaf->count() > 0) {
            last_key = leaf->get_entry(leaf->count() - 1).key;
            has_last = true;
        }
        ovf = leaf->first_overflow();
        next_leaf = leaf->next_leaf();
    }
    verify_overflow_chain(ovf, walk);
    verify_leaf_chain(next_leaf,
                      has_last ? last_key : previous_last_key,
                      has_last || has_previous, walk);
}

void IsamIndex::verify(const std::function<bool(const Rid&)>& occupied,
                       std::size_t& entry_count,
                       std::vector<std::string>& findings) {
    entry_count = 0;
    findings.clear();
    if (!open_) {
        throw common::Error("index is not open");
    }
    // Order checks: entries sorted within each page, and the leaf
    // chain's first keys non-decreasing across leaves. Overflow keys
    // are deliberately NOT ordered against their leaf — an insert into
    // a full leaf lands in overflow regardless of value (6 can sit
    // behind a leaf ending in 9), so a cross-structure check would flag
    // legal data. Dangling rids are checked everywhere.
    VerifyWalk walk{occupied, findings, entry_count};
    verify_leaf_chain(first_leaf_, "", false, walk);
    if (entry_count != entry_count_) {
        findings.push_back(std::format(
            "walked {} entries, the header claims {}", entry_count,
            entry_count_));
    }
}

}
