#pragma once

// common/ — the pin guard (NEW in ex18, clean.md suggestion 3).
//
// A PinnedPage owns one pinned frame from construction to the end of its
// scope: the constructor pins, the destructor unpins (writing back when
// the page was marked dirty), and the attached view hands out the page's
// bytes. Every hand-written "unpin on every exit" path disappears — the
// roadmap's "pin → operate → unpin" rule is enforced by the scope, not
// by discipline:
//
//     PinnedPage<SlottedPage> page = pin(id);
//     if (auto slot = page->first_free_slot()) {
//         page->put_record(*slot, record);
//         page.mark_dirty();
//         ...
//     }                                          // unpins here, always
//
// The view is bound to the frame's buffer at construction (any bind
// signature — buffer plus record size, key width, or nothing — fits the
// variadic forwarding). Views may be re-bound through `view()` when a
// loop inspects several page shapes over one pin.

#include "bufpool/BufferManager.h"

#include <utility>

namespace common {

template <typename View>
class PinnedPage {
public:
    template <typename... Args>
    PinnedPage(bufman::BufferManager& manager, std::size_t frame,
               Args&&... args)
        : manager_(&manager), frame_(frame), dirty_(false) {
        view_.bind(manager.frame(frame).buffer, std::forward<Args>(args)...);
    }

    ~PinnedPage() {
        if (manager_ != nullptr) {
            manager_->unpin(frame_, dirty_);
        }
    }

    PinnedPage(const PinnedPage&) = delete;
    PinnedPage& operator=(const PinnedPage&) = delete;

    PinnedPage(PinnedPage&& other) noexcept
        : manager_(std::exchange(other.manager_, nullptr)),
          frame_(other.frame_), dirty_(other.dirty_),
          view_(std::move(other.view_)) {}

    View* operator->() { return &view_; }
    const View* operator->() const { return &view_; }
    View& operator*() { return view_; }
    View& view() { return view_; }
    char* buffer() { return manager_->frame(frame_).buffer; }

    void mark_dirty() { dirty_ = true; }

private:
    bufman::BufferManager* manager_;
    std::size_t frame_;
    bool dirty_;
    View view_;
};

}
