#include "exec/Value.h"

#include <charconv>

namespace exec {

const char* type_name(Type type) {
    switch (type) {
        case Type::Int: return "integer";
        case Type::Double: return "double";
        case Type::String: return "string";
        case Type::Bool: return "boolean";
    }
    return "";
}

std::string display(const Value& value) {
    if (const int* v = std::get_if<int>(&value)) {
        return std::to_string(*v);
    }
    if (const double* v = std::get_if<double>(&value)) {
        char buffer[64];
        const auto result =
            std::to_chars(buffer, buffer + sizeof(buffer), *v);
        return std::string(buffer, result.ptr);
    }
    return std::get<std::string>(value);
}

int compare_values(const Value& a, const Value& b) {
    const bool a_string = std::holds_alternative<std::string>(a);
    const bool b_string = std::holds_alternative<std::string>(b);
    if (a_string != b_string) {
        return 2;  // incomparable: the binder refuses these before runtime
    }
    if (a_string) {
        const std::string& x = std::get<std::string>(a);
        const std::string& y = std::get<std::string>(b);
        return (x > y) - (x < y);
    }
    const double x = std::holds_alternative<double>(a)
                         ? std::get<double>(a)
                         : static_cast<double>(std::get<int>(a));
    const double y = std::holds_alternative<double>(b)
                         ? std::get<double>(b)
                         : static_cast<double>(std::get<int>(b));
    return (x > y) - (x < y);
}

bool promotes_to_double(const Value& a, const Value& b) {
    return std::holds_alternative<double>(a) ||
           std::holds_alternative<double>(b);
}

}
