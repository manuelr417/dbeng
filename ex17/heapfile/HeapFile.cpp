#include "heapfile/HeapFile.h"

#include "common/Error.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>

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
        throw common::Error("table name is " + std::to_string(name.size()) +
                            " bytes; at most " + std::to_string(kMaxTableNameLength) +
                            " fit the header page");
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
        throw common::Error("heap file is for table '" + stored + "'");
    }
}

}

HeapFile::HeapFile(bufman::BufferManager& manager) : manager_(manager) {}

bool HeapFile::is_open() const {
    return open_;
}

void HeapFile::require_open() const {
    if (!open_) {
        throw common::Error("heap file is not open");
    }
}

void HeapFile::create(const std::string& path,
                      const schema::TableSchema& schema) {
    if (open_) {
        throw common::Error("cannot create \"" + path + "\": this heap file is already open");
    }
    const schema::TupleSerializer serializer(schema);
    const std::size_t size = serializer.record_size();
    if (size == 0) {
        throw common::Error("cannot create \"" + path +
                            "\": the catalog has no record layout (no columns, or "
                            "String columns without declared width)");
    }
    if (size > bufman::kPageSize - kPageHeaderSize - 1) {
        throw common::Error("cannot create \"" + path + "\": a " +
                            std::to_string(bufman::kPageSize) +
                            "-byte page fits records of at most " +
                            std::to_string(bufman::kPageSize - kPageHeaderSize - 1) +
                            " bytes, the catalog needs " + std::to_string(size));
    }
    // open_for_append creates the file when missing, so this also covers the
    // fresh-path case; an existing empty path is allowed.
    bufman::File file = manager_.open_file(path);
    try {
        const std::uint64_t blocks = bufman::block_count(file);
        if (blocks > 0) {
            throw common::Error("cannot create \"" + path + "\": the file already holds " +
                                std::to_string(blocks) + " block(s)");
        }

        std::size_t frame = 0;
        manager_.alloc_page(file, 0, frame);
        SlottedPage header;
        header.bind(manager_.frame(frame).buffer, size);
        header.format(); // prev/next default to kNoPage: an empty chain
        try {
            write_identification(manager_.frame(frame).buffer, schema);
        } catch (...) {
            manager_.unpin(frame, false);
            throw;
        }
        manager_.unpin(frame, true);
    } catch (...) {
        // An unusable file must not stay open.
        close_ignoring_errors(manager_, file);
        throw;
    }

    // Write the header back and leave a one-block file on disk.
    manager_.close_file(file);
}

void HeapFile::open(const std::string& path, const schema::TableSchema& schema) {
    if (open_) {
        throw common::Error("cannot open \"" + path + "\": this heap file is already open");
    }
    const schema::TupleSerializer serializer(schema);
    const std::size_t size = serializer.record_size();
    if (size == 0) {
        throw common::Error("cannot open \"" + path +
                            "\": the catalog has no record layout (no columns, or "
                            "String columns without declared width)");
    }
    if (size > bufman::kPageSize - kPageHeaderSize - 1) {
        throw common::Error("cannot open \"" + path + "\": a " +
                            std::to_string(bufman::kPageSize) +
                            "-byte page fits records of at most " +
                            std::to_string(bufman::kPageSize - kPageHeaderSize - 1) +
                            " bytes, the catalog needs " + std::to_string(size));
    }
    // Probe with a read-only open first: manager_.open_file would silently
    // create a missing file (O_CREAT), and a missing path must be reported
    // as such rather than as a validation failure.
    try {
        bufman::File probe = bufman::open_for_read(path);
        bufman::close(probe);
    } catch (const common::Error& e) {
        throw common::Error("cannot open \"" + path + "\": " + e.what());
    }

    file_ = manager_.open_file(path);
    try {
        const std::uint64_t blocks = bufman::block_count(file_);
        if (blocks == 0) {
            throw common::Error("cannot open \"" + path + "\": the file has no header page");
        }

        std::size_t frame = 0;
        manager_.pin(file_, 0, frame);
        SlottedPage header;
        header.bind(manager_.frame(frame).buffer, size);
        try {
            // Identification first: the magic and name do not depend on the
            // record size, and a foreign file must be reported as such rather
            // than as a slot-count mismatch from attach.
            check_identification(manager_.frame(frame).buffer, schema);
            header.attach();
        } catch (const common::Error& e) {
            manager_.unpin(frame, false);
            throw common::Error("cannot open \"" + path + "\": " + e.what());
        }
        manager_.unpin(frame, false);
    } catch (...) {
        // Unwind completely: an unusable file must not stay open. The close
        // must not mask the real cause above.
        close_ignoring_errors(manager_, file_);
        file_ = bufman::File{};
        throw;
    }
    schema_ = &schema;
    serializer_.emplace(schema);
    record_size_ = size;
    open_ = true;
}

