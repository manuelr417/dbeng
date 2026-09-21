#pragma once

#include "Ast.h"

#include <string>

namespace sql {

// Renders a statement as a terminal drawing: the statement outline with
// box-drawing branches (one per populated clause, absent clauses
// omitted) and every composite expression drawn top-down under the
// clause that owns it — root label, branch row, children. Leaves are
// compact one-liners that join their clause line. Drawing cannot fail
// (the tree is already valid), so there is no error convention here.
std::string draw(const Statement& statement);

}
