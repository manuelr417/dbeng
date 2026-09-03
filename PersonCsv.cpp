#include "PersonCsv.h"

#include <charconv>
#include <fstream>
#include <sstream>

namespace person_csv {
namespace {

bool parse_integer(const std::string& text, int& value) {
    if (text.empty()) {
        return false;
    }
    const char* first = text.data();
    const char* last = first + text.size();
    const auto result = std::from_chars(first, last, value);
    return result.ec == std::errc{} && result.ptr == last;
}

bool parse_line(const std::string& line, Person& person, std::string& error) {
    std::stringstream input(line);
    std::string pid_text;
    std::string name;
    std::string age_text;
    std::string city;
    std::string extra;
    if (!std::getline(input, pid_text, ',') ||
        !std::getline(input, name, ',') ||
        !std::getline(input, age_text, ',') ||
        !std::getline(input, city, ',') ||
        std::getline(input, extra, ',')) {
        error = "expected exactly four comma-separated fields";
        return false;
    }

    int pid = 0;
    int age = 0;
    if (!parse_integer(pid_text, pid) || pid <= 0) {
        error = "pid must be a positive integer";
        return false;
    }
    if (!parse_integer(age_text, age) || age < 0) {
        error = "age must be a nonnegative integer";
        return false;
    }
    if (name.size() > 9) {
        error = "name must contain at most 9 characters";
        return false;
    }
    if (city.size() > 2) {
        error = "city must contain at most 2 characters";
        return false;
    }

    person = Person{};
    person.pid = pid;
    person.age = age;
    name.copy(person.name, name.size());
    city.copy(person.city, city.size());
    return true;
}

}

LoadResult load(const std::string& path, std::ostream& diagnostics) {
    LoadResult result;
    std::ifstream input(path);
    if (!input) {
        diagnostics << "cannot open CSV file: " << path << '\n';
        result.skipped = 1;
        return result;
    }

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty()) {
            ++result.skipped;
            diagnostics << "line " << line_number << ": blank line\n";
            continue;
        }
        Person person{};
        std::string error;
        if (!parse_line(line, person, error)) {
            ++result.skipped;
            diagnostics << "line " << line_number << ": " << error << '\n';
            continue;
        }
        result.people.push_back(person);
    }
    return result;
}

}
