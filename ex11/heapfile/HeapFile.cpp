#include "heapfile/HeapFile.h"

#include "schema/RecordCodec.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace heapfile {

namespace {

// Releases a pin without disturbing `error`, which on failure paths already
// carries the real cause (unpin clears the string it is given).
void release(bufman::BufferManager& manager, std::size_t frame, bool was_dirty) {
    std::string unpin_error;
    manager.unpin(frame, was_dirty, unpin_error);
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
// Returns false with a named error when the table name is too long.
bool write_identification(char* buffer, const schema::TableSchema& schema,
                          std::string& error) {
    const std::string& name = schema.table_name();
    if (name.size() > kMaxTableNameLength) {
        error = "table name is " + std::to_string(name.size()) +
                " bytes; at most " + std::to_string(kMaxTableNameLength) +
                " fit the header page";
        return false;
    }
    write_u32(buffer, kMagicOffset, kHeapMagic);
    write_u32(buffer, kNameLengthOffset,
              static_cast<std::uint32_t>(name.size()));
    std::memcpy(buffer + kNameOffset, name.data(), name.size());
    return true;
}

// Checks the identification record against `schema`. False with a named
// error when the magic is missing or the name differs.
bool check_identification(const char* buffer,
                          const schema::TableSchema& schema,
                          std::string& error) {
    if (read_u32(buffer, kMagicOffset) != kHeapMagic) {
        error = "not a tuple heap file";
        return false;
    }
    const std::uint32_t length = read_u32(buffer, kNameLengthOffset);
    if (length > kMaxTableNameLength) {
        error = "not a tuple heap file";
        return false;
    }
    const std::string stored(buffer + kNameOffset, length);
    if (stored != schema.table_name()) {
        error = "heap file is for table '" + stored + "'";
        return false;
    }
    return true;
}

}

HeapFile::HeapFile(bufman::BufferManager& manager) : manager_(manager) {}

bool HeapFile::is_open() const {
    return open_;
}

bool HeapFile::require_open(std::string& error) const {
    if (!open_) {
        error = "heap file is not open";
        return false;
    }
    return true;
}

bool HeapFile::create(const std::string& path,
                      const schema::TableSchema& schema, std::string& error) {
    error.clear();
    if (open_) {
        error = "cannot create \"" + path + "\": this heap file is already open";
        return false;
    }
    const std::size_t size = schema::record_size(schema);
    if (size == 0) {
        error = "cannot create \"" + path +
                "\": the catalog has no record layout (no columns, or "
                "String columns without declared width)";
        return false;
    }
    if (size > bufman::kPageSize - kPageHeaderSize - 1) {
        error = "cannot create \"" + path + "\": a " +
                std::to_string(bufman::kPageSize) +
                "-byte page fits records of at most " +
                std::to_string(bufman::kPageSize - kPageHeaderSize - 1) +
                " bytes, the catalog needs " + std::to_string(size);
        return false;
    }
    // open_for_append creates the file when missing, so this also covers the
    // fresh-path case; an existing empty path is allowed.
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

    std::size_t frame = 0;
    if (!manager_.alloc_page(file, 0, frame, error)) {
        std::string close_error;
        manager_.close_file(file, close_error);
        return false;
    }
    SlottedPage header;
    header.bind(manager_.frame(frame).buffer, size);
    header.format(); // prev/next default to kNoPage: an empty chain
    if (!write_identification(manager_.frame(frame).buffer, schema, error)) {
        release(manager_, frame, false);
        std::string close_error;
        manager_.close_file(file, close_error);
        return false;
    }
    release(manager_, frame, true);

    // Write the header back and leave a one-block file on disk.
    manager_.close_file(file, error);
    return error.empty();
}

bool HeapFile::open(const std::string& path,
                    const schema::TableSchema& schema, std::string& error) {
    error.clear();
    if (open_) {
        error = "cannot open \"" + path + "\": this heap file is already open";
        return false;
    }
    const std::size_t size = schema::record_size(schema);
    if (size == 0) {
        error = "cannot open \"" + path +
                "\": the catalog has no record layout (no columns, or "
                "String columns without declared width)";
        return false;
    }
    if (size > bufman::kPageSize - kPageHeaderSize - 1) {
        error = "cannot open \"" + path + "\": a " +
                std::to_string(bufman::kPageSize) +
                "-byte page fits records of at most " +
                std::to_string(bufman::kPageSize - kPageHeaderSize - 1) +
                " bytes, the catalog needs " + std::to_string(size);
        return false;
    }
    // Probe with a read-only open first: manager_.open_file would silently
    // create a missing file (O_CREAT), and a missing path must be reported
    // as such rather than as a validation failure.
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
        error = "cannot open \"" + path + "\": the file has no header page";
    }
    std::size_t frame = 0;
    if (error.empty() && manager_.pin(file_, 0, frame, error)) {
        SlottedPage header;
        header.bind(manager_.frame(frame).buffer, size);
        // Identification first: the magic and name do not depend on the
        // record size, and a foreign file must be reported as such rather
        // than as a slot-count mismatch from attach.
        if (!check_identification(manager_.frame(frame).buffer, schema,
                                  error)) {
            error = "cannot open \"" + path + "\": " + error;
        } else if (!header.attach(error)) {
            error = "cannot open \"" + path + "\": " + error;
        }
        release(manager_, frame, false);
    }
    if (!error.empty()) {
        // Unwind completely: an unusable file must not stay open. The close
        // gets its own error string so the real cause above survives.
        std::string close_error;
        manager_.close_file(file_, close_error);
        file_ = bufman::File{};
        return false;
    }
    schema_ = &schema;
    record_size_ = size;
    open_ = true;
    return true;
}

