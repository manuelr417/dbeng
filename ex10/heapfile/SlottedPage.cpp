#include "heapfile/SlottedPage.h"

#include "part/PersonSerializer.h"

#include <cstring>

namespace heapfile {
namespace {

// Native-endian 4-byte integers, matching the existing serializer's
// memcpy-based approach (single-host format).
std::uint32_t read_u32(const char* buffer, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

void write_u32(char* buffer, std::size_t offset, std::uint32_t value) {
    std::memcpy(buffer + offset, &value, sizeof(value));
}

}

std::size_t slot_capacity(std::size_t record_size) {
    return (bufman::kPageSize - kPageHeaderSize) / (record_size + 1);
}

std::size_t dead_space(std::size_t record_size) {
    return (bufman::kPageSize - kPageHeaderSize) -
           slot_capacity(record_size) * (record_size + 1);
}

std::size_t SlottedPage::record_offset(std::size_t slot) const {
    return slot * record_size_;
}

// The occupancy array sits directly below the header: occ[i] is at
// [kPageSize - 12 - C + i], so the array ends exactly at kSlotCountOffset.
std::size_t SlottedPage::occupancy_offset(std::size_t slot) const {
    return kSlotCountOffset - capacity_ + slot;
}

void SlottedPage::bind(char* buffer, std::size_t record_size) {
    buffer_ = buffer;
    record_size_ = record_size;
    capacity_ = 0;
}

void SlottedPage::format(std::uint32_t prev_page_id, std::uint32_t next_page_id) {
    capacity_ = slot_capacity(record_size_);
    write_u32(buffer_, kSlotCountOffset, static_cast<std::uint32_t>(capacity_));
    std::memset(buffer_ + (kSlotCountOffset - capacity_), 0, capacity_);
    write_u32(buffer_, kPrevPageIdOffset, prev_page_id);
    write_u32(buffer_, kNextPageIdOffset, next_page_id);
}

bool SlottedPage::attach(std::string& error) {
    error.clear();
    const std::size_t stored = read_u32(buffer_, kSlotCountOffset);
    const std::size_t expected = slot_capacity(record_size_);
    if (stored != expected) {
        error = "slotted page header holds slot count " + std::to_string(stored) +
                ", expected " + std::to_string(expected) + " for record size " +
                std::to_string(record_size_);
        return false;
    }
    capacity_ = stored;
    return true;
}

std::uint32_t SlottedPage::prev_page() const {
    return read_u32(buffer_, kPrevPageIdOffset);
}

std::uint32_t SlottedPage::next_page() const {
    return read_u32(buffer_, kNextPageIdOffset);
}

void SlottedPage::set_prev_page(std::uint32_t page_id) {
    write_u32(buffer_, kPrevPageIdOffset, page_id);
}

void SlottedPage::set_next_page(std::uint32_t page_id) {
    write_u32(buffer_, kNextPageIdOffset, page_id);
}

std::size_t SlottedPage::capacity() const {
    return capacity_;
}

bool SlottedPage::slot_in_range(std::size_t slot, std::string& error) const {
    if (slot >= capacity_) {
        error = "slot " + std::to_string(slot) + " is out of range (capacity " +
                std::to_string(capacity_) + ")";
        return false;
    }
    return true;
}

bool SlottedPage::is_occupied(std::size_t slot, std::string& error) const {
    error.clear();
    if (!slot_in_range(slot, error)) {
        return false;
    }
    return buffer_[occupancy_offset(slot)] != 0;
}

std::size_t SlottedPage::occupied_count() const {
    std::size_t count = 0;
    for (std::size_t slot = 0; slot < capacity_; ++slot) {
        if (buffer_[occupancy_offset(slot)] != 0) {
            ++count;
        }
    }
    return count;
}

bool SlottedPage::has_free_slot() const {
    return first_free_slot().has_value();
}

// Linear scan, lowest index first; recomputed on every call.
std::optional<std::size_t> SlottedPage::first_free_slot() const {
    for (std::size_t slot = 0; slot < capacity_; ++slot) {
        if (buffer_[occupancy_offset(slot)] == 0) {
            return slot;
        }
    }
    return std::nullopt;
}

bool SlottedPage::get(std::size_t slot, Person& person, std::string& error) const {
    if (!is_occupied(slot, error)) {
        if (error.empty()) {
            error = "slot " + std::to_string(slot) + " is free; no record to get";
        }
        return false;
    }
    if (!bufman::deserialize(buffer_ + record_offset(slot), record_size_, person)) {
        error = "record bytes in slot " + std::to_string(slot) + " failed to deserialize";
        return false;
    }
    return true;
}

bool SlottedPage::put(std::size_t slot, const Person& person, std::string& error) {
    error.clear();
    if (!slot_in_range(slot, error)) {
        return false;
    }
    bufman::serialize(person, buffer_ + record_offset(slot), record_size_);
    buffer_[occupancy_offset(slot)] = 1;
    return true;
}

bool SlottedPage::erase(std::size_t slot, std::string& error) {
    error.clear();
    if (!slot_in_range(slot, error)) {
        return false;
    }
    buffer_[occupancy_offset(slot)] = 0;
    return true;
}

}
