#include "schema/TupleSerializer.h"

#include <cstring>

namespace schema {

namespace {

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

TupleSerializer::TupleSerializer(const TableSchema& schema)
    : schema_(schema) {}

std::size_t TupleSerializer::record_size() const {
    if (schema_.column_count() == 0) {
        return 0;
    }
    std::size_t total = 0;
    for (std::size_t i = 0; i < schema_.column_count(); ++i) {
        const std::size_t width = column_width(schema_.column_info(i));
        if (width == 0) {
            return 0; // an undeclared-width String has no record layout
        }
        total += width;
    }
    return total;
}

bool TupleSerializer::serialize(const tuple::Tuple& tuple, char* buffer,
                                std::size_t max_len,
                                std::string& error) const {
    error.clear();
    // validate covers arity, per-position types, string widths, and NUL;
    // its error carries the column name. Failing here means the buffer is
    // never touched.
    if (!schema_.validate(tuple, error)) {
        return false;
    }
    // Every width must be declared before the first byte moves: a refusal
    // never leaves a half-written record behind.
    for (std::size_t i = 0; i < schema_.column_count(); ++i) {
        if (column_width(schema_.column_info(i)) == 0) {
            const ColumnSchema& meta = schema_.column_info(i);
            error = "column '" + meta.column_name() +
                    "' has no declared width; record storage needs "
                    "catalog-loaded schemas";
            return false;
        }
    }
    std::size_t cursor = 0;
    // Column i is wire position i and tuple slot i: creation order is the
    // one order everywhere. Each column's payload lands at the running
    // offset; the schema is the layout, so nothing structural is written.
    for (std::size_t i = 0; i < schema_.column_count(); ++i) {
        const ColumnSchema& meta = schema_.column_info(i);
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
        const tuple::Column& column = tuple.column(i);
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

bool TupleSerializer::deserialize(const char* buffer, std::size_t max_len,
                                  tuple::Tuple& tuple,
                                  std::string& error) const {
    error.clear();
    tuple.clear();
    // Column i is read from wire position i into tuple slot i: creation
    // order is the one order everywhere. The logical tuple is built
    // locally and assigned only on success, so the out tuple stays empty
    // on any failure.
    tuple::Tuple logical;
    std::size_t cursor = 0;
    for (std::size_t i = 0; i < schema_.column_count(); ++i) {
        const ColumnSchema& meta = schema_.column_info(i);
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
                logical.add_column(tuple::Column(value));
                break;
            }
            case tuple::ColumnType::Double: {
                double value = 0.0;
                std::memcpy(&value, field, 8);
                logical.add_column(tuple::Column(value));
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
                for (std::size_t k = length; k < width; ++k) {
                    if (field[k] != '\0') {
                        error = "column '" + meta.column_name() +
                                "': padding contains garbage";
                        return false; // the out tuple stays empty
                    }
                }
                logical.add_column(tuple::Column(std::string(field, length)));
                break;
            }
        }
    }
    if (!schema_.validate(logical, error)) {
        return false; // the out tuple stays empty
    }
    tuple = logical;
    return true;
}

}
