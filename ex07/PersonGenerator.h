#ifndef EX07_PERSON_GENERATOR_H
#define EX07_PERSON_GENERATOR_H

#include "Person.h"

#include <cstddef>
#include <vector>

namespace person_generator {

// Generates `count` synthetic Person records for bulk-insert testing, with
// pids assigned sequentially starting at `first_pid`.
std::vector<Person> generate(std::size_t count, int first_pid);

}

#endif // EX07_PERSON_GENERATOR_H
