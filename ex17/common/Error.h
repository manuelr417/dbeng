#pragma once

// common/ — the error type every module throws (NEW in ex17).
//
// ex15 and ex16 reported failures with a `bool` return plus an `error`
// string out-parameter. The cost was structural: every fallible call had
// to be wrapped in an `if`, so each function alternated between work
// and error plumbing and the algorithm disappeared under the shells.
// ex17 keeps the messages but changes the mechanism: a failure is a
// `common::Error` carrying the same named message, thrown from the
// failure site and caught once at the system boundary — the CLI prints
// it, the check harness compares it, everything between just calls.
//
// The dividing rule from ex16 still holds: a reachable state (bad SQL,
// a missing or foreign file, an exhausted resource) throws with its
// named message; a check that guards a module's own contract — a
// caller misusing an internal API — is an `assert` instead, with the
// message documenting the contract. A correct caller never reaches
// either the throw-free contract or the assert.

#include <stdexcept>
#include <string>

namespace common {

struct Error : std::runtime_error {
    explicit Error(const std::string& message) : std::runtime_error(message) {}
};

}
