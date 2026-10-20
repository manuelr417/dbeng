#ifndef EX11_COLUMN_H
#define EX11_COLUMN_H

#include <cstddef>
#include <optional>
#include <string>
#include <variant>

namespace tuple {

// The data type of a column, mirroring the alternatives of ColumnValue below
// in the same order, so the wire tag equals column_value.index() by
// construction.
enum class ColumnType : int {
    Integer = 0, // payload: the int, 4 bytes
    String  = 1, // payload: the characters, column_length bytes
    Double  = 2, // payload: the double, 8 bytes
};

// The value of one column: an int, a string, or a double. std::variant is a
// tagged union — exactly one alternative is active, and .index() reports
// which (0 = int, 1 = string, 2 = double).
using ColumnValue = std::variant<int, std::string, double>;

// Payload byte count of the fixed-size types; nullopt for String, whose
// length comes from the string itself. The one place that knows the sizes.
inline std::optional<std::size_t> fixed_payload_size(ColumnType type) {
    switch (type) {
        case ColumnType::Integer: return sizeof(int);
        case ColumnType::Double:  return sizeof(double);
        case ColumnType::String:  return std::nullopt;
    }
    return std::nullopt;
}

/// One column of a tuple: a value plus the two descriptions stored next to
/// it (its type and its payload length in bytes). The constructors are the
/// only writers, so column_type and column_length always mirror the value;
/// serialize() re-checks that mirror at the boundary anyway.
class Column {
public:
    Column()                                   // Integer, value 0, length 4
        : column_value_(0),
          column_type_(ColumnType::Integer),
          column_length_(sizeof(int)) {}

    explicit Column(int value)                 // length 4
        : column_value_(value),
          column_type_(ColumnType::Integer),
          column_length_(sizeof(int)) {}

    explicit Column(double value)              // length 8
        : column_value_(value),
          column_type_(ColumnType::Double),
          column_length_(sizeof(double)) {}

    explicit Column(const std::string& value)  // length = value.size()
        : column_value_(value),
          column_type_(ColumnType::String),
          column_length_(value.size()) {}

    const ColumnValue& column_value() const { return column_value_; }
    ColumnType column_type() const { return column_type_; }
    std::size_t column_length() const { return column_length_; }

    friend bool operator==(const Column& a, const Column& b) {
        return a.column_value_ == b.column_value_ &&
               a.column_type_ == b.column_type_ &&
               a.column_length_ == b.column_length_;
    }

private:
    ColumnValue column_value_;
    ColumnType column_type_;
    std::size_t column_length_; // payload bytes only, never the header
};

}

#endif // EX11_COLUMN_H
