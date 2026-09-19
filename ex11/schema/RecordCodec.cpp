#include "schema/RecordCodec.h"

#include "schema/ColumnSchema.h"
#include "tuple/Column.h"

#include <cstring>
#include <string>
#include <vector>

namespace schema {

namespace {

static_assert(sizeof(int) == 4 && sizeof(double) == 8,
              "the fixed-width record layout assumes 4-byte ints and "
              "8-byte doubles");

// Width of one column in a record: natural size for numerics, the
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

// One tuple position's place in the record: which schema column lives
// there, at what offset, how wide.
struct RecordSlot {
    std::size_t index;  // column_info index of the column at this position
    std::size_t offset;
    std::size_t width;
};

// The record layout of `schema`, one slot per tuple position in position
// order (heap.md section 3). False with a named error when a String
// column has no declared width — records need catalog-loaded schemas.
bool record_layout(const TableSchema& schema, std::vector<RecordSlot>& slots,
                   std::string& error) {
    error.clear();
    slots.clear();
    std::size_t offset = 0;
    for (std::size_t p = 0; p < schema.column_count(); ++p) {
        bool found = false;
        for (std::size_t i = 0; i < schema.column_count(); ++i) {
            const ColumnSchema& column = schema.column_info(i);
            if (column.column_position() != p) {
                continue;
            }
            const std::size_t width = column_width(column);
            if (width == 0) {
                error = "column '" + column.column_name() +
                        "' has no declared width; record packing needs "
                        "catalog-loaded schemas";
                return false;
            }
            slots.push_back(RecordSlot{i, offset, width});
            offset += width;
            found = true;
            break;
        }
        if (!found) {
            error = "schema column positions are not a permutation of 0.." +
                    std::to_string(schema.column_count() - 1);
            return false;
        }
    }
    return true;
}

}

std::size_t record_size(const TableSchema& schema) {
    std::vector<RecordSlot> slots;
    std::string ignored;
    if (!record_layout(schema, slots, ignored)) {
        return 0;
    }
    std::size_t total = 0;
    for (const RecordSlot& slot : slots) {
        total += slot.width;
    }
    return total;
}

bool pack(const TableSchema& schema, const tuple::Tuple& tuple,
          char* buffer, std::size_t max_len, std::string& error) {
    error.clear();
    // validate covers arity, per-position types, string widths, and NUL;
    // its error carries the column name.
    if (!schema.validate(tuple, error)) {
        return false;
    }
    std::vector<RecordSlot> slots;
    if (!record_layout(schema, slots, error)) {
        return false;
    }
    std::size_t total = 0;
    for (const RecordSlot& slot : slots) {
        total += slot.width;
    }
    if (max_len < total) {
        error = "record needs " + std::to_string(total) +
                " bytes, buffer holds " + std::to_string(max_len);
        return false;
    }
    for (const RecordSlot& slot : slots) {
        const tuple::Column& column = tuple.column(slot.index);
        char* field = buffer + slot.offset;
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
                std::memset(field + value.size(), 0, slot.width - value.size());
                break;
            }
        }
    }
    return true;
}

bool unpack(const TableSchema& schema, const char* buffer,
            std::size_t max_len, tuple::Tuple& tuple, std::string& error) {
    error.clear();
    tuple.clear();
    std::vector<RecordSlot> slots;
    if (!record_layout(schema, slots, error)) {
        return false; // the out tuple stays empty
    }
    std::size_t total = 0;
    for (const RecordSlot& slot : slots) {
        total += slot.width;
    }
    if (max_len < total) {
        error = "record needs " + std::to_string(total) +
                " bytes, buffer holds " + std::to_string(max_len);
        return false;
    }
    tuple::Tuple out;
    for (const RecordSlot& slot : slots) {
        const ColumnSchema& column = schema.column_info(slot.index);
        const char* field = buffer + slot.offset;
        switch (column.column_type()) {
            case tuple::ColumnType::Integer: {
                int value = 0;
                std::memcpy(&value, field, 4);
                out.add(value);
                break;
            }
            case tuple::ColumnType::Double: {
                double value = 0.0;
                std::memcpy(&value, field, 8);
                out.add(value);
                break;
            }
            case tuple::ColumnType::String: {
                // Value bytes up to the first NUL; everything behind it
                // must be padding.
                std::size_t length = 0;
                while (length < slot.width && field[length] != '\0') {
                    ++length;
                }
                bool padding_clean = true;
                for (std::size_t i = length; i < slot.width; ++i) {
                    if (field[i] != '\0') {
                        padding_clean = false;
                    }
                }
                if (!padding_clean) {
                    error = "column '" + column.column_name() +
                            "': padding contains garbage";
                    return false;
                }
                out.add(std::string(field, length));
                break;
            }
        }
    }
    tuple = out;
    return true;
}

}
