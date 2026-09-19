#ifndef EX11_COLUMN_SCHEMA_H
#define EX11_COLUMN_SCHEMA_H

#include "tuple/Column.h"

#include <cstddef>
#include <string>

namespace schema {

/// Metadata of one column: its name, its type (reused from the tuple
/// layer), the slot it occupies in a schema-shaped tuple, and its declared
/// width (column_size, e.g. from a JSON catalog; 0 = undeclared). The
/// schema says what must be in a tuple and how wide; tuple::Column says
/// what is.
class ColumnSchema {
public:
    ColumnSchema()  // empty name, Integer, position 0, no declared size
        : column_name_(),
          column_type_(tuple::ColumnType::Integer),
          column_position_(0),
          column_size_(0) {}

    ColumnSchema(const std::string& column_name,
                 tuple::ColumnType column_type,
                 std::size_t column_position,
                 std::size_t column_size = 0)
        : column_name_(column_name),
          column_type_(column_type),
          column_position_(column_position),
          column_size_(column_size) {}

    const std::string& column_name() const { return column_name_; }
    tuple::ColumnType column_type() const { return column_type_; }
    std::size_t column_position() const { return column_position_; }
    std::size_t column_size() const { return column_size_; }

    /// Declared width of the column (a String's maximum character count,
    /// the byte width of Integer/Double). The JSON catalog loader is the
    /// only writer after construction; 0 means undeclared.
    void set_column_size(std::size_t column_size) { column_size_ = column_size; }

    friend bool operator==(const ColumnSchema& a, const ColumnSchema& b) {
        return a.column_name_ == b.column_name_ &&
               a.column_type_ == b.column_type_ &&
               a.column_position_ == b.column_position_ &&
               a.column_size_ == b.column_size_;
    }

private:
    std::string column_name_;
    tuple::ColumnType column_type_;
    std::size_t column_position_; // slot of this column in a schema tuple
    std::size_t column_size_;     // declared width; 0 = undeclared
};

}

#endif // EX11_COLUMN_SCHEMA_H
