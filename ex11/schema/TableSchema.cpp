#include "schema/TableSchema.h"

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

bool serialize(const TableSchema& schema, const tuple::Tuple& tuple,
               char* buffer, std::size_t max_len, std::string& error) {
    if (!schema.validate(tuple, error)) {
        return false;
    }
    // Wire position p takes the column whose column_info index is
    // column_order[p]; that column's value lives at tuple slot
    // column_position.
    tuple::Tuple wire;
    const std::vector<std::size_t>& order = schema.column_order();
    for (std::size_t p = 0; p < order.size(); ++p) {
        const ColumnSchema& meta = schema.column_info(order[p]);
        wire.add_column(tuple.column(meta.column_position()));
    }
    return tuple::serialize(wire, buffer, max_len, error);
}

bool deserialize(const TableSchema& schema, const char* buffer,
                 std::size_t max_len, tuple::Tuple& tuple, std::string& error) {
    tuple.clear();
    tuple::Tuple wire;
    if (!tuple::deserialize(buffer, max_len, wire, error)) {
        return false;
    }
    // Wire position q holds the column column_info[order[q]]; its value
    // belongs at logical slot column_position.
    const std::vector<std::size_t>& order = schema.column_order();
    std::vector<tuple::Column> slots(order.size());
    for (std::size_t q = 0; q < order.size(); ++q) {
        const ColumnSchema& meta = schema.column_info(order[q]);
        slots[meta.column_position()] = wire.column(q);
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

}
