#include "group/PartitionRun.h"

#include "common/Fail.h"
#include "schema/TableSchemaJson.h"

#include <cassert>
#include <cstdio>
#include <cstring>

namespace group {

namespace {

// The identification record's shape, re-derived from the heap header's
// layout (name length at 4, name bytes at 8 — the header page's
// never-used slot area). The equivalence check patches offset 0 to the
// heap magic and opens the run as a HeapFile; the two derivations must
// not drift.
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kNameLengthOffset = 4;
constexpr std::size_t kNameOffset = 8;
constexpr std::size_t kMaxRunNameLength = 200;

std::uint32_t read_u32(const char* buffer, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

void write_u32(char* buffer, std::size_t offset, std::uint32_t value) {
    std::memcpy(buffer + offset, &value, sizeof(value));
}

// Closes the block file on every exit path; close() is infallible.
struct FileGuard {
    bufman::File file;

    explicit FileGuard(bufman::File f) : file(f) {}
    ~FileGuard() {
        if (file.fd >= 0) {
            bufman::close(file);
        }
    }
};

exec::Value column_to_value(const tuple::Column& column) {
    switch (column.column_type()) {
        case tuple::ColumnType::Integer:
            return exec::Value(std::get<int>(column.column_value()));
        case tuple::ColumnType::Double:
            return exec::Value(std::get<double>(column.column_value()));
        case tuple::ColumnType::String:
            return exec::Value(std::get<std::string>(column.column_value()));
    }
    return exec::Value(0);
}

}

// --- the mini-schema ---------------------------------------------------------

schema::TableSchema run_schema(const std::string& name,
                               const std::vector<exec::Type>& types,
                               std::size_t string_width) {
    // Build the catalog text, then parse it through the sanctioned
    // writer of declared widths — the CREATE TABLE path's route.
    std::string text = "{\"table_name\":\"" + name + "\",\"columns\":[";
    for (std::size_t i = 0; i < types.size(); ++i) {
        const char* type_text = "Integer";
        std::size_t size = 4;
        switch (types[i]) {
            case exec::Type::Int:
                type_text = "Integer";
                size = 4;
                break;
            case exec::Type::Double:
                type_text = "Double";
                size = 8;
                break;
            default:
                type_text = "String";
                size = string_width;
                break;
        }
        if (i > 0) {
            text += ",";
        }
        text += "{\"column_name\":\"c" + std::to_string(i) +
                "\",\"column_type\":\"" + type_text +
                "\",\"size\":" + std::to_string(size) + "}";
    }
    text += "],\"indexes\":[]}";
    schema::TableSchema schema(name);
    std::vector<schema::TableIndex> no_indexes;
    std::string error;
    if (!schema::parse_table_schema(text, schema, no_indexes, error)) {
        return schema::TableSchema("");  // unreachable for these column types
    }
    return schema;
}

// --- the writer --------------------------------------------------------------

PartitionRunWriter::~PartitionRunWriter() { close_quietly(); }

void PartitionRunWriter::close_quietly() {
    if (file_.fd >= 0) {
        bufman::close(file_);
        file_ = bufman::File{};
    }
    open_ = false;
}

bool PartitionRunWriter::open(const std::string& path,
                              const schema::TableSchema& schema,
                              std::string& error) {
    close_quietly();
    schema_ = schema;  // copy: the serializer binds our member's address
    serializer_.emplace(schema_);
    record_size_ = serializer_->record_size();
    if (record_size_ == 0) {
        return common::fail(error, "spill run",
                            "the mini-schema has no record layout");
    }
    if (record_size_ > bufman::kBlockSize - heapfile::kPageHeaderSize - 1) {
        return common::fail(error, "spill run",
                            "a " + std::to_string(bufman::kBlockSize) +
                                "-byte page fits records of at most " +
                                std::to_string(bufman::kBlockSize -
                                               heapfile::kPageHeaderSize - 1) +
                                " bytes, the mini-schema needs " +
                                std::to_string(record_size_));
    }
    capacity_ = heapfile::slot_capacity(record_size_);

    // A stale file at the path (a leaked run from a crashed statement)
    // must not be appended to: runs are always fresh files.
    std::remove(path.c_str());
    if (!bufman::open_for_append(path, file_, error)) {
        return false;
    }
    open_ = true;

    // Block 0: the identification record inside a SlottedPage-formatted
    // header, with an optimistic next link (patched back at close when
    // no rows arrive).
    const std::string& name = schema_.table_name();
    if (name.size() > kMaxRunNameLength) {
        close_quietly();
        return common::fail(error, "spill run",
                            "the identification name is " +
                                std::to_string(name.size()) + " bytes; at most " +
                                std::to_string(kMaxRunNameLength) +
                                " fit the header page");
    }
    std::array<char, bufman::kBlockSize> block{};
    heapfile::SlottedPage header;
    header.bind(block.data(), record_size_);
    header.format();  // prev/next default to kNoPage
    header.set_next_page(1);  // optimistic: patched back if no rows come
    write_u32(block.data(), kMagicOffset, kRunMagic);
    write_u32(block.data(), kNameLengthOffset,
              static_cast<std::uint32_t>(name.size()));
    std::memcpy(block.data() + kNameOffset, name.data(), name.size());
    if (!bufman::write_block(file_, 0, block.data(), block.size(), error)) {
        close_quietly();
        return common::wrap(error, "spill run");
    }
    page_.fill('\0');
    page_id_ = 1;
    slot_ = 0;
    empty_ = true;
    return true;
}

bool PartitionRunWriter::append(const std::vector<exec::Value>& row,
                                std::string& error) {
    // Internal contract: the executor drives the writer and cannot call
    // it closed or with a mis-shaped record.
    assert(open_ && "the spill run writer must be open");
    assert(row.size() == schema_.column_count() &&
           "the record must match the run's mini-schema");
    tuple::Tuple tuple;
    for (const exec::Value& value : row) {
        if (const int* v = std::get_if<int>(&value)) {
            tuple.add(*v);
        } else if (const double* v = std::get_if<double>(&value)) {
            tuple.add(*v);
        } else {
            tuple.add(std::get<std::string>(value));
        }
    }
    if (slot_ == capacity_) {
        // Flush the full page and start a fresh one.
        if (!bufman::write_block(file_, page_id_, page_.data(), page_.size(),
                                 error)) {
            return common::wrap(error, "spill run");
        }
        page_.fill('\0');
        ++page_id_;
        slot_ = 0;
    }
    if (slot_ == 0) {
        // A fresh page: the BulkLoader's optimistic next link (the next
        // page id), patched to kNoPage at the tail by close().
        view_.bind(page_.data(), record_size_);
        view_.format(page_id_ - 1, page_id_ + 1);
    }
    std::array<char, bufman::kBlockSize> record{};
    if (!serializer_->serialize(tuple, record.data(), record.size(), error)) {
        error = "spill run: " + error;
        return false;
    }
    if (!view_.put_record(slot_, record.data(), record_size_, error)) {
        error = "spill run: " + error;
        return false;
    }
    ++slot_;
    empty_ = false;
    return true;
}

bool PartitionRunWriter::close(std::string& error) {
    if (!open_) {
        return true;  // idempotent
    }
    if (!empty_) {
        // Flush the pending page when it holds records.
        if (slot_ > 0) {
            if (!bufman::write_block(file_, page_id_, page_.data(),
                                     page_.size(), error)) {
                close_quietly();
                return common::wrap(error, "spill run");
            }
        }
        // The trailer patch: the last written page carried an optimistic
        // next link; it is the chain tail.
        const std::uint32_t last_page =
            slot_ > 0 ? page_id_ : page_id_ - 1;
        if (last_page >= 1) {
            std::array<char, bufman::kBlockSize> tail{};
            if (!bufman::read_block(file_, last_page, tail.data(),
                                    tail.size(), error)) {
                close_quietly();
                return common::wrap(error, "spill run");
            }
            write_u32(tail.data(), heapfile::kNextPageIdOffset,
                      heapfile::kNoPage);
            if (!bufman::write_block(file_, last_page, tail.data(),
                                     tail.size(), error)) {
                close_quietly();
                return common::wrap(error, "spill run");
            }
        }
    } else {
        // No rows arrived: patch the header's optimistic link back to
        // the empty chain.
        std::array<char, bufman::kBlockSize> header{};
        if (!bufman::read_block(file_, 0, header.data(), header.size(),
                                error)) {
            close_quietly();
            return common::wrap(error, "spill run");
        }
        write_u32(header.data(), heapfile::kNextPageIdOffset,
                  heapfile::kNoPage);
        if (!bufman::write_block(file_, 0, header.data(), header.size(),
                                 error)) {
            close_quietly();
            return common::wrap(error, "spill run");
        }
    }
    close_quietly();
    return true;
}

// --- the reader --------------------------------------------------------------

PartitionRunReader::~PartitionRunReader() { close_quietly(); }

void PartitionRunReader::close_quietly() {
    if (file_.fd >= 0) {
        bufman::close(file_);
        file_ = bufman::File{};
    }
    open_ = false;
    page_id_ = 0;
}

bool PartitionRunReader::open(const std::string& path,
                              const schema::TableSchema& schema,
                              std::string& error) {
    close_quietly();
    schema_ = schema;
    serializer_.emplace(schema_);
    record_size_ = serializer_->record_size();

    if (!bufman::open_for_read(path, file_, error)) {
        return common::wrap(error, "spill run");
    }
    open_ = true;
    std::array<char, bufman::kBlockSize> header{};
    if (!bufman::read_block(file_, 0, header.data(), header.size(), error)) {
        close_quietly();
        return common::wrap(error, "spill run");
    }
    if (read_u32(header.data(), kMagicOffset) != kRunMagic) {
        close_quietly();
        return common::fail(error, "spill run", "not a spill run file");
    }
    const std::uint32_t length = read_u32(header.data(), kNameLengthOffset);
    if (length > kMaxRunNameLength) {
        close_quietly();
        return common::fail(error, "spill run", "not a spill run file");
    }
    const std::string stored(header.data() + kNameOffset, length);
    if (stored != schema_.table_name()) {
        close_quietly();
        return common::fail(error, "spill run",
                            "the run belongs to '" + stored + "'");
    }
    heapfile::SlottedPage header_page;
    header_page.bind(header.data(), record_size_);
    if (!header_page.attach(error)) {
        close_quietly();
        return common::wrap(error, "spill run");
    }
    page_id_ = header_page.next_page();
    slot_ = 0;
    return true;
}

bool PartitionRunReader::next(std::vector<exec::Value>& row,
                              std::string& error) {
    row.clear();
    // Internal contract: the executor drives the reader.
    assert(open_ && "the spill run reader must be open");
    while (page_id_ != heapfile::kNoPage) {
        if (slot_ == 0) {
            // Enter the page: one raw read, no pool.
            if (!bufman::read_block(file_, page_id_, page_.data(),
                                    page_.size(), error)) {
                close_quietly();
                return common::wrap(error, "spill run");
            }
            view_.bind(page_.data(), record_size_);
            if (!view_.attach(error)) {
                close_quietly();
                return common::wrap(error, "spill run");
            }
        }
        if (slot_ < view_.occupied_count()) {
            std::array<char, bufman::kBlockSize> record{};
            if (!view_.get_record(slot_, record.data(), record.size(),
                                  error)) {
                close_quietly();
                return common::wrap(error, "spill run");
            }
            tuple::Tuple tuple;
            if (!serializer_->deserialize(record.data(), record.size(),
                                          tuple, error)) {
                close_quietly();
                return common::wrap(error, "spill run");
            }
            for (std::size_t i = 0; i < tuple.column_count(); ++i) {
                row.push_back(column_to_value(tuple.column(i)));
            }
            ++slot_;
            return true;
        }
        // The page is exhausted: follow the chain.
        page_id_ = view_.next_page();
        slot_ = 0;
    }
    close_quietly();  // clean end of the run
    return false;
}

}
