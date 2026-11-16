#pragma once

#include "Token.h"

#include <string>
#include <vector>

namespace sql {

// Scans query text into a token stream; the stream always ends with an
// End token. Errors are values: false return plus a "line L: message"
// in error; tokens is cleared on entry and left empty on failure.
bool tokenize(const std::string& source, std::vector<Token>& tokens, std::string& error);

}
