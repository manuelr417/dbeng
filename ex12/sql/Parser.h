#pragma once

#include "Ast.h"

#include <string>

namespace sql {

struct ParseResult {
    std::vector<std::unique_ptr<Statement>> statements;
};

// Parses a ';'-separated statement list. Errors are values: false return
// plus a "line L: message" in error; result.statements is cleared on
// entry and left empty on failure. The trailing ';' is optional and
// empty statements between semicolons are ignored.
bool parse(const std::string& query, ParseResult& result, std::string& error);

}
