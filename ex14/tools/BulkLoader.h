#pragma once

// BulkLoader — the bootstrap loader for empty heap files. A tool, not a
// class member: it builds a valid heap file outside the HeapFile API by
// composing each slotted page in memory and writing it to disk exactly
// once, bypassing the buffer pool entirely (sequential write-once I/O
// needs no pool, and the per-row insert walk is quadratic).
//
// The file contract is the HeapFile class's own: block 0 is a
// SlottedPage-formatted header page carrying the identification record
// (magic, table name) and the chain head; data pages are block ids 1..n
// filled to slot_capacity and chained in generation order, with row ids
// in the same (page, slot) pattern sequential inserts produce. The
// result is engine-equivalent: HeapFile::open/scan/find treat it as
// their own.
//
// I/O is raw 4096-byte blocks through BlockFile — no BufferManager. The
// caller therefore runs this tool BEFORE any buffer manager opens the
// path (bootstrap-before-open): the tool writes behind any pool's back
// and cannot honor another manager's cached state.

#include "schema/TableSchema.h"
#include "tuple/Tuple.h"

#include <cstddef>
#include <functional>
#include <string>

namespace tools {

// One generated row: called with index 0..count-1 in order; fills `out`
// and returns true, or returns false with a named error (which fails
// the whole load, naming the index).
using RowGenerator = std::function<bool(std::size_t index, tuple::Tuple& out,
                                        std::string& error)>;

struct BulkLoadStats {
    std::size_t records = 0;            // rows stored
    std::size_t pages = 0;              // data pages written
    std::size_t per_page_capacity = 0;  // K: slots per data page
};

// Fills the (missing or empty) heap file at `path` with `count` records
// from `generator`. A missing file is created and formatted (header
// page with the identification record); an existing one-block file with
// an empty chain is accepted; anything else is refused with a named
// error before any write. On success the file is closed and complete;
// on failure the records already written remain (a disposable prefix —
// the last page's next link may dangle).
bool bulk_load(const std::string& path, const schema::TableSchema& schema,
               std::size_t count, const RowGenerator& generator,
               BulkLoadStats& stats, std::string& error);

}
