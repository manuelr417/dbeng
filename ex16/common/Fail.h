#pragma once

// common/ — the named-error idiom shared by every module (NEW in ex16).
//
// ex16 keeps the ex11–ex15 error model — a `bool` return plus a named
// error string the caller can print — but concentrates the plumbing
// here. `fail` reports a fresh message under a context; `wrap` re-reports
// a nested call's error under the same context. Both return false, so a
// failure site is one line:
//
//     return common::fail(error, "spill run", "the writer is not open");
//     if (!bufman::write_block(...)) {
//         return common::wrap(error, "spill run");
//     }
//
// What earns a named error is decided at the system boundary — the
// parser, the binder, the executor, and everything that opens a file
// from disk. A check that guards a module's own contract (a caller
// misusing an internal API) is an assert instead, with the message
// serving as the contract's documentation; the check suites never
// observe one, because a correct caller cannot reach it.

#include <string>
#include <string_view>

namespace common {

// Reports `context: message` and returns false.
inline bool fail(std::string& error, std::string_view context,
                 std::string_view message) {
    error = std::string(context) + ": " + std::string(message);
    return false;
}

// Re-reports an error from a nested call as `context: <error>` and
// returns false.
inline bool wrap(std::string& error, std::string_view context) {
    error = std::string(context) + ": " + error;
    return false;
}

}
