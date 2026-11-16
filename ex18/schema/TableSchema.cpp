#include "schema/TableSchema.h"

#include "common/Error.h"

#include <cstring>
#include <format>
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

}

TableSchema::TableSchema(const std::string& table_name)
    : table_name_(table_name) {}

void TableSchema::add_column(const std::string& name, tuple::ColumnType type) {
    if (name.empty()) {
        throw common::Error("column name is empty");
    }
    if (column_name_map_.find(name) != column_name_map_.end()) {
        throw common::Error(std::format("column '{}' already exists", name));
    }
    const std::size_t index = column_info_.size();
    column_info_.emplace_back(name, type); // creation order = file order
    column_name_map_.emplace(name, index);
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

void TableSchema::validate(const tuple::Tuple& tuple) const {
    if (tuple.column_count() != column_info_.size()) {
        throw common::Error(
            std::format("tuple has {} columns, schema has {}",
                        tuple.column_count(), column_info_.size()));
    }
    // Tuple slot i holds the type of schema column i: creation order is
    // the one addressing everywhere, and the name lookup returns the same
    // index.
    for (std::size_t i = 0; i < column_info_.size(); ++i) {
        const ColumnSchema& column = column_info_[i];
        const tuple::Column& value = tuple.column(i);
        if (value.column_type() != column.column_type()) {
            throw common::Error(
                std::format("column {}: tuple has {}, schema says {}", i,
                            type_name(value.column_type()),
                            type_name(column.column_type())));
        }
        // Fixed-width record storage (heap.md section 3) makes these two
        // checks essential: the packer writes exactly column_size bytes for
        // a String. Width is enforced only where a width is declared
        // (column_size > 0); code-built schemas stay unbounded.
        if (value.column_type() == tuple::ColumnType::String) {
            const std::string& text =
                std::get<std::string>(value.column_value());
            if (text.find('\0') != std::string::npos) {
                throw common::Error(
                    std::format("column '{}': strings must not contain NUL",
                                column.column_name()));
            }
            if (column.column_size() > 0 &&
                text.size() > column.column_size()) {
                throw common::Error(
                    std::format("column '{}': value of {} bytes exceeds the "
                                "declared width of {}",
                                column.column_name(), text.size(),
                                column.column_size()));
            }
        }
    }
}

tuple::Tuple TableSchema::make_tuple() const {
    // One default column per creation-order index, in order.
    tuple::Tuple tuple;
    for (const ColumnSchema& column : column_info_) {
        tuple.add_column(default_column(column.column_type()));
    }
    return tuple;
}

}