HeapFileIterator HeapFile::scan() {
    return HeapFileIterator(*this, 0, 0);
}

bool HeapFile::insert(const tuple::Tuple& tuple, RowId& row_id,
                      std::string& error) {
    if (!require_open(error)) {
        return false;
    }
    // pack validates the tuple against the schema first (arity, types,
    // string widths) and fails before any page is pinned.
    std::array<char, bufman::kPageSize> record{};
    if (!schema::pack(*schema_, tuple, record.data(), record.size(), error)) {
        return false;
    }

    // 1. The header is the chain head.
    std::size_t frame = 0;
    if (!manager_.pin(file_, 0, frame, error)) {
        return false;
    }
    SlottedPage page;
    page.bind(manager_.frame(frame).buffer, record_size_);
    if (!page.attach(error)) {
        release(manager_, frame, false);
        return false;
    }
    std::uint32_t current = page.next_page();
    std::uint32_t previous = 0;
    release(manager_, frame, false);

    // 2. Walk the chain for the first page with a free slot.
    while (current != kNoPage) {
        if (!manager_.pin(file_, current, frame, error)) {
            return false;
        }
        page.bind(manager_.frame(frame).buffer, record_size_);
        if (!page.attach(error)) {
            release(manager_, frame, false);
            return false;
        }
        const std::optional<std::size_t> slot = page.first_free_slot();
        if (slot) {
            if (!page.put_record(*slot, record.data(), record_size_, error)) {
                release(manager_, frame, false);
                return false;
            }
            row_id = RowId{current, static_cast<std::uint32_t>(*slot)};
            release(manager_, frame, true);
            return true;
        }
        previous = current;
        current = page.next_page();
        release(manager_, frame, false);
    }

    // 3. Chain exhausted — allocate and splice a new tail. Pages are
    // allocated strictly in increasing order and never removed, so the
    // first unused id is the old tail's id plus one — the header
    // itself plays tail for an empty chain. (The on-disk block count cannot
    // be used here: dirty pages reach disk only on eviction/flush, so it
    // lags behind the pool.)
    if (previous + 1 >= kNoPage) {
        error = "insert would need page id " + std::to_string(previous + 1) +
                ", beyond the 32-bit row id range";
        return false;
    }
    const std::uint32_t new_page_id = previous + 1;
    if (!manager_.alloc_page(file_, new_page_id, frame, error)) {
        return false;
    }
    page.bind(manager_.frame(frame).buffer, record_size_);
    page.format(previous, kNoPage);
    if (!page.put_record(0, record.data(), record_size_, error)) {
        release(manager_, frame, true);
        return false;
    }
    release(manager_, frame, true);

    if (!manager_.pin(file_, previous, frame, error)) {
        return false;
    }
    page.bind(manager_.frame(frame).buffer, record_size_);
    if (!page.attach(error)) {
        release(manager_, frame, false);
        return false;
    }
    page.set_next_page(new_page_id);
    release(manager_, frame, true);

    row_id = RowId{new_page_id, 0};
    return true;
}

bool HeapFile::pin_record_page(const RowId row_id, SlottedPage& page,
                               std::size_t& frame, std::string& error) {
    if (row_id.page_id == 0) {
        error = "page 0 is the header page; it never stores records";
        return false;
    }
    const std::uint64_t blocks = bufman::block_count(file_, error);
    if (!error.empty()) {
        return false;
    }
    // A page exists iff it is resident in the pool or already written back:
    // dirty pages reach disk only on eviction/flush, so the on-disk block
    // count alone would lag behind the pool and reject live pages.
    if (row_id.page_id >= blocks && !manager_.resident(file_, row_id.page_id)) {
        error = "page " + std::to_string(row_id.page_id) +
                " is beyond the end of the file";
        return false;
    }
    if (!manager_.pin(file_, row_id.page_id, frame, error)) {
        return false;
    }
    page.bind(manager_.frame(frame).buffer, record_size_);
    if (!page.attach(error)) {
        release(manager_, frame, false);
        return false;
    }
    if (!page.is_occupied(row_id.slot, error)) {
        if (error.empty()) {
            error = "no record at row id (page " + std::to_string(row_id.page_id) +
                    ", slot " + std::to_string(row_id.slot) + "): the slot is free";
        }
        release(manager_, frame, false);
        return false;
    }
    return true;
}

