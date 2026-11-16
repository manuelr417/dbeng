#include "heapfile/HeapFile.h"

#include "common/Error.h"

#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <format>

namespace heapfile {

namespace {

// Closes the file while a failure is already unwinding: close_file's own
// error must not mask the real cause (the old code gave the close its own
// error string for the same reason).
void close_ignoring_errors(bufman::BufferManager& manager, bufman::File& file) {
    try {
        manager.close_file(file);
    } catch (...) {
    }
}

// The identification record at offset 0 of the header page: magic, table
// name length, table name bytes (heap.md section 6). The header page's
// slots are never used, so its record area is free for this.
constexpr std::uint32_t kHeapMagic = 0x31505554u; // "TUP1"
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kNameLengthOffset = 4;
constexpr std::size_t kNameOffset = 8;
constexpr std::size_t kMaxTableNameLength = 200;

std::uint32_t read_u32(const char* buffer, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

void write_u32(char* buffer, std::size_t offset, std::uint32_t value) {
    std::memcpy(buffer + offset, &value, sizeof(value));
}

// Writes the identification record into a bound header-page buffer.
// Throws when the table name is too long.
void write_identification(char* buffer, const schema::TableSchema& schema) {
    const std::string& name = schema.table_name();
    if (name.size() > kMaxTableNameLength) {
        throw common::Error(std::format(
            "table name is {} bytes; at most {} fit the header page",
            name.size(), kMaxTableNameLength));
    }
    write_u32(buffer, kMagicOffset, kHeapMagic);
    write_u32(buffer, kNameLengthOffset,
              static_cast<std::uint32_t>(name.size()));
    std::memcpy(buffer + kNameOffset, name.data(), name.size());
}

// Checks the identification record against `schema`. Throws when the magic
// is missing or the name differs.
void check_identification(const char* buffer,
                          const schema::TableSchema& schema) {
    if (read_u32(buffer, kMagicOffset) != kHeapMagic) {
        throw common::Error("not a tuple heap file");
    }
    const std::uint32_t length = read_u32(buffer, kNameLengthOffset);
    if (length > kMaxTableNameLength) {
        throw common::Error("not a tuple heap file");
    }
    const std::string stored(buffer + kNameOffset, length);
    if (stored != schema.table_name()) {
        throw common::Error(std::format("heap file is for table '{}'", stored));
    }
}

// The width guard shared by create and open: the schema must yield a
// usable fixed-width record that fits one slot.
void check_record_layout(const std::string& path, const std::string& verb,
                         std::size_t size) {
    if (size == 0) {
        throw common::Error(std::format(
            "cannot {} \"{}\": the catalog has no record layout (no columns, "
            "or String columns without declared width)",
            verb, path));
    }
    if (size > bufman::kPageSize - kPageHeaderSize - 1) {
        throw common::Error(std::format(
            "cannot {} \"{}\": a {}-byte page fits records of at most {} "
            "bytes, the catalog needs {}",
            verb, path, bufman::kPageSize,
            bufman::kPageSize - kPageHeaderSize - 1, size));
    }
}

}

HeapFile::HeapFile(bufman::BufferManager& manager) : manager_(manager) {}

bool HeapFile::is_open() const {
    return open_;
}

void HeapFile::create(const std::string& path,
                      const schema::TableSchema& schema) {
    assert(!open_ && "create on an already-open heap file");
    const schema::TupleSerializer serializer(schema);
    check_record_layout(path, "create", serializer.record_size());
    // open_for_append creates the file when missing, so this also covers the
    // fresh-path case; an existing empty path is allowed.
    bufman::File file = manager_.open_file(path);
    try {
        const std::uint64_t blocks = bufman::block_count(file);
        if (blocks > 0) {
            throw common::Error(std::format(
                "cannot create \"{}\": the file already holds {} block(s)",
                path, blocks));
        }

        std::size_t frame = 0;
        manager_.alloc_page(file, 0, frame);
        common::PinnedPage<SlottedPage> header(manager_, frame,
                                               serializer.record_size());
        header->format(); // prev/next default to kNoPage: an empty chain
        write_identification(header.buffer(), schema);
        header.mark_dirty();
    } catch (...) {
        // An unusable file must not stay open.
        close_ignoring_errors(manager_, file);
        throw;
    }

    // Write the header back and leave a one-block file on disk.
    manager_.close_file(file);
}

void HeapFile::open(const std::string& path, const schema::TableSchema& schema) {
    assert(!open_ && "open on an already-open heap file");
    const schema::TupleSerializer serializer(schema);
    check_record_layout(path, "open", serializer.record_size());
    // Probe with a read-only open first: manager_.open_file would silently
    // create a missing file (O_CREAT), and a missing path must be reported
    // as such rather than as a validation failure.
    try {
        bufman::File probe = bufman::open_for_read(path);
        bufman::close(probe);
    } catch (const common::Error& e) {
        throw common::Error(
            std::format("cannot open \"{}\": {}", path, e.what()));
    }

    file_ = manager_.open_file(path);
    try {
        const std::uint64_t blocks = bufman::block_count(file_);
        if (blocks == 0) {
            throw common::Error(std::format(
                "cannot open \"{}\": the file has no header page", path));
        }

        std::size_t frame = 0;
        manager_.pin(file_, 0, frame);
        common::PinnedPage<SlottedPage> header(manager_, frame,
                                               serializer.record_size());
        // Identification first: the magic and name do not depend on the
        // record size, and a foreign file must be reported as such rather
        // than as a slot-count mismatch from attach.
        check_identification(header.buffer(), schema);
        header->attach();
    } catch (const common::Error& e) {
        // Unwind completely: an unusable file must not stay open. The close
        // must not mask the real cause above.
        close_ignoring_errors(manager_, file_);
        file_ = bufman::File{};
        throw common::Error(
            std::format("cannot open \"{}\": {}", path, e.what()));
    }
    schema_ = &schema;
    serializer_.emplace(schema);
    record_size_ = serializer.record_size();
    open_ = true;
}

HeapFileIterator HeapFile::scan() {
    return HeapFileIterator(*this, 0, 0);
}

RowId HeapFile::insert(const tuple::Tuple& tuple) {
    assert(is_open() && "the heap file must be open");
    // serialize validates the tuple against the schema first (arity,
    // types, string widths) and throws before any page is pinned.
    const std::string record = serializer_->serialize(tuple);

    // Walk the chain for the first page with a free slot.
    std::uint32_t tail = 0; // the header heads the chain
    for (std::uint32_t id = pin(0)->next_page(); id != kNoPage;) {
        common::PinnedPage<SlottedPage> page = pin(id);
        page->attach();
        if (auto slot = page->first_free_slot()) {
            page->put_record(*slot, record);
            page.mark_dirty();
            return RowId{id, static_cast<std::uint32_t>(*slot)};
        }
        tail = id;
        id = page->next_page();
    }
    return append_page(tail, record); // chain full: grow it
}

RowId HeapFile::append_page(std::uint32_t tail, const std::string& record) {
    // Pages are allocated strictly in increasing order and never removed,
    // so the first unused id is the old tail's id plus one — the header
    // itself plays tail for an empty chain. (The on-disk block count cannot
    // be used here: dirty pages reach disk only on eviction/flush, so it
    // lags behind the pool.)
    if (tail + 1 >= kNoPage) {
        throw common::Error(std::format(
            "insert would need page id {}, beyond the 32-bit row id range",
            tail + 1));
    }
    const std::uint32_t new_page_id = tail + 1;
    {
        std::size_t frame = 0;
        manager_.alloc_page(file_, new_page_id, frame);
        common::PinnedPage<SlottedPage> page(manager_, frame, record_size_);
        page->format(tail, kNoPage);
        page->put_record(0, record);
        page.mark_dirty();
    }
    {
        common::PinnedPage<SlottedPage> tail_page = pin(tail);
        tail_page->set_next_page(new_page_id);
        tail_page.mark_dirty();
    }
    return RowId{new_page_id, 0};
}

common::PinnedPage<SlottedPage> HeapFile::pin(std::uint32_t page_id) {
    std::size_t frame = 0;
    manager_.pin(file_, page_id, frame);
    return common::PinnedPage<SlottedPage>(manager_, frame, record_size_);
}

common::PinnedPage<SlottedPage> HeapFile::pin_record_page(RowId row_id) {
    if (row_id.page_id == 0) {
        throw common::Error(
            "page 0 is the header page; it never stores records");
    }
    const std::uint64_t blocks = bufman::block_count(file_);
    // A page exists iff it is resident in the pool or already written
    // back — see EXPLAIN.md, "Why the block count cannot decide" — so the
    // on-disk count alone would reject live pages.
    if (row_id.page_id >= blocks && !manager_.resident(file_, row_id.page_id)) {
        throw common::Error(std::format(
            "page {} is beyond the end of the file", row_id.page_id));
    }
    common::PinnedPage<SlottedPage> page = pin(row_id.page_id);
    page->attach();
    if (!page->is_occupied(row_id.slot)) {
        throw common::Error(std::format(
            "no record at row id (page {}, slot {}): the slot is free",
            row_id.page_id, row_id.slot));
    }
    return page;
}

void HeapFile::find(const RowId row_id, tuple::Tuple& tuple) {
    assert(is_open() && "the heap file must be open");
    common::PinnedPage<SlottedPage> page = pin_record_page(row_id);
    std::array<char, bufman::kPageSize> record{};
    page->get_record(row_id.slot, record.data(), record.size());
    serializer_->deserialize(record.data(), record.size(), tuple);
}

void HeapFile::update(const RowId row_id, const tuple::Tuple& tuple) {
    assert(is_open() && "the heap file must be open");
    const std::string record = serializer_->serialize(tuple);
    common::PinnedPage<SlottedPage> page = pin_record_page(row_id);
    page->put_record(row_id.slot, record);
    page.mark_dirty();
}

void HeapFile::erase(const RowId row_id) {
    assert(is_open() && "the heap file must be open");
    common::PinnedPage<SlottedPage> page = pin_record_page(row_id);
    page->erase(row_id.slot);
    page.mark_dirty();
}

void HeapFile::close() {
    assert(is_open() && "the heap file must be open");
    manager_.close_file(file_);
    open_ = false;
    schema_ = nullptr;
    serializer_.reset();
    record_size_ = 0;
    file_ = bufman::File{};
}

bool HeapFile::resident(std::uint32_t page_id) const {
    return manager_.resident(file_, page_id);
}

// --- HeapFileIterator -------------------------------------------------------

HeapFileIterator::HeapFileIterator(HeapFile& file, std::uint32_t page_id,
                                   std::uint32_t slot)
    : file_(&file), page_id_(page_id), slot_(slot) {}

bool HeapFileIterator::next(tuple::Tuple& tuple) {
    assert(file_->is_open() && "the heap file must be open");
    while (page_id_ != kNoPage) {
        common::PinnedPage<SlottedPage> page = file_->pin(page_id_);
        page->attach();
        const std::uint32_t next_page_id = page->next_page();
        for (std::uint32_t slot = slot_; slot < page->capacity(); ++slot) {
            if (!page->is_occupied(slot)) {
                continue;
            }
            std::array<char, bufman::kPageSize> record{};
            page->get_record(slot, record.data(), record.size());
            file_->serializer_->deserialize(record.data(), record.size(),
                                            tuple);
            current_ = RowId{page_id_, slot};
            if (slot + 1 < page->capacity()) {
                slot_ = slot + 1; // stay on this page
            } else {
                page_id_ = next_page_id;
                slot_ = 0;
            }
            return true;
        }
        // Page exhausted from slot_ on: continue with the next one.
        page_id_ = next_page_id;
        slot_ = 0;
    }
    return false; // exhausted; every later call reports exhaustion again
}

// --- Free functions ---------------------------------------------------------

void erase_file(bufman::BufferManager& manager, const std::string& path) {
    if (manager.is_open(path)) {
        throw common::Error(std::format("cannot erase \"{}\": the file is open",
                                        path));
    }
    if (std::remove(path.c_str()) != 0) {
        throw common::Error(std::format("cannot erase \"{}\": {}", path,
                                        std::strerror(errno)));
    }
}

}
