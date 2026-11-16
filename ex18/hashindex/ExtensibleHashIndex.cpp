#include "hashindex/ExtensibleHashIndex.h"

#include "common/Error.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>

namespace hashindex {

namespace {

// The identification record at the fixed offsets of block 0, laid out
// like the ISAM header: magic, key type and width, the two names, then
// the hash-specific fields. Magic is "HASH" read little-endian.
constexpr std::uint32_t kHashMagic = 0x48534148u;  // "HASH"
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
constexpr std::size_t kGlobalDepthOffset =
    kColumnNameOffset + kMaxNameLength;             // 420
constexpr std::size_t kFirstDirectoryOffset = kGlobalDepthOffset + 4;
constexpr std::size_t kEntryCountOffset = kGlobalDepthOffset + 8;
constexpr std::size_t kLastBlockOffset = kGlobalDepthOffset + 16;

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

}

ExtensibleHashIndex::ExtensibleHashIndex(bufman::BufferManager& manager,
                                         const schema::TableSchema& schema,
                                         std::size_t column_index)
    : manager_(manager), schema_(schema), column_index_(column_index) {
    if (column_index < schema.column_count()) {
        const schema::ColumnSchema& column = schema.column_info(column_index);
        codec_ = isam::KeyCodec(column.column_type(),
                                column.column_type() == tuple::ColumnType::String
                                    ? column.column_size()
                                    : 0);
    }
}

// --- header -----------------------------------------------------------------

void ExtensibleHashIndex::store_header(const Header& header, char* buffer) {
    write_u32(buffer, kMagicOffset, kHashMagic);
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
    write_u32(buffer, kGlobalDepthOffset, header.global_depth);
    write_u32(buffer, kFirstDirectoryOffset, header.first_directory);
    write_u64(buffer, kEntryCountOffset, header.entry_count);
    write_u32(buffer, kLastBlockOffset, header.last_block);
}

void ExtensibleHashIndex::write_header() {
    std::size_t frame = 0;
    manager_.pin(file_, 0, frame);
    common::PinnedPage<RawBlock> block(manager_, frame);
    Header header;
    header.global_depth = global_depth_;
    header.first_directory = first_directory_;
    header.last_block = last_block_;
    header.entry_count = entry_count_;
    header.table_name = schema_.table_name();
    header.column_name = schema_.column_info(column_index_).column_name();
    store_header(header, block.buffer());
    block.mark_dirty();
}

void ExtensibleHashIndex::read_header(Header& header) {
    std::size_t frame = 0;
    manager_.pin(file_, 0, frame);
    common::PinnedPage<RawBlock> block(manager_, frame);
    const char* buffer = block.buffer();
    header.magic = read_u32(buffer, kMagicOffset);
    header.key_type = read_u32(buffer, kKeyTypeOffset);
    header.key_width = read_u32(buffer, kKeyWidthOffset);
    header.global_depth = read_u32(buffer, kGlobalDepthOffset);
    header.first_directory = read_u32(buffer, kFirstDirectoryOffset);
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
    (void)block;
}

// --- the directory chain ------------------------------------------------------

void ExtensibleHashIndex::load_directory() {
    directory_.clear();
    std::uint32_t block = first_directory_;
    while (block != kNoPage) {
        common::PinnedPage<DirectoryPage> page = pin_directory(block);
        const std::size_t stored = page->count();
        for (std::size_t i = 0; i < stored; ++i) {
            directory_.push_back(page->slot(i));
        }
        block = page->next_directory();
    }
    // The directory must hold exactly one slot per last-d-bit pattern.
    const std::size_t expected = std::size_t{1} << global_depth_;
    if (directory_.size() != expected) {
        throw common::Error(
            std::format("directory holds {} slots, global depth {} implies {}",
                        directory_.size(), global_depth_, expected));
    }
}

void ExtensibleHashIndex::store_directory() {
    // Walk the chain, overwriting; extend it when the directory grew
    // (a doubling). The directory never shrinks, but the walk still
    // truncates a longer chain rather than leaving a stale tail.
    std::size_t written = 0;
    std::uint32_t block = first_directory_;
    std::uint32_t previous = kNoPage;
    while (block != kNoPage || written < directory_.size()) {
        if (block == kNoPage) {
            // The chain ended early: allocate the next directory block.
            std::size_t frame = 0;
            alloc_next_block(block, frame);
            {
                common::PinnedPage<DirectoryPage> fresh(manager_, frame);
                fresh->format();
                fresh.mark_dirty();
            }
            if (previous == kNoPage) {
                first_directory_ = block;
            } else {
                std::size_t link_frame = 0;
                manager_.pin(file_, previous, link_frame);
                common::PinnedPage<DirectoryPage> link(manager_, link_frame);
                link->set_next_directory(block);
                link.mark_dirty();
            }
        }
        common::PinnedPage<DirectoryPage> page = pin_directory(block);
        const std::size_t here =
            std::min(directory_.size() - written, directory_capacity());
        for (std::size_t i = 0; i < here; ++i) {
            page->set_slot(i, directory_[written + i]);
        }
        page->set_next_directory(here < directory_.size()
                                     ? page->next_directory()
                                     : kNoPage);
        // The stored count is this page's slot fill, not the directory
        // total — pages hold at most directory_capacity() slots each.
        if (page->count() != here) {
            write_u32(page.buffer(), 0, static_cast<std::uint32_t>(here));
        }
        previous = block;
        block = page->next_directory();
        written += here;
        page.mark_dirty();
    }
}

// --- pin helpers ------------------------------------------------------------

// Each pins a block, binds the requested view, and attaches the page;
// the guard owns the pin from here to the caller's scope end.

common::PinnedPage<DirectoryPage>
ExtensibleHashIndex::pin_directory(std::uint32_t block) {
    std::size_t frame = 0;
    manager_.pin(file_, block, frame);
    common::PinnedPage<DirectoryPage> page(manager_, frame);
    page->attach();
    return page;
}

common::PinnedPage<BucketPage>
ExtensibleHashIndex::pin_bucket(std::uint32_t block) {
    std::size_t frame = 0;
    manager_.pin(file_, block, frame);
    common::PinnedPage<BucketPage> page(manager_, frame, codec_.width());
    page->attach();
    return page;
}

common::PinnedPage<OverflowPage>
ExtensibleHashIndex::pin_overflow(std::uint32_t block) {
    std::size_t frame = 0;
    manager_.pin(file_, block, frame);
    common::PinnedPage<OverflowPage> page(manager_, frame, codec_.width());
    page->attach();
    return page;
}

void ExtensibleHashIndex::alloc_next_block(std::uint32_t& block,
                                           std::size_t& frame) {
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

// --- lifecycle ----------------------------------------------------------------

void ExtensibleHashIndex::create(const std::string& path) {
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
    // existing empty path is allowed (the executor's lazy rebuild
    // replaces a deleted index file this way).
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
    global_depth_ = 1;
    first_directory_ = kNoPage;
    entry_count_ = 0;
    directory_.assign(std::size_t{1} << global_depth_, kNoPage);

    std::size_t header_frame = 0;
    try {
        manager_.alloc_page(file_, 0, header_frame);
    } catch (...) {
        manager_.close_file(file_);
        file_ = bufman::File{};
        throw;
    }
    last_block_ = 0;  // the header block exists from here on
    try {
        {
            std::size_t directory_frame = 0;
            alloc_next_block(first_directory_, directory_frame);
            common::PinnedPage<DirectoryPage> directory(manager_,
                                                        directory_frame);
            directory->format();
            directory.mark_dirty();
        }
        write_header();
        store_directory();
    } catch (...) {
        manager_.unpin(header_frame, false);
        manager_.close_file(file_);
        file_ = bufman::File{};
        last_block_ = kNoPage;
        throw;
    }
    manager_.unpin(header_frame, true);
    open_ = true;
}

void ExtensibleHashIndex::open(const std::string& path) {
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
        if (header.magic != kHashMagic) {
            throw common::Error(
                std::format("cannot open \"{}\": not a hash index file",
                            path));
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
    global_depth_ = header.global_depth;
    first_directory_ = header.first_directory;
    last_block_ = header.last_block;
    entry_count_ = static_cast<std::size_t>(header.entry_count);
    path_ = path;
    open_ = true;
    try {
        load_directory();
    } catch (...) {
        try {
            close();
        } catch (...) {
            // Best effort: the load error is the one that propagates.
        }
        throw;
    }
}

void ExtensibleHashIndex::close() {
    if (!open_) {
        throw common::Error("index is not open");
    }
    manager_.close_file(file_);
    open_ = false;
    file_ = bufman::File{};
    global_depth_ = 1;
    first_directory_ = kNoPage;
    last_block_ = kNoPage;
    entry_count_ = 0;
    directory_.clear();
}

// --- lookup ------------------------------------------------------------------

void ExtensibleHashIndex::build(
    const std::string& path,
    const std::vector<std::pair<tuple::Column, Rid>>& entries) {
    create(path);
    for (const auto& [value, rid] : entries) {
        try {
            insert(value, rid);
        } catch (...) {
            try {
                close();
            } catch (...) {
                // Best effort: the insert error is the one that propagates.
            }
            throw;
        }
    }
}

std::uint32_t ExtensibleHashIndex::slot_for(const char* key_bytes) const {
    return hash_key(codec_, key_bytes) & ((std::uint32_t{1} << global_depth_) - 1);
}

void ExtensibleHashIndex::scan_overflow_chain(std::uint32_t block,
                                              const std::string& key_bytes,
                                              std::vector<Rid>& out) {
    if (block == kNoPage) {
        return;  // the chain has ended
    }
    std::uint32_t next = kNoPage;
    {
        common::PinnedPage<OverflowPage> page = pin_overflow(block);
        for (std::size_t i = 0; i < page->count(); ++i) {
            const DataEntry entry = page->get_entry(i);
            if (entry.key == key_bytes) {
                out.push_back(entry.rid);
            }
        }
        next = page->next_overflow();
    }
    scan_overflow_chain(next, key_bytes, out);
}

void ExtensibleHashIndex::scan_bucket(std::uint32_t block,
                                      const std::string& key_bytes,
                                      std::vector<Rid>& out) {
    if (block == kNoPage) {
        return;  // no bucket grew here yet
    }
    std::uint32_t chain = kNoPage;
    {
        common::PinnedPage<BucketPage> bucket = pin_bucket(block);
        for (std::size_t i = 0; i < bucket->count(); ++i) {
            const DataEntry entry = bucket->get_entry(i);
            if (entry.key == key_bytes) {
                out.push_back(entry.rid);
            }
        }
        chain = bucket->first_overflow();
    }
    scan_overflow_chain(chain, key_bytes, out);
}

void ExtensibleHashIndex::lookup(const tuple::Column& key,
                                 std::vector<Rid>& out) {
    out.clear();
    if (!open_) {
        throw common::Error("index is not open");
    }
    std::string key_bytes(codec_.width(), '\0');
    codec_.encode(key, key_bytes.data());
    scan_bucket(directory_[slot_for(key_bytes.data())], key_bytes, out);
}

// --- insert --------------------------------------------------------------------

std::uint32_t ExtensibleHashIndex::find_overflow_with_room(
    std::uint32_t block) {
    if (block == kNoPage) {
        return kNoPage;  // no page in the chain has room
    }
    bool has_room = false;
    std::uint32_t next = kNoPage;
    {
        common::PinnedPage<OverflowPage> page = pin_overflow(block);
        has_room = page->count() < page->capacity();
        next = page->next_overflow();
    }
    if (has_room) {
        return block;
    }
    return find_overflow_with_room(next);
}

void ExtensibleHashIndex::extend_overflow(std::uint32_t bucket_block,
                                          std::uint32_t chain_head,
                                          const std::string& key_bytes,
                                          Rid rid) {
    const std::uint32_t target = find_overflow_with_room(chain_head);
    if (target != kNoPage) {
        // Room found in an existing overflow page: append there.
        {
            common::PinnedPage<OverflowPage> page = pin_overflow(target);
            page->append_entry(DataEntry{key_bytes, rid});
            page.mark_dirty();
        }
        entry_count_ += 1;
        write_header();
        return;
    }

    // No page has room: a new page takes the front of the chain.
    std::size_t new_frame = 0;
    std::uint32_t new_block = 0;
    alloc_next_block(new_block, new_frame);
    {
        common::PinnedPage<OverflowPage> page(manager_, new_frame,
                                              codec_.width());
        page->format(chain_head);  // next = the old front
        // Unreachable throw: a fresh page never fills early.
        page->append_entry(DataEntry{key_bytes, rid});
        page.mark_dirty();
    }
    // Re-point the bucket at the new front page.
    {
        std::size_t link_frame = 0;
        manager_.pin(file_, bucket_block, link_frame);
        common::PinnedPage<BucketPage> link(manager_, link_frame,
                                            codec_.width());
        link->set_first_overflow(new_block);
        link.mark_dirty();
    }
    entry_count_ += 1;
    write_header();
}

void ExtensibleHashIndex::split_bucket(std::uint32_t bucket_block,
                                       const std::string& key_bytes,
                                       Rid rid) {
    // Read the bucket's entries and the new entry, then decide the
    // split: the entries redistribute by bit `local depth` of their
    // hashes, and the split keeps only when both sides end up non-empty.
    std::uint32_t local = 0;
    std::vector<DataEntry> all;
    {
        common::PinnedPage<BucketPage> bucket = pin_bucket(bucket_block);
        local = bucket->local_depth();
        all.reserve(bucket->count() + 1);
        for (std::size_t i = 0; i < bucket->count(); ++i) {
            all.push_back(bucket->get_entry(i));
        }
    }
    all.push_back(DataEntry{key_bytes, rid});

    // The bucket's own low-bit pattern: every stored key agrees on the
    // low `local` bits, so any entry's hash carries it.
    const std::uint32_t pattern =
        hash_key(codec_, all[0].key.data()) & ((std::uint32_t{1} << local) - 1);
    std::size_t ones = 0;
    for (const DataEntry& entry : all) {
        ones += (hash_key(codec_, entry.key.data()) >> local) & 1u;
    }
    if (ones == 0 || ones == all.size()) {
        // A split that cannot separate: the duplicate-crowd case. The
        // bucket grows an overflow chain instead (and never splits
        // again).
        extend_overflow(bucket_block, kNoPage, key_bytes, rid);
        return;
    }

    // Splitting at the global depth needs more slots: double first.
    if (local == global_depth_) {
        if (global_depth_ == 31) {
            throw common::Error("insert would need directory slot 2^32, "
                                "beyond the 32-bit slot range");
        }
        const std::size_t old_size = directory_.size();
        directory_.resize(old_size * 2);
        for (std::size_t i = 0; i < old_size; ++i) {
            directory_[old_size + i] = directory_[i];
        }
        global_depth_ += 1;
    }

    // Allocate the "1" side's bucket; the old block keeps the "0" side.
    std::size_t new_frame = 0;
    std::uint32_t new_block = 0;
    alloc_next_block(new_block, new_frame);
    {
        common::PinnedPage<BucketPage> fresh(manager_, new_frame,
                                             codec_.width());
        fresh->format(local + 1, kNoPage);
        fresh.mark_dirty();
    }

    // Redistribute in arrival order: the old bucket is rewritten in
    // place, the new bucket appends the entries that moved. Both sides
    // are pinned at once, as the redistribution needs them together.
    {
        common::PinnedPage<BucketPage> zero_side = pin_bucket(bucket_block);
        common::PinnedPage<BucketPage> one_side = pin_bucket(new_block);
        std::size_t zero_count = 0;
        for (const DataEntry& entry : all) {
            if ((hash_key(codec_, entry.key.data()) >> local) & 1u) {
                // Unreachable throw: a fresh page holds one side.
                one_side->append_entry(entry);
            } else {
                // Rewrite in place: slot order equals arrival order filtered.
                const std::size_t at = zero_count;
                char* raw = zero_side.buffer() +
                            at * (codec_.width() + 8) + 12;
                std::memcpy(raw, entry.key.data(), codec_.width());
                std::memcpy(raw + codec_.width(), &entry.rid.page_id, 4);
                std::memcpy(raw + codec_.width() + 4, &entry.rid.slot, 4);
                zero_count += 1;
            }
        }
        write_u32(zero_side.buffer(), 0,
                  static_cast<std::uint32_t>(zero_count));
        write_u32(zero_side.buffer(), 4, local + 1);
        zero_side.mark_dirty();
        one_side.mark_dirty();
    }

    // Re-point every slot whose bit-`local` is 1 and whose low `local`
    // bits equal the pattern (the general form covers both the doubled
    // and the below-depth split).
    for (std::size_t s = 0; s < directory_.size(); ++s) {
        const std::uint32_t slot = static_cast<std::uint32_t>(s);
        if ((slot & ((std::uint32_t{1} << local) - 1)) == pattern &&
            (slot >> local) & 1u) {
            directory_[s] = new_block;
        }
    }
    store_directory();
    entry_count_ += 1;
    write_header();
}

void ExtensibleHashIndex::insert(const tuple::Column& key, Rid rid) {
    if (!open_) {
        throw common::Error("index is not open");
    }
    std::string key_bytes(codec_.width(), '\0');
    codec_.encode(key, key_bytes.data());
    const std::uint32_t slot = slot_for(key_bytes.data());
    if (directory_[slot] == kNoPage) {
        // The first bucket for this slot grows at the current global
        // depth (the slides' buckets carry local depth = file depth).
        std::size_t frame = 0;
        std::uint32_t block = 0;
        alloc_next_block(block, frame);
        {
            common::PinnedPage<BucketPage> bucket(manager_, frame,
                                                  codec_.width());
            bucket->format(global_depth_, kNoPage);
            // Unreachable throw: a fresh page never fills early.
            bucket->append_entry(DataEntry{key_bytes, rid});
            bucket.mark_dirty();
        }
        directory_[slot] = block;
        store_directory();
        entry_count_ += 1;
        write_header();
        return;
    }
    insert_into_bucket(directory_[slot], key_bytes, rid);
}

// The post-routing insert (D5's order of preference): a roomy bucket
// takes the entry, a bucket with an overflow chain spills into the
// chain, and a full chainless bucket splits.
void ExtensibleHashIndex::insert_into_bucket(std::uint32_t bucket_block,
                                             const std::string& key_bytes,
                                             Rid rid) {
    bool stored = false;
    std::uint32_t chain = kNoPage;
    {
        common::PinnedPage<BucketPage> bucket = pin_bucket(bucket_block);
        if (bucket->count() < bucket->capacity()) {
            bucket->append_entry(DataEntry{key_bytes, rid});
            bucket.mark_dirty();
            stored = true;
        } else {
            chain = bucket->first_overflow();
        }
    }
    if (stored) {
        entry_count_ += 1;
        write_header();
        return;
    }
    if (chain != kNoPage) {
        // Overflow is permanent: once a bucket owns a chain, the chain
        // absorbs everything further.
        extend_overflow(bucket_block, chain, key_bytes, rid);
        return;
    }
    split_bucket(bucket_block, key_bytes, rid);
}

// --- erase ---------------------------------------------------------------------

std::size_t ExtensibleHashIndex::find_exact(const HashEntryPage& page,
                                            const std::string& key_bytes,
                                            Rid rid) const {
    for (std::size_t i = 0; i < page.count(); ++i) {
        const DataEntry entry = page.get_entry(i);
        if (entry.key == key_bytes && entry.rid == rid) {
            return i;
        }
    }
    return page.count();
}

void ExtensibleHashIndex::erase_from_overflow(std::uint32_t block,
                                              const std::string& key_bytes,
                                              Rid rid, bool& found) {
    if (found || block == kNoPage) {
        return;
    }
    std::uint32_t next = kNoPage;
    {
        common::PinnedPage<OverflowPage> page = pin_overflow(block);
        const std::size_t at = find_exact(*page, key_bytes, rid);
        if (at < page->count()) {
            page->remove_entry(at);
            found = true;
            page.mark_dirty();
            return;
        }
        next = page->next_overflow();
    }
    erase_from_overflow(next, key_bytes, rid, found);
}

void ExtensibleHashIndex::erase_from_bucket(std::uint32_t block,
                                            const std::string& key_bytes,
                                            Rid rid, bool& found) {
    if (block == kNoPage) {
        return;  // no bucket grew here: nothing to erase
    }
    std::uint32_t chain = kNoPage;
    {
        common::PinnedPage<BucketPage> bucket = pin_bucket(block);
        const std::size_t at = find_exact(*bucket, key_bytes, rid);
        if (at < bucket->count()) {
            bucket->remove_entry(at);
            found = true;
            bucket.mark_dirty();
            return;
        }
        chain = bucket->first_overflow();
    }
    erase_from_overflow(chain, key_bytes, rid, found);
}

void ExtensibleHashIndex::erase(const tuple::Column& key, Rid rid) {
    if (!open_) {
        throw common::Error("index is not open");
    }
    std::string key_bytes(codec_.width(), '\0');
    codec_.encode(key, key_bytes.data());
    bool found = false;
    erase_from_bucket(directory_[slot_for(key_bytes.data())], key_bytes,
                      rid, found);
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

// The per-entry integrity checks shared by bucket and overflow pages
// (D11): rid occupancy and the bucket's low-bit pattern. `label` names
// the page in findings ("bucket 3", "overflow 5").
void check_hash_entries(const HashEntryPage& page, const isam::KeyCodec& codec,
                        const std::string& label,
                        const std::function<bool(const Rid&)>& occupied,
                        std::uint32_t mask, std::uint32_t expected,
                        std::size_t& walked,
                        std::vector<std::string>& findings) {
    for (std::size_t i = 0; i < page.count(); ++i) {
        const DataEntry entry = page.get_entry(i);
        if (!occupied(entry.rid)) {
            findings.push_back(std::format(
                "dangling entry: key {} rid ({},{})",
                codec.display(entry.key.data()), entry.rid.page_id,
                entry.rid.slot));
        }
        if ((hash_key(codec, entry.key.data()) & mask) != expected) {
            findings.push_back(std::format(
                "{}: misplaced entry: key {} rid ({},{})", label,
                codec.display(entry.key.data()), entry.rid.page_id,
                entry.rid.slot));
        }
        walked += 1;
    }
}

void ExtensibleHashIndex::verify_overflow_chain(
    std::uint32_t block, const std::function<bool(const Rid&)>& occupied,
    std::uint32_t mask, std::uint32_t expected, const std::string& label,
    std::size_t& walked, std::vector<std::string>& findings) {
    if (block == kNoPage) {
        return;
    }
    std::uint32_t next = kNoPage;
    {
        common::PinnedPage<OverflowPage> page = pin_overflow(block);
        check_hash_entries(*page, codec_, label, occupied, mask, expected,
                           walked, findings);
        next = page->next_overflow();
    }
    verify_overflow_chain(next, occupied, mask, expected, label,
                          walked, findings);
}

void ExtensibleHashIndex::verify(
    const std::function<bool(const Rid&)>& occupied, std::size_t& walked,
    std::vector<std::string>& findings) {
    findings.clear();
    walked = 0;
    if (!open_) {
        throw common::Error("index is not open");
    }
    // Distinct buckets, in block order (corresponding slots share one
    // bucket; each is verified once).
    std::vector<std::uint32_t> blocks = directory_;
    std::sort(blocks.begin(), blocks.end());
    blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
    for (const std::uint32_t block : blocks) {
        if (block == kNoPage) {
            continue;  // a slot no bucket grew into
        }
        const std::string label = std::format("bucket {}", block);
        std::uint32_t local = 0;
        std::uint32_t mask = 0;
        std::uint32_t expected = 0;
        std::uint32_t chain = kNoPage;
        {
            common::PinnedPage<BucketPage> bucket = pin_bucket(block);
            local = bucket->local_depth();
            if (local > global_depth_) {
                findings.push_back(std::format(
                    "{}: local depth {} exceeds the global depth {}", label,
                    local, global_depth_));
                continue;
            }
            mask = local == 0 ? 0u : (std::uint32_t{1} << local) - 1;
            // The bucket's expected pattern, from the slots that route to
            // it: they must agree on the low `local` bits.
            bool have_expected = false;
            for (std::size_t s = 0; s < directory_.size(); ++s) {
                if (directory_[s] != block) {
                    continue;
                }
                const std::uint32_t low =
                    static_cast<std::uint32_t>(s) & mask;
                if (!have_expected) {
                    expected = low;
                    have_expected = true;
                } else if (low != expected) {
                    findings.push_back(std::format(
                        "directory slot {} routes to {} at odds with its "
                        "pattern",
                        s, label));
                }
            }
            check_hash_entries(*bucket, codec_, label, occupied, mask,
                               expected, walked, findings);
            chain = bucket->first_overflow();
        }
        verify_overflow_chain(chain, occupied, mask, expected,
                              label, walked, findings);
    }
    if (walked != entry_count_) {
        findings.push_back(
            std::format("header counts {} entries, walk found {}",
                        entry_count_, walked));
    }
}

}
