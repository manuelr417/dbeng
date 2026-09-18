#ifndef EX10_HEAP_FILE_H
#define EX10_HEAP_FILE_H

#include "bufpool/BufferManager.h"
#include "heapfile/SlottedPage.h"
#include "part/Person.h"

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
/// yields the next occupied slot's record, walking the page chain head ->
/// tail; once the chain is exhausted, every further call keeps reporting
/// exhaustion (false with an empty error). Do not insert/erase while an
/// iterator is alive: the iteration order under concurrent mutation is
/// deliberately unspecified.
class HeapFileIterator {
public:
    /// Reports the next record: true and `person` filled when a record was
    /// found, false with an empty `error` on exhaustion, false with `error`
    /// filled on failure.
    bool next(Person& person, std::string& error);

private:
    friend class HeapFile;
    HeapFileIterator(HeapFile& file, std::uint32_t page_id, std::uint32_t slot);

    HeapFile* file_;        // the owning HeapFile
    std::uint32_t page_id_; // kNoPage once the chain is exhausted
    std::uint32_t slot_;    // next slot to inspect on page_id_
};

/// A file of fixed-length Person records stored in a chain of slotted pages.
/// Block 0 is the header page: formatted like a SlottedPage, but its slots
/// are never used — its only meaningful field is `next`, the id of the first
/// data page (kNoPage for an empty file). Page ids are block numbers; pages
/// are allocated once and never removed. Every operation holds exactly one
/// pinned page at a time and reaches the disk only through the injected
/// BufferManager. One HeapFile manages one open file at a time and expects
/// to be the only user of its manager (the academic single-file pattern):
/// close() flushes and closes via close_all.
class HeapFile {
public:
    explicit HeapFile(bufman::BufferManager& manager);

    bool is_open() const;

    /// Creates a new one-block heap file (just the header page) at `path`.
    /// Refuses a path that already holds a non-empty file; an existing empty
    /// path is formatted. The new file is left closed.
    bool create(const std::string& path, std::string& error);

    /// Opens an existing heap file read-write, validating that the file has
    /// at least one block and block 0 is a header page for Person records.
    bool open(const std::string& path, std::string& error);

    /// Starts a scan over all records in chain order. The walk starts at
    /// block 0: the header page never holds occupied slots, so the first
    /// next() call skips straight to the first data page.
    HeapFileIterator scan();

    /// Serializes the record into the first free slot found by walking the
    /// chain from the header; when every page is full, allocates a new tail
    /// page, splices it after the old tail, and uses its slot 0. Reports the
    /// new record's row id.
    bool insert(const Person& person, RowId& row_id, std::string& error);

    /// Copies the record at `row_id` into `person`. Fails when the row id
    /// names the header page, a page beyond the end of the file, an
    /// out-of-range slot, or a free slot.
    bool find(RowId row_id, Person& person, std::string& error);

    /// Overwrites the record at `row_id` with `person` (occupancy stays 1).
    bool update(RowId row_id, const Person& person, std::string& error);

    /// Clears the occupancy byte of `row_id`'s slot; the record bytes stay
    /// behind as garbage and the slot becomes reusable by later inserts.
    bool erase(RowId row_id, std::string& error);

    /// Flushes and closes the file through close_all; every later operation
    /// fails with "heap file is not open".
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
    bool open_ = false;
};

/// Deletes the heap file at `path` from disk with the standard file-remove
/// call. Refuses while the file is open through `manager` — the registry
/// every HeapFile of the single-manager pattern reports to.
bool erase_file(bufman::BufferManager& manager, const std::string& path,
                std::string& error);

}

#endif // EX10_HEAP_FILE_H
