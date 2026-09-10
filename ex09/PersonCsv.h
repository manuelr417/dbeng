#ifndef EX07_PERSON_CSV_H
#define EX07_PERSON_CSV_H

#include "Person.h"

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

namespace bufman {

struct LoadResult {
    std::vector<Person> people;
    std::size_t skipped = 0;
};

LoadResult load(const std::string& path, std::ostream& diagnostics);

}

#endif // EX07_PERSON_CSV_H
