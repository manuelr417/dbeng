#include "sql/AstDraw.h"
#include "sql/Parser.h"

#include <iostream>
#include <string>

// Reads one line of SQL per prompt (a line may hold several ';'-separated
// statements), parses it, and prints each AST as a top-down drawing:
// the statement outline with the expressions that hang off its clauses.
// Parse errors print "error: line L: ..." and the session continues;
// "quit" (or end of input) ends it.
int main() {
    std::string line;
    while (std::cout << "sql> " && std::getline(std::cin, line)) {
        if (line == "quit") break;
        sql::ParseResult result;
        std::string error;
        if (!sql::parse(line, result, error)) {
            std::cout << "error: " << error << "\n";
            continue;
        }
        for (const auto& statement : result.statements) {
            std::cout << sql::draw(*statement) << "\n\n";
        }
    }
    return 0;
}
