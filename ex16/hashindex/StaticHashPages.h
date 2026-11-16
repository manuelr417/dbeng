#pragma once

// StaticHashPages — the page views over static-hash blocks, the lecture
// 11 baseline scheme beside the extensible one. A static file has two
// page castes: StaticBucketPage (a primary bucket: its overflow link and
// (key, rid) entries in arrival order — no local-depth field, the bucket
// count never changes) and the extensible module's OverflowPage reused
// verbatim for the spill chains. The entry machinery is HashEntryPage,
// shared with HashPages.h; the overflow capacity is that module's
// overflow_capacity unchanged.
//
// One encoding: keys move through index/KeyCodec only (the one key
// encoding both index kinds share); a row address is index/KeyCodec's
// module's Rid, so both index kinds speak one row-address vocabulary.

#include "hashindex/HashPages.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace hashindex {

// --- capacities -------------------------------------------------------------

// Static bucket page: count u32 + first-overflow u32 + entries — the
// same 8-byte header shape as an overflow page.
std::size_t static_bucket_capacity(std::size_t key_width);

// --- StaticBucketPage: a primary bucket ---------------------------------------

class StaticBucketPage : public HashEntryPage {
public:
    void bind(char* buffer, std::size_t key_width);

    // format: zero entries, overflow link as given.
    void format(std::uint32_t first_overflow);

    bool attach(std::string& error) const;  // count check + HashEntryPage check

    std::uint32_t first_overflow() const;
    void set_first_overflow(std::uint32_t page_id);

private:
    static constexpr std::size_t kFirstOverflowOffset = 4;
};

}
