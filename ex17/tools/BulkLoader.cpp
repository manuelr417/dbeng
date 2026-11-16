#include "tools/BulkLoader.h"

#include "common/Error.h"
#include "file/BlockFile.h"
#include "heapfile/SlottedPage.h"
#include "schema/TupleSerializer.h"

#include <array>
#include <cstring>

namespace tools {

namespace {

// The header-page identification record, re-derived from the same layout
// HeapFile encodes (magic "TUP1" at offset 0, name length at 4, name
// bytes at 8 — the header page's never-used slot area). The differential
// check in the suite fails loudly if the two derivations ever drift.
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

// Composes and writes block 0: a SlottedPage-formatted header page whose
// record area carries the identification record and whose next pointer
// is the chain head (1 when records are coming, kNoPage for an empty
// load).
void write_header_page(bufman::File& file, const schema::TableSchema& schema,
                       std::size_t record_size, bool records_follow) {
    const std::string& name = schema.table_name();
    if (name.size() > kMaxTableNameLength) {
        throw common::Error("bulk load: table name is " +
                            std::to_string(name.size()) + " bytes; at most " +
                            std::to_string(kMaxTableNameLength) +
                            " fit the header page");
    }
    std::array<char, bufman::kBlockSize> block{};
    heapfile::SlottedPage header;
    header.bind(block.data(), record_size);
    header.format(); // prev/next default to kNoPage: an empty chain
    if (records_follow) {
        header.set_next_page(1);
    }
    write_u32(block.data(), kMagicOffset, kHeapMagic);
    write_u32(block.data(), kNameLengthOffset,
              static_cast<std::uint32_t>(name.size()));
    std::memcpy(block.data() + kNameOffset, name.data(), name.size());
    bufman::write_block(file, 0, block.data(), block.size());
}

// Validates an existing one-block file's header: identification record
// first (a foreign file must be reported as such), then the slotted
// layout, then the empty chain.
void check_empty_header(const char* block, const schema::TableSchema& schema,
                        std::size_t record_size) {
    if (read_u32(block, kMagicOffset) != kHeapMagic) {
        throw common::Error("bulk load: not a tuple heap file");
    }
    const std::uint32_t length = read_u32(block, kNameLengthOffset);
    if (length > kMaxTableNameLength) {
        throw common::Error("bulk load: not a tuple heap file");
    }
    const std::string stored(block + kNameOffset, length);
    if (stored != schema.table_name()) {
        throw common::Error("bulk load: heap file is for table '" + stored +
                            "'");
    }
    heapfile::SlottedPage header;
    header.bind(const_cast<char*>(block), record_size);
    try {
        header.attach();
    } catch (const common::Error& e) {
        throw common::Error("bulk load: " + std::string(e.what()));
    }
    if (header.next_page() != heapfile::kNoPage) {
        throw common::Error("bulk load: the file already holds records");
    }
}

}  // namespace

void bulk_load(const std::string& path, const schema::TableSchema& schema,
               std::size_t count, const RowGenerator& generator,
               BulkLoadStats& stats) {
    stats = BulkLoadStats{};

    // The record layout decides everything structural; the same bounds
    // HeapFile::create enforces apply here.
    const schema::TupleSerializer serializer(schema);
    const std::size_t record_size = serializer.record_size();
    if (record_size == 0) {
        throw common::Error("bulk load: the catalog has no record layout "
                            "(no columns, or String columns without "
                            "declared width)");
    }
    if (record_size > bufman::kBlockSize - heapfile::kPageHeaderSize - 1) {
        throw common::Error("bulk load: a " +
                            std::to_string(bufman::kBlockSize) +
                            "-byte page fits records of at most " +
                            std::to_string(bufman::kBlockSize -
                                           heapfile::kPageHeaderSize - 1) +
                            " bytes, the catalog needs " +
                            std::to_string(record_size));
    }
    const std::size_t capacity = heapfile::slot_capacity(record_size);

    bufman::File file = bufman::open_for_append(path);
    FileGuard guard(file);

    // Bootstrap decision by block count, refused before any write.
    const std::uint64_t blocks = bufman::block_count(file);
    if (blocks > 1) {
        throw common::Error("bulk load: the file already holds " +
                            std::to_string(blocks) + " block(s)");
    }
    if (blocks == 1) {
        std::array<char, bufman::kBlockSize> block{};
        bufman::read_block(file, 0, block.data(), block.size());
        check_empty_header(block.data(), schema, record_size);
        // Reuse the file as-is: the header already carries the right
        // identification and an empty chain.
    } else {
        write_header_page(file, schema, record_size, count > 0);
    }

    // The fill/link loop: one local page buffer at a time, formatted
    // with an optimistic next link (P+1), filled to capacity, written
    // once. The last page's trailer is patched after the loop.
    std::array<char, bufman::kBlockSize> page{};
    std::size_t record_index = 0;
    std::uint32_t page_id = 1;
    std::array<char, bufman::kBlockSize> record{};
    while (record_index < count) {
        heapfile::SlottedPage view;
        view.bind(page.data(), record_size);
        view.format(page_id - 1, page_id + 1);
        for (std::size_t slot = 0;
             slot < capacity && record_index < count; ++slot) {
            tuple::Tuple tuple;
            std::string generator_error;
            if (!generator(record_index, tuple, generator_error)) {
                throw common::Error(
                    "bulk load: generator failed at row index " +
                    std::to_string(record_index) +
                    (generator_error.empty() ? "" : ": " + generator_error));
            }
            try {
                serializer.serialize(tuple, record.data(), record.size());
            } catch (const common::Error& e) {
                throw common::Error(
                    "bulk load: serialize failed at row index " +
                    std::to_string(record_index) + ": " + e.what());
            }
            try {
                view.put_record(slot, record.data(), record_size);
            } catch (const common::Error& e) {
                throw common::Error("bulk load: " + std::string(e.what()));
            }
            ++record_index;
        }
        try {
            bufman::write_block(file, page_id, page.data(), page.size());
        } catch (const common::Error& e) {
            throw common::Error("bulk load: " + std::string(e.what()));
        }
        ++page_id;
        page.fill('\0');
    }

    // The final trailer patch: the last written page carried an
    // optimistic next link; it is the chain tail.
    if (page_id > 1) {
        std::array<char, bufman::kBlockSize> tail{};
        const std::uint32_t last_page = page_id - 1;
        try {
            bufman::read_block(file, last_page, tail.data(), tail.size());
        } catch (const common::Error& e) {
            throw common::Error("bulk load: " + std::string(e.what()));
        }
        write_u32(tail.data(), heapfile::kNextPageIdOffset,
                  heapfile::kNoPage);
        try {
            bufman::write_block(file, last_page, tail.data(), tail.size());
        } catch (const common::Error& e) {
            throw common::Error("bulk load: " + std::string(e.what()));
        }
    }

    stats.records = record_index;
    stats.pages = page_id - 1;
    stats.per_page_capacity = capacity;
}

}
