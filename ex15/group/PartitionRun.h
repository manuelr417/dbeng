#pragma once

// group/PartitionRun — the spill run files (design D7).
//
// A run stores plain input tuples — the group-key values plus one value
// per distinct aggregate argument, pre-evaluated — under a synthesized
// mini-schema (run_schema: key columns then argument columns, types from
// binding). The format is the BulkLoader's discipline verbatim:
// 4096-byte blocks, local SlottedPage composition, each block written
// once with an optimistic next link and one trailer patch. Block 0
// carries the magic "GRUN" and the identification record; after the
// magic the layout is engine-equivalent, so the check suite can open a
// written run as a HeapFile (patching the four identification bytes)
// and scan it back record-for-record.
//
// Runs are written and read POOL-FREE (raw bufman::File block I/O):
// temp I/O must not evict session pages from the buffer pool. The
// reader walks the page chain sequentially, one block buffer.

#include "bufpool/DataFrame.h"
#include "file/BlockFile.h"
#include "exec/Value.h"
#include "heapfile/SlottedPage.h"
#include "schema/TableSchema.h"
#include "schema/TupleSerializer.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace group {

// The run's mini-schema: `types.size()` columns named key0.. / arg0..
// (callers put the group-key types first). String columns get
// `string_width`; the executor passes the widest string column of the
// source table, the only way a spill record can carry a string (there
// are no string-producing expressions in this surface).
schema::TableSchema run_schema(const std::string& name,
                               const std::vector<exec::Type>& types,
                               std::size_t string_width);

// The header-page identification record: magic "GRUN" at offset 0, then
// the heap header's name-length/name shape. Exposed for the equivalence
// check, which patches the magic to open a run as a HeapFile.
constexpr std::uint32_t kRunMagic = 0x4E555247u; // "GRUN"

// Writes one run file. open() creates a fresh file (any stale file at
// the path is removed first); append() serializes one row; close()
// patches the chain tail and closes. Close also runs best-effort in the
// destructor — a named error path leaves a closed file either way.
class PartitionRunWriter {
public:
    PartitionRunWriter() = default;
    ~PartitionRunWriter();
    PartitionRunWriter(const PartitionRunWriter&) = delete;
    PartitionRunWriter& operator=(const PartitionRunWriter&) = delete;

    bool open(const std::string& path, const schema::TableSchema& schema,
              std::string& error);
    bool append(const std::vector<exec::Value>& row, std::string& error);
    bool close(std::string& error);

private:
    void close_quietly();

    schema::TableSchema schema_{"run"};
    std::optional<schema::TupleSerializer> serializer_;
    bufman::File file_{};
    std::array<char, bufman::kBlockSize> page_{};
    heapfile::SlottedPage view_;
    std::size_t record_size_ = 0;
    std::size_t capacity_ = 0;
    std::size_t slot_ = 0;
    std::uint32_t page_id_ = 0;  // the page being filled (1-based after header)
    bool open_ = false;
    bool empty_ = true;
};

// Reads one run file back, pool-free and sequential. next() follows the
// page chain; it returns false with an EMPTY error at the clean end of
// the run and false with a named error on any failure. The file closes
// when the run is exhausted (or in the destructor).
class PartitionRunReader {
public:
    PartitionRunReader() = default;
    ~PartitionRunReader();
    PartitionRunReader(const PartitionRunReader&) = delete;
    PartitionRunReader& operator=(const PartitionRunReader&) = delete;

    bool open(const std::string& path, const schema::TableSchema& schema,
              std::string& error);
    bool next(std::vector<exec::Value>& row, std::string& error);

private:
    void close_quietly();

    schema::TableSchema schema_{"run"};
    std::optional<schema::TupleSerializer> serializer_;
    bufman::File file_{};
    std::array<char, bufman::kBlockSize> page_{};
    heapfile::SlottedPage view_;
    std::size_t record_size_ = 0;
    std::size_t slot_ = 0;
    std::uint32_t page_id_ = 0;  // kNoPage: exhausted
    bool open_ = false;
};

}
