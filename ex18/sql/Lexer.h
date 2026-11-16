#pragma once

#include "Token.h"

#include <string>
#include <vector>

namespace sql {

// Scans query text into a token stream; the stream always ends with an
// End token. Failures (a malformed number, an unterminated string, an
// unknown character) throw common::Error carrying "line L: message".
std::vector<Token> tokenize(const std::string& source);

}
