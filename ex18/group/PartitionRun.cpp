#include "group/PartitionRun.h"

#include "common/Error.h"
#include "schema/TableSchemaJson.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <format>

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
    // Unreachable failure: these column types are pre-checked.
    schema::parse_table_schema(text, schema, no_indexes);
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

void PartitionRunWriter::open(const std::string& path,
                              const schema::TableSchema& schema) {
    close_quietly();
    schema_ = schema;  // copy: the serializer binds our member's address
    serializer_.emplace(schema_);
    record_size_ = serializer_->record_size();
    if (record_size_ == 0) {
        throw common::Error("spill run: the mini-schema has no record layout");
    }
    if (record_size_ > bufman::kBlockSize - heapfile::kPageHeaderSize - 1) {
        throw common::Error(std::format(
            "spill run: a {}-byte page fits records of at most {} bytes, "
            "the mini-schema needs {}",
            bufman::kBlockSize,
            bufman::kBlockSize - heapfile::kPageHeaderSize - 1,
            record_size_));
    }
    capacity_ = heapfile::slot_capacity(record_size_);

    // A stale file at the path (a leaked run from a crashed statement)
    // must not be appended to: runs are always fresh files.
    std::remove(path.c_str());
    file_ = bufman::open_for_append(path);
    open_ = true;

    // Block 0: the identification record inside a SlottedPage-formatted
    // header, with an optimistic next link (patched back at close when
    // no rows arrive).
    const std::string& name = schema_.table_name();
    if (name.size() > kMaxRunNameLength) {
        close_quietly();
        throw common::Error(std::format(
            "spill run: the identification name is {} bytes; at most {} fit "
            "the header page",
            name.size(), kMaxRunNameLength));
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
    try {
        bufman::write_block(file_, 0, block.data(), block.size());
    } catch (const common::Error& e) {
        close_quietly();
        throw common::Error(std::format("spill run: {}", e.what()));
    }
    page_.fill('\0');
    page_id_ = 1;
    slot_ = 0;
    empty_ = true;
}

void PartitionRunWriter::append(const std::vector<exec::Value>& row) {
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
        try {
            bufman::write_block(file_, page_id_, page_.data(), page_.size());
        } catch (const common::Error& e) {
            throw common::Error(std::format("spill run: {}", e.what()));
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
    std::string record;
    try {
        record = serializer_->serialize(tuple);
    } catch (const common::Error& e) {
        throw common::Error(std::format("spill run: {}", e.what()));
    }
    try {
        view_.put_record(slot_, record);
    } catch (const common::Error& e) {
        throw common::Error(std::format("spill run: {}", e.what()));
    }
    ++slot_;
    empty_ = false;
}

void PartitionRunWriter::close() {
    if (!open_) {
        return;  // idempotent
    }
    if (!empty_) {
        // Flush the pending page when it holds records.
        if (slot_ > 0) {
            try {
                bufman::write_block(file_, page_id_, page_.data(),
                                    page_.size());
            } catch (const common::Error& e) {
                close_quietly();
                throw common::Error(std::format("spill run: {}", e.what()));
            }
        }
        // The trailer patch: the last written page carried an optimistic
        // next link; it is the chain tail.
        const std::uint32_t last_page =
            slot_ > 0 ? page_id_ : page_id_ - 1;
        if (last_page >= 1) {
            std::array<char, bufman::kBlockSize> tail{};
            try {
                bufman::read_block(file_, last_page, tail.data(),
                                   tail.size());
            } catch (const common::Error& e) {
                close_quietly();
                throw common::Error(std::format("spill run: {}", e.what()));
            }
            write_u32(tail.data(), heapfile::kNextPageIdOffset,
                      heapfile::kNoPage);
            try {
                bufman::write_block(file_, last_page, tail.data(),
                                    tail.size());
            } catch (const common::Error& e) {
                close_quietly();
                throw common::Error(std::format("spill run: {}", e.what()));
            }
        }
    } else {
        // No rows arrived: patch the header's optimistic link back to
        // the empty chain.
        std::array<char, bufman::kBlockSize> header{};
        try {
            bufman::read_block(file_, 0, header.data(), header.size());
        } catch (const common::Error& e) {
            close_quietly();
            throw common::Error(std::format("spill run: {}", e.what()));
        }
        write_u32(header.data(), heapfile::kNextPageIdOffset,
                  heapfile::kNoPage);
        try {
            bufman::write_block(file_, 0, header.data(), header.size());
        } catch (const common::Error& e) {
            close_quietly();
            throw common::Error(std::format("spill run: {}", e.what()));
        }
    }
    close_quietly();
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

void PartitionRunReader::open(const std::string& path,
                              const schema::TableSchema& schema) {
    close_quietly();
    schema_ = schema;
    serializer_.emplace(schema_);
    record_size_ = serializer_->record_size();

    try {
        file_ = bufman::open_for_read(path);
    } catch (const common::Error& e) {
        throw common::Error(std::format("spill run: {}", e.what()));
    }
    open_ = true;
    std::array<char, bufman::kBlockSize> header{};
    try {
        bufman::read_block(file_, 0, header.data(), header.size());
    } catch (const common::Error& e) {
        close_quietly();
        throw common::Error(std::format("spill run: {}", e.what()));
    }
    if (read_u32(header.data(), kMagicOffset) != kRunMagic) {
        close_quietly();
        throw common::Error("spill run: not a spill run file");
    }
    const std::uint32_t length = read_u32(header.data(), kNameLengthOffset);
    if (length > kMaxRunNameLength) {
        close_quietly();
        throw common::Error("spill run: not a spill run file");
    }
    const std::string stored(header.data() + kNameOffset, length);
    if (stored != schema_.table_name()) {
        close_quietly();
        throw common::Error(
            std::format("spill run: the run belongs to '{}'", stored));
    }
    heapfile::SlottedPage header_page;
    header_page.bind(header.data(), record_size_);
    try {
        header_page.attach();
    } catch (const common::Error& e) {
        close_quietly();
        throw common::Error(std::format("spill run: {}", e.what()));
    }
    page_id_ = header_page.next_page();
    slot_ = 0;
}

bool PartitionRunReader::next(std::vector<exec::Value>& row) {
    row.clear();
    // Internal contract: the executor drives the reader.
    assert(open_ && "the spill run reader must be open");
    while (page_id_ != heapfile::kNoPage) {
        if (slot_ == 0) {
            // Enter the page: one raw read, no pool.
            try {
                bufman::read_block(file_, page_id_, page_.data(),
                                   page_.size());
            } catch (const common::Error& e) {
                close_quietly();
                throw common::Error(std::format("spill run: {}", e.what()));
            }
            view_.bind(page_.data(), record_size_);
            try {
                view_.attach();
            } catch (const common::Error& e) {
                close_quietly();
                throw common::Error(std::format("spill run: {}", e.what()));
            }
        }
        if (slot_ < view_.occupied_count()) {
            std::array<char, bufman::kBlockSize> record{};
            try {
                view_.get_record(slot_, record.data(), record.size());
            } catch (const common::Error& e) {
                close_quietly();
                throw common::Error(std::format("spill run: {}", e.what()));
            }
            tuple::Tuple tuple;
            try {
                serializer_->deserialize(record.data(), record.size(), tuple);
            } catch (const common::Error& e) {
                close_quietly();
                throw common::Error(std::format("spill run: {}", e.what()));
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
