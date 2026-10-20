#include "hashindex/StaticHashIndex.h"

#include <algorithm>
#include <cstring>

namespace hashindex {

namespace {

// The identification record at the fixed offsets of block 0, laid out
// like the extensible header: magic, key type and width, the two names,
// then the static-specific fields — the bucket count in place of the
// global depth. Magic is "SHSH" read little-endian, distinct from the
// extensible "HASH" so neither module opens the other's files.
constexpr std::uint32_t kStaticHashMagic = 0x48534853u;  // "SHSH"
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
constexpr std::size_t kBucketCountOffset =
    kColumnNameOffset + kMaxNameLength;             // 420
constexpr std::size_t kFirstDirectoryOffset = kBucketCountOffset + 4;
constexpr std::size_t kEntryCountOffset = kBucketCountOffset + 8;
constexpr std::size_t kLastBlockOffset = kBucketCountOffset + 16;

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

// Releases a pin without disturbing `error`, which on failure paths
// already carries the real cause.
void release(bufman::BufferManager& manager, std::size_t frame,
             bool was_dirty) {
    std::string unpin_error;
    manager.unpin(frame, was_dirty, unpin_error);
}

}

StaticHashIndex::StaticHashIndex(bufman::BufferManager& manager,
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

void StaticHashIndex::store_header(const Header& header,
                                   std::size_t frame) {
    char* buffer = manager_.frame(frame).buffer;  // the pinned header frame
    write_u32(buffer, kMagicOffset, kStaticHashMagic);
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
    write_u32(buffer, kBucketCountOffset, header.bucket_count);
    write_u32(buffer, kFirstDirectoryOffset, header.first_directory);
    write_u64(buffer, kEntryCountOffset, header.entry_count);
    write_u32(buffer, kLastBlockOffset, header.last_block);
}

bool StaticHashIndex::write_header(std::string& error) {
    std::size_t frame = 0;
    if (!manager_.pin(file_, 0, frame, error)) {
        return false;
    }
    Header header;
    header.bucket_count = bucket_count_;
    header.first_directory = first_directory_;
    header.last_block = last_block_;
    header.entry_count = entry_count_;
    header.table_name = schema_.table_name();
    header.column_name = schema_.column_info(column_index_).column_name();
    store_header(header, frame);
    release(manager_, frame, true);
    return true;
}

bool StaticHashIndex::read_header(Header& header, std::string& error) {
    std::size_t frame = 0;
    if (!manager_.pin(file_, 0, frame, error)) {
        return false;
    }
    const char* buffer = manager_.frame(frame).buffer;
    header.magic = read_u32(buffer, kMagicOffset);
    header.key_type = read_u32(buffer, kKeyTypeOffset);
    header.key_width = read_u32(buffer, kKeyWidthOffset);
    header.bucket_count = read_u32(buffer, kBucketCountOffset);
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
    release(manager_, frame, false);
    return true;
}

// --- the directory chain ------------------------------------------------------

bool StaticHashIndex::load_directory(std::string& error) {
    directory_.clear();
    std::uint32_t block = first_directory_;
    while (block != kNoPage) {
        std::size_t frame = 0;
        DirectoryPage page;
        if (!pin_directory(block, page, frame, error)) {
            return false;
        }
        const std::size_t stored = page.count();
        for (std::size_t i = 0; i < stored; ++i) {
            directory_.push_back(page.slot(i));
        }
        block = page.next_directory();
        release(manager_, frame, false);
    }
    // The array must hold exactly one slot per primary bucket.
    if (directory_.size() != bucket_count_) {
        error = "directory holds " + std::to_string(directory_.size()) +
                " slots, the header records " +
                std::to_string(bucket_count_) + " buckets";
        return false;
    }
    return true;
}

bool StaticHashIndex::store_directory(std::string& error) {
    // Walk the chain, overwriting; extend it when the array needs more
    // blocks. The array never shrinks, but the walk still truncates a
    // longer chain rather than leaving a stale tail.
    std::size_t written = 0;
    std::uint32_t block = first_directory_;
    std::uint32_t previous = kNoPage;
    while (block != kNoPage || written < directory_.size()) {
        if (block == kNoPage) {
            // The chain ended early: allocate the next directory block.
            std::size_t frame = 0;
            if (!alloc_next_block(block, frame, error)) {
                return false;
            }
            DirectoryPage fresh;
            fresh.bind(manager_.frame(frame).buffer);
            fresh.format();
            release(manager_, frame, true);
            if (previous == kNoPage) {
                first_directory_ = block;
            } else {
                std::size_t link_frame = 0;
                if (!manager_.pin(file_, previous, link_frame, error)) {
                    return false;
                }
                DirectoryPage link;
                link.bind(manager_.frame(link_frame).buffer);
                link.set_next_directory(block);
                release(manager_, link_frame, true);
            }
        }
        std::size_t frame = 0;
        DirectoryPage page;
        if (!pin_directory(block, page, frame, error)) {
            return false;
        }
        const std::size_t here =
            std::min(directory_.size() - written, directory_capacity());
        for (std::size_t i = 0; i < here; ++i) {
            page.set_slot(i, directory_[written + i]);
        }
        page.set_next_directory(here < directory_.size()
                                    ? page.next_directory()
                                    : kNoPage);
        // The stored count is this page's slot fill, not the array
        // total — pages hold at most directory_capacity() slots each.
        if (page.count() != here) {
            write_u32(manager_.frame(frame).buffer, 0,
                      static_cast<std::uint32_t>(here));
        }
        previous = block;
        block = page.next_directory();
        written += here;
        release(manager_, frame, true);
    }
    return true;
}

// --- pin helpers ------------------------------------------------------------

bool StaticHashIndex::pin_directory(std::uint32_t block,
                                    DirectoryPage& page,
                                    std::size_t& frame,
                                    std::string& error) {
    if (!manager_.pin(file_, block, frame, error)) {
        return false;
    }
    page.bind(manager_.frame(frame).buffer);
    if (!page.attach(error)) {
        release(manager_, frame, false);
        return false;
    }
    return true;
}

bool StaticHashIndex::pin_bucket(std::uint32_t block, StaticBucketPage& page,
                                 std::size_t& frame,
                                 std::string& error) {
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

bool StaticHashIndex::pin_overflow(std::uint32_t block,
                                   OverflowPage& page,
                                   std::size_t& frame,
                                   std::string& error) {
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

bool StaticHashIndex::alloc_next_block(std::uint32_t& block,
                                       std::size_t& frame,
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

// --- lifecycle ----------------------------------------------------------------

bool StaticHashIndex::create(const std::string& path,
                             std::uint32_t bucket_count,
                             std::string& error) {
    error.clear();
    if (open_) {
        error = "cannot create \"" + path + "\": this index is already open";
        return false;
    }
    if (bucket_count == 0) {
        error = "cannot create \"" + path +
                "\": the bucket count must be at least 1";
        return false;
    }
    if (!codec_.valid()) {
        error = "cannot create \"" + path +
                "\": the column has no usable key width (undeclared String)";
        return false;
    }
    // Fresh file only: an existing non-empty path is a refusal, an
    // existing empty path is allowed (a lazy rebuild replaces a deleted
    // index file this way).
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
    bucket_count_ = bucket_count;
    first_directory_ = kNoPage;
    entry_count_ = 0;
    // Every primary bucket exists from birth, empty: the array starts
    // filled with the bucket ids allocated just below.
    directory_.assign(bucket_count_, kNoPage);

    std::size_t header_frame = 0;
    if (!manager_.alloc_page(file_, 0, header_frame, error)) {
        std::string close_error;
        manager_.close_file(file_, close_error);
        file_ = bufman::File{};
        return false;
    }
    last_block_ = 0;  // the header block exists from here on
    {
        std::size_t directory_frame = 0;
        if (!alloc_next_block(first_directory_, directory_frame, error)) {
            release(manager_, header_frame, false);
            std::string close_error;
            manager_.close_file(file_, close_error);
            file_ = bufman::File{};
            last_block_ = kNoPage;
            return false;
        }
        DirectoryPage directory;
        directory.bind(manager_.frame(directory_frame).buffer);
        directory.format();
        release(manager_, directory_frame, true);
    }
    // The N primary buckets, each an empty page with no overflow link.
    for (std::uint32_t i = 0; i < bucket_count_; ++i) {
        std::size_t bucket_frame = 0;
        std::uint32_t bucket_block = 0;
        if (!alloc_next_block(bucket_block, bucket_frame, error)) {
            release(manager_, header_frame, false);
            std::string close_error;
            manager_.close_file(file_, close_error);
            file_ = bufman::File{};
            last_block_ = kNoPage;
            directory_.clear();
            return false;
        }
        StaticBucketPage bucket;
        bucket.bind(manager_.frame(bucket_frame).buffer, codec_.width());
        bucket.format(kNoPage);
        release(manager_, bucket_frame, true);
        directory_[i] = bucket_block;
    }

    const bool wrote = write_header(error) && store_directory(error);
    release(manager_, header_frame, wrote);
    if (!wrote) {
        std::string close_error;
        manager_.close_file(file_, close_error);
        file_ = bufman::File{};
        last_block_ = kNoPage;
        directory_.clear();
        return false;
    }
    open_ = true;
    return true;
}

bool StaticHashIndex::open(const std::string& path, std::string& error) {
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
        if (header.magic != kStaticHashMagic) {
            error = "cannot open \"" + path +
                    "\": not a static hash index file";
        } else if (header.bucket_count == 0) {
            error = "cannot open \"" + path +
                    "\": the header records 0 buckets";
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
    bucket_count_ = header.bucket_count;
    first_directory_ = header.first_directory;
    last_block_ = header.last_block;
    entry_count_ = static_cast<std::size_t>(header.entry_count);
    path_ = path;
    open_ = true;
    if (!load_directory(error)) {
        std::string close_error;
        close(close_error);
        return false;
    }
    return true;
}

bool StaticHashIndex::close(std::string& error) {
    if (!open_) {
        error = "index is not open";
        return false;
    }
    manager_.close_file(file_, error);
    open_ = false;
    file_ = bufman::File{};
    bucket_count_ = 0;
    first_directory_ = kNoPage;
    last_block_ = kNoPage;
    entry_count_ = 0;
    directory_.clear();
    return error.empty();
}

// --- lookup -------------------------------------------------------------------

bool StaticHashIndex::build(const std::string& path,
                            std::uint32_t bucket_count,
                            const std::vector<std::pair<tuple::Column, Rid>>& entries,
                            std::string& error) {
    if (!create(path, bucket_count, error)) {
        return false;
    }
    for (const auto& [value, rid] : entries) {
        if (!insert(value, rid, error)) {
            std::string close_error;
            close(close_error);
            return false;
        }
    }
    return true;
}

std::uint32_t StaticHashIndex::slot_for(const char* key_bytes) const {
    return hash_key(codec_, key_bytes) % bucket_count_;
}

bool StaticHashIndex::scan_overflow_chain(std::uint32_t block,
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
    for (std::size_t i = 0; i < page.count(); ++i) {
        const DataEntry entry = page.get_entry(i);
        if (entry.key == key_bytes) {
            out.push_back(entry.rid);
        }
    }
    const std::uint32_t next = page.next_overflow();
    release(manager_, frame, false);
    return scan_overflow_chain(next, key_bytes, out, error);
}

bool StaticHashIndex::scan_bucket(std::uint32_t block,
                                  const std::string& key_bytes,
                                  std::vector<Rid>& out,
                                  std::string& error) {
    if (block == kNoPage) {
        return true;  // unreachable in a static file: every bucket exists
    }
    std::size_t frame = 0;
    StaticBucketPage bucket;
    if (!pin_bucket(block, bucket, frame, error)) {
        return false;
    }
    for (std::size_t i = 0; i < bucket.count(); ++i) {
        const DataEntry entry = bucket.get_entry(i);
        if (entry.key == key_bytes) {
            out.push_back(entry.rid);
        }
    }
    const std::uint32_t chain = bucket.first_overflow();
    release(manager_, frame, false);
    return scan_overflow_chain(chain, key_bytes, out, error);
}

bool StaticHashIndex::lookup(const tuple::Column& key,
                             std::vector<Rid>& out, std::string& error) {
    error.clear();
    out.clear();
    if (!open_) {
        error = "index is not open";
        return false;
    }
    std::string key_bytes(codec_.width(), '\0');
    if (!codec_.encode(key, key_bytes.data(), error)) {
        return false;
    }
    return scan_bucket(directory_[slot_for(key_bytes.data())], key_bytes,
                       out, error);
}

// --- insert ---------------------------------------------------------------------

std::uint32_t StaticHashIndex::find_overflow_with_room(
    std::uint32_t block, std::string& error) {
    if (block == kNoPage) {
        return kNoPage;  // no page in the chain has room
    }
    std::size_t frame = 0;
    OverflowPage page;
    if (!pin_overflow(block, page, frame, error)) {
        return kNoPage;  // the caller sees the error and stops
    }
    const bool has_room = page.count() < page.capacity();
    const std::uint32_t next = page.next_overflow();
    release(manager_, frame, false);
    if (has_room) {
        return block;
    }
    return find_overflow_with_room(next, error);
}

bool StaticHashIndex::extend_overflow(std::uint32_t bucket_block,
                                      std::uint32_t chain_head,
                                      const std::string& key_bytes,
                                      Rid rid, std::string& error) {
    const std::uint32_t target =
        find_overflow_with_room(chain_head, error);
    if (!error.empty()) {
        return false;
    }
    if (target != kNoPage) {
        // Room found in an existing overflow page: append there.
        std::size_t frame = 0;
        OverflowPage page;
        if (!pin_overflow(target, page, frame, error)) {
            return false;
        }
        const bool appended =
            page.append_entry(DataEntry{key_bytes, rid}, error);
        release(manager_, frame, appended);
        if (!appended) {
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
    page.format(chain_head);  // next = the old front
    if (!page.append_entry(DataEntry{key_bytes, rid}, error)) {
        release(manager_, new_frame, true);
        return false;
    }
    release(manager_, new_frame, true);
    // Re-point the bucket at the new front page.
    std::size_t link_frame = 0;
    if (!manager_.pin(file_, bucket_block, link_frame, error)) {
        return false;
    }
    StaticBucketPage link;
    link.bind(manager_.frame(link_frame).buffer, codec_.width());
    link.set_first_overflow(new_block);
    release(manager_, link_frame, true);
    entry_count_ += 1;
    return write_header(error);
}

// The post-routing insert: a roomy bucket takes the entry, and a full
// bucket spills into its overflow chain — the lecture's static scheme
// has no split, no new primary bucket, no directory change.
bool StaticHashIndex::insert_into_bucket(std::uint32_t bucket_block,
                                         const std::string& key_bytes,
                                         Rid rid, std::string& error) {
    std::size_t frame = 0;
    StaticBucketPage bucket;
    if (!pin_bucket(bucket_block, bucket, frame, error)) {
        return false;
    }
    if (bucket.count() < bucket.capacity()) {
        const bool appended =
            bucket.append_entry(DataEntry{key_bytes, rid}, error);
        release(manager_, frame, appended);
        if (!appended) {
            return false;
        }
        entry_count_ += 1;
        return write_header(error);
    }
    const std::uint32_t chain = bucket.first_overflow();
    release(manager_, frame, false);
    return extend_overflow(bucket_block, chain, key_bytes, rid, error);
}

bool StaticHashIndex::insert(const tuple::Column& key, Rid rid,
                             std::string& error) {
    error.clear();
    if (!open_) {
        error = "index is not open";
        return false;
    }
    std::string key_bytes(codec_.width(), '\0');
    if (!codec_.encode(key, key_bytes.data(), error)) {
        return false;
    }
    return insert_into_bucket(directory_[slot_for(key_bytes.data())],
                              key_bytes, rid, error);
}

// --- erase ----------------------------------------------------------------------

std::size_t StaticHashIndex::find_exact(const HashEntryPage& page,
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

bool StaticHashIndex::erase_from_overflow(std::uint32_t block,
                                          const std::string& key_bytes,
                                          Rid rid, bool& found,
                                          std::string& error) {
    if (found || block == kNoPage) {
        return true;
    }
    std::size_t frame = 0;
    OverflowPage page;
    if (!pin_overflow(block, page, frame, error)) {
        return false;
    }
    const std::size_t at = find_exact(page, key_bytes, rid);
    if (at < page.count()) {
        page.remove_entry(at);
        found = true;
        release(manager_, frame, true);
        return true;
    }
    const std::uint32_t next = page.next_overflow();
    release(manager_, frame, false);
    return erase_from_overflow(next, key_bytes, rid, found, error);
}

bool StaticHashIndex::erase_from_bucket(std::uint32_t block,
                                        const std::string& key_bytes,
                                        Rid rid, bool& found,
                                        std::string& error) {
    if (block == kNoPage) {
        return true;  // unreachable in a static file: every bucket exists
    }
    std::size_t frame = 0;
    StaticBucketPage bucket;
    if (!pin_bucket(block, bucket, frame, error)) {
        return false;
    }
    const std::size_t at = find_exact(bucket, key_bytes, rid);
    if (at < bucket.count()) {
        bucket.remove_entry(at);
        found = true;
        release(manager_, frame, true);
        return true;
    }
    const std::uint32_t chain = bucket.first_overflow();
    release(manager_, frame, false);
    return erase_from_overflow(chain, key_bytes, rid, found, error);
}

bool StaticHashIndex::erase(const tuple::Column& key, Rid rid,
                            std::string& error) {
    error.clear();
    if (!open_) {
        error = "index is not open";
        return false;
    }
    std::string key_bytes(codec_.width(), '\0');
    if (!codec_.encode(key, key_bytes.data(), error)) {
        return false;
    }
    bool found = false;
    if (!erase_from_bucket(directory_[slot_for(key_bytes.data())], key_bytes,
                           rid, found, error)) {
        return false;
    }
    if (!found) {
        error = "no entry for key " + codec_.display(key_bytes.data()) +
                " rid (" + std::to_string(rid.page_id) + "," +
                std::to_string(rid.slot) + ")";
        return false;
    }
    entry_count_ -= 1;
    return write_header(error);
}

// --- verify -----------------------------------------------------------------------

// The per-entry integrity checks shared by bucket and overflow pages:
// rid occupancy and the static placement rule — the entry's hash must
// select the slot whose bucket chain holds it. `label` names the page
// in findings ("bucket 3", "overflow 5").
void check_static_entries(const HashEntryPage& page,
                          const isam::KeyCodec& codec,
                          std::uint32_t bucket_count,
                          std::uint32_t expected_slot,
                          const std::string& label,
                          const std::function<bool(const Rid&)>& occupied,
                          std::size_t& walked,
                          std::vector<std::string>& findings) {
    for (std::size_t i = 0; i < page.count(); ++i) {
        const DataEntry entry = page.get_entry(i);
        if (!occupied(entry.rid)) {
            findings.push_back("dangling entry: key " +
                               codec.display(entry.key.data()) + " rid (" +
                               std::to_string(entry.rid.page_id) + "," +
                               std::to_string(entry.rid.slot) + ")");
        }
        if (hash_key(codec, entry.key.data()) % bucket_count !=
            expected_slot) {
            findings.push_back(label + ": misplaced entry: key " +
                               codec.display(entry.key.data()) + " rid (" +
                               std::to_string(entry.rid.page_id) + "," +
                               std::to_string(entry.rid.slot) + ")");
        }
        walked += 1;
    }
}

bool StaticHashIndex::verify_overflow_chain(
    std::uint32_t block, const std::function<bool(const Rid&)>& occupied,
    std::uint32_t expected_slot, const std::string& label,
    std::size_t& walked, std::vector<std::string>& findings,
    std::string& error) {
    if (block == kNoPage) {
        return true;
    }
    std::size_t frame = 0;
    OverflowPage page;
    if (!pin_overflow(block, page, frame, error)) {
        return false;
    }
    check_static_entries(page, codec_, bucket_count_, expected_slot, label,
                         occupied, walked, findings);
    const std::uint32_t next = page.next_overflow();
    release(manager_, frame, false);
    return verify_overflow_chain(next, occupied, expected_slot, label,
                                 walked, findings, error);
}

bool StaticHashIndex::verify(
    const std::function<bool(const Rid&)>& occupied, std::size_t& walked,
    std::vector<std::string>& findings, std::string& error) {
    error.clear();
    findings.clear();
    walked = 0;
    if (!open_) {
        error = "index is not open";
        return false;
    }
    // One bucket per slot, every slot occupied: walk them in slot
    // order, each chain judged against its own slot number.
    for (std::size_t s = 0; s < directory_.size(); ++s) {
        const std::uint32_t block = directory_[s];
        if (block == kNoPage) {
            continue;  // unreachable in a static file, but a walked
                       // total below the header count would name it
        }
        std::size_t frame = 0;
        StaticBucketPage bucket;
        if (!pin_bucket(block, bucket, frame, error)) {
            return false;
        }
        const std::string label = "bucket " + std::to_string(block);
        const std::uint32_t expected = static_cast<std::uint32_t>(s);
        check_static_entries(bucket, codec_, bucket_count_, expected, label,
                             occupied, walked, findings);
        const std::uint32_t chain = bucket.first_overflow();
        release(manager_, frame, false);
        if (!verify_overflow_chain(chain, occupied, expected, label,
                                   walked, findings, error)) {
            return false;
        }
    }
    if (walked != entry_count_) {
        findings.push_back("header counts " +
                           std::to_string(entry_count_) +
                           " entries, walk found " + std::to_string(walked));
    }
    return true;
}




}

