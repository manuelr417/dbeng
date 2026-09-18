#ifndef EX10_SLOTTED_PAGE_H
#define EX10_SLOTTED_PAGE_H

#include "bufpool/DataFrame.h"
#include "part/Person.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace heapfile {

// Fixed page header: the last 12 bytes of every page are three
// native-endian 4-byte integers — slot count, prev page id, next page id.
constexpr std::size_t kPageHeaderSize = 12;
constexpr std::size_t kSlotCountOffset = bufman::kPageSize - kPageHeaderSize;
constexpr std::size_t kPrevPageIdOffset = bufman::kPageSize - 8;
constexpr std::size_t kNextPageIdOffset = bufman::kPageSize - 4;
constexpr std::size_t kOccupancyArrayEnd = kSlotCountOffset;

// Sentinel for "no page in this direction" in the chain links. Valid page
// ids are block offsets, and 0 is a legal id, so 0 can never be the sentinel.
constexpr std::uint32_t kNoPage = 0xFFFFFFFFu;

// Slot capacity for a record of R bytes: each slot costs the
// record plus its occupancy byte, and the 12-byte header comes off the top.
std::size_t slot_capacity(std::size_t record_size);

// Dead space left after C slots of R+1 bytes each — always smaller than one
// more slot, by construction.
std::size_t dead_space(std::size_t record_size);

/// A non-owning view over one page-sized buffer (a DataFrame::buffer) that
/// interprets the bytes as a slotted page of fixed-length Person records.
/// The page manages layout only: pin/unpin, dirty marking, and write-back
/// stay in BufferManager. Lifecycle: bind a buffer and record size, then
/// either format() a fresh page or attach() to existing bytes. Every record
/// operation is bounds-checked against the capacity read at format/attach.
class SlottedPage {
public:
    /// Binds the view to a buffer and record size without touching bytes;
    /// capacity is unset until format() or attach() runs.
    void bind(char* buffer, std::size_t record_size);

    /// Formats a fresh page: writes the slot count derived from the record
    /// size, zeroes the occupancy array, and stores prev/next ids. Record
    /// bytes and dead space are left untouched.
    void format(std::uint32_t prev_page_id = kNoPage,
                std::uint32_t next_page_id = kNoPage);

    /// Attaches to existing bytes: reads the header slot count and verifies
    /// it matches the value recomputed from the bound record size. A mismatch
    /// means corrupted or foreign bytes and is a hard error.
    bool attach(std::string& error);

    // --- Chain navigation ---------------------------------------------------
    std::uint32_t prev_page() const;
    std::uint32_t next_page() const;
    void set_prev_page(std::uint32_t page_id);
    void set_next_page(std::uint32_t page_id);

    // --- Slot inspection ----------------------------------------------------
    std::size_t capacity() const;
    std::size_t occupied_count() const;
    bool has_free_slot() const;
    std::optional<std::size_t> first_free_slot() const;
    /// True if occupied, false with empty error if free, false with error
    /// filled if the slot index is out of range.
    bool is_occupied(std::size_t slot, std::string& error) const;

    // --- Record operations (bounds-checked) ---------------------------------
    /// Copies the record out of an occupied slot; fails on a free slot.
    bool get(std::size_t slot, Person& person, std::string& error) const;
    /// Copies the record in (overwriting either way) and marks it occupied.
    bool put(std::size_t slot, const Person& person, std::string& error);
    /// Marks the slot free; the record bytes stay behind as garbage.
    bool erase(std::size_t slot, std::string& error);

private:
    std::size_t record_offset(std::size_t slot) const;
    std::size_t occupancy_offset(std::size_t slot) const;
    bool slot_in_range(std::size_t slot, std::string& error) const;

    char* buffer_ = nullptr;
    std::size_t record_size_ = 0;
    std::size_t capacity_ = 0;
};

}

#endif // EX10_SLOTTED_PAGE_H
