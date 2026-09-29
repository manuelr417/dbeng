#include "schema/TableSchema.h"

#include <cstring>
#include <stdexcept>

namespace schema {

namespace {

const char* type_name(tuple::ColumnType type) {
    switch (type) {
        case tuple::ColumnType::Integer: return "Integer";
        case tuple::ColumnType::String:  return "String";
        case tuple::ColumnType::Double:  return "Double";
    }
    return "?";
}

tuple::Column default_column(tuple::ColumnType type) {
    switch (type) {
        case tuple::ColumnType::Integer: return tuple::Column(0);
        case tuple::ColumnType::String:  return tuple::Column(std::string());
        case tuple::ColumnType::Double:  return tuple::Column(0.0);
    }
    return tuple::Column();
}

// The width of one column in a record: natural size for numerics, the
// declared width for Strings. 0 flags an undeclared String, which cannot
// live in a record.
std::size_t column_width(const ColumnSchema& column) {
    switch (column.column_type()) {
        case tuple::ColumnType::Integer: return 4;
        case tuple::ColumnType::Double:  return 8;
        case tuple::ColumnType::String:  return column.column_size();
    }
    return 0;
}

}

TableSchema::TableSchema(const std::string& table_name)
    : table_name_(table_name) {}

bool TableSchema::add_column(const std::string& name, tuple::ColumnType type,
                             std::string& error) {
    error.clear();
    if (name.empty()) {
        error = "column name is empty";
        return false;
    }
    if (column_name_map_.find(name) != column_name_map_.end()) {
        error = "column '" + name + "' already exists";
        return false;
    }
    const std::size_t index = column_info_.size();
    column_info_.emplace_back(name, type, index); // position = next free slot
    column_name_map_.emplace(name, index);
    column_order_.push_back(index); // identity until set_column_order
    return true;
}

bool TableSchema::set_column_order(const std::vector<std::size_t>& order,
                                   std::string& error) {
    error.clear();
    if (order.size() != column_info_.size()) {
        error = "column order must name all " +
                std::to_string(column_info_.size()) + " columns";
        return false;
    }
    std::vector<bool> seen(order.size(), false);
    for (const std::size_t index : order) {
        if (index >= order.size() || seen[index]) {
            error = "column order is not a permutation of 0.." +
                    std::to_string(order.size() - 1);
            return false;
        }
        seen[index] = true;
    }
    column_order_ = order;
    return true;
}

const ColumnSchema& TableSchema::column_info(std::size_t index) const {
    if (index >= column_info_.size()) {
        throw std::out_of_range("schema column index out of range");
    }
    return column_info_[index];
}

std::optional<std::size_t> TableSchema::column_index(
    const std::string& name) const {
    const auto it = column_name_map_.find(name);
    if (it == column_name_map_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool TableSchema::validate(const tuple::Tuple& tuple, std::string& error) const {
    error.clear();
    if (tuple.column_count() != column_info_.size()) {
        error = "tuple has " + std::to_string(tuple.column_count()) +
                " columns, schema has " + std::to_string(column_info_.size());
        return false;
    }
    // Tuple slot p must hold the type of the schema column whose
    // column_position is p; the positions form a permutation, so walking the
    // columns visits every slot exactly once.
    for (const ColumnSchema& column : column_info_) {
        const tuple::Column& value = tuple.column(column.column_position());
        if (value.column_type() != column.column_type()) {
            error = "column " + std::to_string(column.column_position()) +
                    ": tuple has " + type_name(value.column_type()) +
                    ", schema says " + type_name(column.column_type());
            return false;
        }
        // Fixed-width record storage (heap.md section 3) makes these two
        // checks essential: the packer writes exactly column_size bytes for
        // a String. Width is enforced only where a width is declared
        // (column_size > 0); code-built schemas stay unbounded.
        if (value.column_type() == tuple::ColumnType::String) {
            const std::string& text =
                std::get<std::string>(value.column_value());
            if (text.find('\0') != std::string::npos) {
                error = "column '" + column.column_name() +
                        "': strings must not contain NUL";
                return false;
            }
            if (column.column_size() > 0 &&
                text.size() > column.column_size()) {
                error = "column '" + column.column_name() + "': value of " +
                        std::to_string(text.size()) +
                        " bytes exceeds the declared width of " +
                        std::to_string(column.column_size());
                return false;
            }
        }
    }
    return true;
}

tuple::Tuple TableSchema::make_tuple() const {
    // One slot per tuple position, filled by routing through
    // column_position — never through the column_info index.
    std::vector<tuple::Column> slots(column_info_.size());
    for (const ColumnSchema& column : column_info_) {
        slots[column.column_position()] = default_column(column.column_type());
    }
    tuple::Tuple tuple;
    for (const tuple::Column& column : slots) {
        tuple.add_column(column);
    }
    return tuple;
}

bool serialize(const tuple::Tuple& tuple, const TableSchema& schema,
               char* buffer, std::size_t max_len, std::string& error) {
    error.clear();
    // validate covers arity, per-position types, string widths, and NUL;
    // its error carries the column name. Failing here means the buffer is
    // never touched.
    if (!schema.validate(tuple, error)) {
        return false;
    }
    // Every width must be declared before the first byte moves: a refusal
    // never leaves a half-written record behind.
    for (std::size_t i = 0; i < schema.column_count(); ++i) {
        if (column_width(schema.column_info(i)) == 0) {
            const ColumnSchema& meta = schema.column_info(i);
            error = "column '" + meta.column_name() +
                    "' has no declared width; record storage needs "
                    "catalog-loaded schemas";
            return false;
        }
    }
    std::size_t cursor = 0;
    // Wire position p takes the column whose column_info index is
    // column_order[p]; that column's value lives at tuple slot
    // column_position. Each column's payload lands at the running offset;
    // the schema is the layout, so nothing structural is written.
    for (std::size_t p = 0; p < schema.column_count(); ++p) {
        const ColumnSchema& meta = schema.column_info(schema.column_order()[p]);
        const std::size_t width = column_width(meta);
        if (width == 0) {
            error = "column '" + meta.column_name() +
                    "' has no declared width; record storage needs "
                    "catalog-loaded schemas";
            return false;
        }
        if (max_len < cursor + width) {
            error = "record needs " + std::to_string(cursor + width) +
                    " bytes, buffer holds " + std::to_string(max_len);
            return false;
        }
        char* field = buffer + cursor;
        const tuple::Column& column = tuple.column(meta.column_position());
        switch (column.column_type()) {
            case tuple::ColumnType::Integer: {
                const int value = std::get<int>(column.column_value());
                std::memcpy(field, &value, 4);
                break;
            }
            case tuple::ColumnType::Double: {
                const double value = std::get<double>(column.column_value());
                std::memcpy(field, &value, 8);
                break;
            }
            case tuple::ColumnType::String: {
                const std::string& value =
                    std::get<std::string>(column.column_value());
                std::memcpy(field, value.data(), value.size());
                std::memset(field + value.size(), 0, width - value.size());
                break;
            }
        }
        cursor += width;
    }
    return true;
}

bool deserialize(const TableSchema& schema, const char* buffer,
                 std::size_t max_len, tuple::Tuple& tuple, std::string& error) {
    error.clear();
    tuple.clear();
    std::vector<tuple::Column> slots(schema.column_count());
    std::size_t cursor = 0;
    for (std::size_t p = 0; p < schema.column_count(); ++p) {
        const ColumnSchema& meta = schema.column_info(schema.column_order()[p]);
        const std::size_t width = column_width(meta);
        if (width == 0) {
            error = "column '" + meta.column_name() +
                    "' has no declared width; record storage needs "
                    "catalog-loaded schemas";
            return false; // the out tuple stays empty
        }
        if (max_len < cursor + width) {
            error = "record needs " + std::to_string(cursor + width) +
                    " bytes, buffer holds " + std::to_string(max_len);
            return false; // the out tuple stays empty
        }
        const char* field = buffer + cursor;
        cursor += width;
        switch (meta.column_type()) {
            case tuple::ColumnType::Integer: {
                int value = 0;
                std::memcpy(&value, field, 4);
                slots[meta.column_position()] = tuple::Column(value);
                break;
            }
            case tuple::ColumnType::Double: {
                double value = 0.0;
                std::memcpy(&value, field, 8);
                slots[meta.column_position()] = tuple::Column(value);
                break;
            }
            case tuple::ColumnType::String: {
                // Value bytes up to the first NUL; everything behind it
                // must be padding. A value filling the whole width has no
                // NUL and no padding — both are fine.
                std::size_t length = 0;
                while (length < width && field[length] != '\0') {
                    ++length;
                }
                for (std::size_t i = length; i < width; ++i) {
                    if (field[i] != '\0') {
                        error = "column '" + meta.column_name() +
                                "': padding contains garbage";
                        return false; // the out tuple stays empty
                    }
                }
                slots[meta.column_position()] =
                    tuple::Column(std::string(field, length));
                break;
            }
        }
    }
    tuple::Tuple logical;
    for (const tuple::Column& column : slots) {
        logical.add_column(column);
    }
    if (!schema.validate(logical, error)) {
        return false; // the out tuple stays empty
    }
    tuple = logical;
    return true;
}

std::size_t record_size(const TableSchema& schema) {
    if (schema.column_count() == 0) {
        return 0;
    }
    std::size_t total = 0;
    for (std::size_t i = 0; i < schema.column_count(); ++i) {
        const std::size_t width = column_width(schema.column_info(i));
        if (width == 0) {
            return 0; // an undeclared-width String has no record layout
        }
        total += width;
    }
    return total;
}

}