bool HeapFile::find(const RowId row_id, tuple::Tuple& tuple,
                    std::string& error) {
    if (!require_open(error)) {
        return false;
    }
    std::size_t frame = 0;
    SlottedPage page;
    if (!pin_record_page(row_id, page, frame, error)) {
        return false;
    }
    std::array<char, bufman::kPageSize> record{};
    const bool got = page.get_record(row_id.slot, record.data(), record.size(),
                                     error);
    release(manager_, frame, false);
    if (!got) {
        return false;
    }
    return schema::unpack(*schema_, record.data(), record.size(), tuple, error);
}

bool HeapFile::update(const RowId row_id, const tuple::Tuple& tuple,
                      std::string& error) {
    if (!require_open(error)) {
        return false;
    }
    std::array<char, bufman::kPageSize> record{};
    if (!schema::pack(*schema_, tuple, record.data(), record.size(), error)) {
        return false;
    }
    std::size_t frame = 0;
    SlottedPage page;
    if (!pin_record_page(row_id, page, frame, error)) {
        return false;
    }
    const bool ok = page.put_record(row_id.slot, record.data(), record_size_,
                                    error);
    release(manager_, frame, ok);
    return ok;
}

bool HeapFile::erase(const RowId row_id, std::string& error) {
    if (!require_open(error)) {
        return false;
    }
    std::size_t frame = 0;
    SlottedPage page;
    if (!pin_record_page(row_id, page, frame, error)) {
        return false;
    }
    const bool ok = page.erase(row_id.slot, error);
    release(manager_, frame, ok);
    return ok;
}

bool HeapFile::resident(std::uint32_t page_id) const {
    return manager_.resident(file_, page_id);
}

bool HeapFile::close(std::string& error) {
    if (!require_open(error)) {
        return false;
    }
    manager_.close_file(file_, error);
    open_ = false;
    schema_ = nullptr;
    record_size_ = 0;
    file_ = bufman::File{};
    return error.empty();
}

// --- HeapFileIterator -------------------------------------------------------

HeapFileIterator::HeapFileIterator(HeapFile& file, std::uint32_t page_id,
                                   std::uint32_t slot)
    : file_(&file), page_id_(page_id), slot_(slot) {}

bool HeapFileIterator::next(tuple::Tuple& tuple, std::string& error) {
    error.clear();
    if (!file_->is_open()) {
        error = "heap file is not open";
        return false;
    }
    std::array<char, bufman::kPageSize> record{};
    while (page_id_ != kNoPage) {
        std::size_t frame = 0;
        if (!file_->manager_.pin(file_->file_, page_id_, frame, error)) {
            return false;
        }
        SlottedPage page;
        page.bind(file_->manager_.frame(frame).buffer, file_->record_size_);
        if (!page.attach(error)) {
            release(file_->manager_, frame, false);
            return false;
        }
        const std::uint32_t next_page_id = page.next_page();
        bool found = false;
        for (std::uint32_t slot = slot_; slot < page.capacity(); ++slot) {
            if (!page.is_occupied(slot, error)) {
                continue;
            }
            if (!page.get_record(slot, record.data(), record.size(), error)) {
                release(file_->manager_, frame, false);
                return false;
            }
            // Copy first, decode after the pin is released: the record
            // bytes live in the frame buffer only while it stays pinned.
            release(file_->manager_, frame, false);
            if (!schema::unpack(*file_->schema_, record.data(), record.size(),
                                tuple, error)) {
                return false;
            }
            current_ = RowId{page_id_, slot};
            if (slot + 1 < page.capacity()) {
                slot_ = slot + 1; // stay on this page
            } else {
                page_id_ = next_page_id;
                slot_ = 0;
            }
            found = true;
            break;
        }
        if (found) {
            return true;
        }
        release(file_->manager_, frame, false);
        // Page exhausted from slot_ on: continue with the next one.
        page_id_ = next_page_id;
        slot_ = 0;
    }
    return false; // exhausted; the error stays empty on every later call
}

// --- Free functions ---------------------------------------------------------

bool erase_file(bufman::BufferManager& manager, const std::string& path,
                std::string& error) {
    error.clear();
    if (manager.is_open(path)) {
        error = "cannot erase \"" + path + "\": the file is open";
        return false;
    }
    if (std::remove(path.c_str()) != 0) {
        error = "cannot erase \"" + path + "\": " + std::strerror(errno);
        return false;
    }
    return true;
}

}
