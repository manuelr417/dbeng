#ifndef EX11_HEAP_FILE_H
#define EX11_HEAP_FILE_H

#include "bufpool/BufferManager.h"
#include "heapfile/SlottedPage.h"
#include "schema/TableSchema.h"
#include "tuple/Tuple.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace heapfile {

/// Address of one record in a heap file: the page id (the block number,
/// where block 0 is the header page) and the slot number inside that page.
/// A row id is valid only while its record exists: erasing frees the slot,
/// and a later insert may hand out the same row id again.
struct RowId {
    std::uint32_t page_id = 0;
    std::uint32_t slot = 0;
};

inline bool operator==(const RowId& a, const RowId& b) {
    return a.page_id == b.page_id && a.slot == b.slot;
}

inline bool operator!=(const RowId& a, const RowId& b) {
    return !(a == b);
}

class HeapFile;

/// Scan iterator over the records of one open HeapFile. Each next() call
/// yields the next occupied slot's tuple, walking the page chain head ->
/// tail; once the chain is exhausted, every further call keeps reporting
/// exhaustion (false with an empty error). Do not insert/erase while an
/// iterator is alive: the iteration order under concurrent mutation is
/// deliberately unspecified.
class HeapFileIterator {
public:
    /// Reports the next tuple: true and `tuple` filled when a record was
    /// found, false with an empty `error` on exhaustion, false with
    /// `error` filled on failure (including a record that violates the
    /// schema).
    bool next(tuple::Tuple& tuple, std::string& error);

    /// The row id of the record most recently reported by next(); valid
    /// only after a true return.
    const RowId& row_id() const { return current_; }

private:
    friend class HeapFile;
    HeapFileIterator(HeapFile& file, std::uint32_t page_id, std::uint32_t slot);

    HeapFile* file_;        // the owning HeapFile
    std::uint32_t page_id_; // kNoPage once the chain is exhausted
    std::uint32_t slot_;    // next slot to inspect on page_id_
    RowId current_;         // row id of the last yielded record
};

/// A file of fixed-width tuple records stored in a chain of slotted pages.
/// The record layout is chosen by the schema handed to create/open: every
/// column occupies a fixed width (natural size for numerics, the
/// catalog-declared column_size for Strings), so one record is always
/// record_size(schema) bytes. Block 0 is the header page: formatted like a
/// SlottedPage, but its slots are never used — it carries the chain head
/// plus a small identification record (magic and table name) that open()
/// verifies against the schema it is handed. The schema itself lives in
/// the caller's catalog (the JSON loader); the file only stores its name.
/// Page ids are block numbers; pages are allocated once and never
/// removed. Every operation holds exactly one pinned page at a time and
/// reaches the disk only through the injected BufferManager. One HeapFile
/// manages one open file at a time and shares the session's buffer
/// manager with other files: it owns only its own file's lifecycle —
/// close() flushes and closes just this file (BufferManager::close_file),
/// leaving every other file the manager holds untouched. The schema must
/// outlive the open file.
class HeapFile {
public:
    explicit HeapFile(bufman::BufferManager& manager);

    bool is_open() const;

    /// Creates a new one-block heap file (just the header page, carrying
    /// the identification record) at `path`. Refuses a path that already
    /// holds a non-empty file; an existing empty path is formatted. The
    /// new file is left closed.
    bool create(const std::string& path, const schema::TableSchema& schema,
                std::string& error);

    /// Opens an existing heap file read-write, checking that block 0 is a
    /// tuple heap file header for `schema`'s table and that the pages
    /// hold records of the schema's record size. The schema must outlive
    /// the open file.
    bool open(const std::string& path, const schema::TableSchema& schema,
              std::string& error);

    /// Starts a scan over all records in chain order. The walk starts at
    /// block 0: the header page never holds occupied slots, so the first
    /// next() call skips straight to the first data page.
    HeapFileIterator scan();

    /// Validates `tuple` against the schema, packs it into the
    /// fixed-width record, and stores it in the first free slot found by
    /// walking the chain from the header; when every page is full,
    /// allocates a new tail page, splices it after the old tail, and uses
    /// its slot 0. Reports the new record's row id.
    bool insert(const tuple::Tuple& tuple, RowId& row_id, std::string& error);

    /// Copies the record at `row_id` and unpacks it into `tuple`. Fails
    /// when the row id names the header page, a page beyond the end of
    /// the file, an out-of-range slot, a free slot, or a record that
    /// violates the schema.
    bool find(RowId row_id, tuple::Tuple& tuple, std::string& error);

    /// Packs `tuple` and overwrites the record at `row_id` (occupancy
    /// stays 1 — records are all the same size).
    bool update(RowId row_id, const tuple::Tuple& tuple, std::string& error);

    /// Clears the occupancy byte of `row_id`'s slot; the record bytes stay
    /// behind as garbage and the slot becomes reusable by later inserts.
    bool erase(RowId row_id, std::string& error);

    /// True when `page_id` is currently resident in the file's pool — the
    /// introspection a CLI needs to report pool hits. A read through
    /// find/scan decides residency on its own; this only observes.
    bool resident(std::uint32_t page_id) const;

    /// Flushes and closes only this file (BufferManager::close_file);
    /// every other file the manager holds stays open. Every later
    /// operation fails with "heap file is not open".
    bool close(std::string& error);

private:
    friend class HeapFileIterator;

    // Shared pin-and-validate path for find/update/erase: checks the row id
    // against the header page and the file's block count, pins the page,
    // attaches a view, and requires the slot occupied. On success the caller
    // owns one pin and must unpin it; on failure the pin is released here.
    bool pin_record_page(RowId row_id, SlottedPage& page, std::size_t& frame,
                         std::string& error);
    bool require_open(std::string& error) const;

    bufman::BufferManager& manager_;
    bufman::File file_;
    const schema::TableSchema* schema_ = nullptr; // caller-owned; outlives open
    std::size_t record_size_ = 0;
    bool open_ = false;
};

/// Deletes the heap file at `path` from disk with the standard file-remove
/// call. Refuses while the file is open through `manager` — the registry
/// every HeapFile of the single-manager pattern reports to.
bool erase_file(bufman::BufferManager& manager, const std::string& path,
                std::string& error);

}

#endif // EX11_HEAP_FILE_H
