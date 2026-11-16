#include "hashindex/StaticHashPages.h"

#include <cstring>

namespace hashindex {

namespace {

std::uint32_t read_u32(const char* buffer, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

void write_u32(char* buffer, std::size_t offset, std::uint32_t value) {
    std::memcpy(buffer + offset, &value, sizeof(value));
}

}

// --- capacities -------------------------------------------------------------

std::size_t static_bucket_capacity(std::size_t key_width) {
    // count u32 + first-overflow u32 + entries (key + rid)
    return (kBlockSize - 8) / (key_width + 8);
}

// --- StaticBucketPage ---------------------------------------------------------

void StaticBucketPage::bind(char* buffer, std::size_t key_width) {
    HashEntryPage::bind(buffer, key_width, 8, static_bucket_capacity(key_width));
}

void StaticBucketPage::format(std::uint32_t first_overflow) {
    write_count(0);
    set_first_overflow(first_overflow);
}

bool StaticBucketPage::attach(std::string& error) const {
    return HashEntryPage::attach(error);
}

std::uint32_t StaticBucketPage::first_overflow() const {
    return read_u32(buffer_, kFirstOverflowOffset);
}

void StaticBucketPage::set_first_overflow(std::uint32_t page_id) {
    write_u32(buffer_, kFirstOverflowOffset, page_id);
}

}