HeapFileIterator HeapFile::scan() {
    return HeapFileIterator(*this, 0, 0);
}

void HeapFile::insert(const tuple::Tuple& tuple, RowId& row_id) {
    require_open();
    // serialize validates the tuple against the schema first (arity,
    // types, string widths) and throws before any page is pinned.
    std::array<char, bufman::kPageSize> record{};
    serializer_->serialize(tuple, record.data(), record.size());

    // 1. The header is the chain head.
    std::size_t frame = 0;
    manager_.pin(file_, 0, frame);
    SlottedPage page;
    page.bind(manager_.frame(frame).buffer, record_size_);
    try {
        page.attach();
    } catch (...) {
        manager_.unpin(frame, false);
        throw;
    }
    std::uint32_t current = page.next_page();
    std::uint32_t previous = 0;
    manager_.unpin(frame, false);

    // 2. Walk the chain for the first page with a free slot.
    while (current != kNoPage) {
        manager_.pin(file_, current, frame);
        page.bind(manager_.frame(frame).buffer, record_size_);
        try {
            page.attach();
            const std::optional<std::size_t> slot = page.first_free_slot();
            if (slot) {
                page.put_record(*slot, record.data(), record_size_);
                row_id = RowId{current, static_cast<std::uint32_t>(*slot)};
                manager_.unpin(frame, true);
                return;
            }
            previous = current;
            current = page.next_page();
        } catch (...) {
            manager_.unpin(frame, false);
            throw;
        }
        manager_.unpin(frame, false);
    }

    // 3. Chain exhausted — allocate and splice a new tail. Pages are
    // allocated strictly in increasing order and never removed, so the
    // first unused id is the old tail's id plus one — the header
    // itself plays tail for an empty chain. (The on-disk block count cannot
    // be used here: dirty pages reach disk only on eviction/flush, so it
    // lags behind the pool.)
    if (previous + 1 >= kNoPage) {
        throw common::Error("insert would need page id " + std::to_string(previous + 1) +
                            ", beyond the 32-bit row id range");
    }
    const std::uint32_t new_page_id = previous + 1;
    manager_.alloc_page(file_, new_page_id, frame);
    page.bind(manager_.frame(frame).buffer, record_size_);
    page.format(previous, kNoPage);
    try {
        page.put_record(0, record.data(), record_size_);
    } catch (...) {
        manager_.unpin(frame, true);
        throw;
    }
    manager_.unpin(frame, true);

    manager_.pin(file_, previous, frame);
    page.bind(manager_.frame(frame).buffer, record_size_);
    try {
        page.attach();
        page.set_next_page(new_page_id);
    } catch (...) {
        manager_.unpin(frame, false);
        throw;
    }
    manager_.unpin(frame, true);

    row_id = RowId{new_page_id, 0};
}

