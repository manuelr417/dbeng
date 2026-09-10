#include "PersonGenerator.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace bufman {
namespace {

constexpr std::array<const char*, 8> kCityCodes = {
    "PR", "NY", "LA", "TX", "FL", "CA", "WA", "OH"
};

}

std::vector<Person> generate(std::size_t count, int first_pid) {
    std::vector<Person> people;
    people.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        Person person{};
        person.pid = first_pid + static_cast<int>(i);
        person.age = 18 + (person.pid % 63);

        const std::string name = "P" + std::to_string(person.pid);
        name.copy(person.name, std::min(name.size(), sizeof(person.name) - 1));

        const char* city = kCityCodes[i % kCityCodes.size()];
        std::memcpy(person.city, city, 2);

        people.push_back(person);
    }
    return people;
}

}