void HeapFile::pin_record_page(const RowId row_id, SlottedPage& page,
                               std::size_t& frame) {
    if (row_id.page_id == 0) {
        throw common::Error("page 0 is the header page; it never stores records");
    }
    const std::uint64_t blocks = bufman::block_count(file_);
    // A page exists iff it is resident in the pool or already written back:
    // dirty pages reach disk only on eviction/flush, so the on-disk block
    // count alone would lag behind the pool and reject live pages.
    if (row_id.page_id >= blocks && !manager_.resident(file_, row_id.page_id)) {
        throw common::Error("page " + std::to_string(row_id.page_id) +
                            " is beyond the end of the file");
    }
    manager_.pin(file_, row_id.page_id, frame);
    page.bind(manager_.frame(frame).buffer, record_size_);
    try {
        page.attach();
        if (!page.is_occupied(row_id.slot)) {
            throw common::Error("no record at row id (page " +
                                std::to_string(row_id.page_id) +
                                ", slot " + std::to_string(row_id.slot) +
                                "): the slot is free");
        }
    } catch (...) {
        manager_.unpin(frame, false);
        throw;
    }
}

void HeapFile::find(const RowId row_id, tuple::Tuple& tuple) {
    require_open();
    std::size_t frame = 0;
    SlottedPage page;
    pin_record_page(row_id, page, frame);
    std::array<char, bufman::kPageSize> record{};
    try {
        page.get_record(row_id.slot, record.data(), record.size());
    } catch (...) {
        manager_.unpin(frame, false);
        throw;
    }
    manager_.unpin(frame, false);
    serializer_->deserialize(record.data(), record.size(), tuple);
}

void HeapFile::update(const RowId row_id, const tuple::Tuple& tuple) {
    require_open();
    std::array<char, bufman::kPageSize> record{};
    serializer_->serialize(tuple, record.data(), record.size());
    std::size_t frame = 0;
    SlottedPage page;
    pin_record_page(row_id, page, frame);
    try {
        page.put_record(row_id.slot, record.data(), record_size_);
    } catch (...) {
        manager_.unpin(frame, false);
        throw;
    }
    manager_.unpin(frame, true);
}

void HeapFile::erase(const RowId row_id) {
    require_open();
    std::size_t frame = 0;
    SlottedPage page;
    pin_record_page(row_id, page, frame);
    try {
        page.erase(row_id.slot);
    } catch (...) {
        manager_.unpin(frame, false);
        throw;
    }
    manager_.unpin(frame, true);
}

void HeapFile::close() {
    require_open();
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
    if (!file_->is_open()) {
        throw common::Error("heap file is not open");
    }
    std::array<char, bufman::kPageSize> record{};
    while (page_id_ != kNoPage) {
        std::size_t frame = 0;
        file_->manager_.pin(file_->file_, page_id_, frame);
        SlottedPage page;
        page.bind(file_->manager_.frame(frame).buffer, file_->record_size_);
        std::uint32_t next_page_id = kNoPage;
        std::uint32_t found_slot = 0;
        bool found = false;
        try {
            page.attach();
            next_page_id = page.next_page();
            for (std::uint32_t slot = slot_; slot < page.capacity(); ++slot) {
                if (!page.is_occupied(slot)) {
                    continue;
                }
                page.get_record(slot, record.data(), record.size());
                found_slot = slot;
                found = true;
                break;
            }
        } catch (...) {
            file_->manager_.unpin(frame, false);
            throw;
        }
        // Copy first, decode after the pin is released: the record bytes
        // live in the frame buffer only while it stays pinned.
        file_->manager_.unpin(frame, false);
        if (found) {
            file_->serializer_->deserialize(record.data(), record.size(), tuple);
            current_ = RowId{page_id_, found_slot};
            if (found_slot + 1 < page.capacity()) {
                slot_ = found_slot + 1; // stay on this page
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
        throw common::Error("cannot erase \"" + path + "\": the file is open");
    }
    if (std::remove(path.c_str()) != 0) {
        throw common::Error("cannot erase \"" + path + "\": " + std::strerror(errno));
    }
}

}
