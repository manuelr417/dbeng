#include "schema/TupleSerializer.h"

#include "common/Error.h"

#include <cstring>
#include <format>

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

std::string TupleSerializer::serialize(const tuple::Tuple& tuple) const {
    // validate covers arity, per-position types, string widths, and NUL;
    // its error carries the column name. Failing here means no record is
    // produced.
    schema_.validate(tuple);
    std::string record(record_size(), '\0');
    // Column i is wire position i and tuple slot i: creation order is the
    // one order everywhere. Each column's payload lands at the running
    // offset; the schema is the layout, so nothing structural is written.
    std::size_t cursor = 0;
    for (std::size_t i = 0; i < schema_.column_count(); ++i) {
        const ColumnSchema& meta = schema_.column_info(i);
        const std::size_t width = column_width(meta);
        if (width == 0) {
            throw common::Error(std::format(
                "column '{}' has no declared width; record storage needs "
                "catalog-loaded schemas",
                meta.column_name()));
        }
        char* field = record.data() + cursor;
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
    return record;
}

void TupleSerializer::deserialize(const char* data, std::size_t length,
                                  tuple::Tuple& tuple) const {
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
            throw common::Error(std::format(
                "column '{}' has no declared width; record storage needs "
                "catalog-loaded schemas",
                meta.column_name()));
        }
        if (length < cursor + width) {
            throw common::Error(
                std::format("record needs {} bytes, buffer holds {}",
                            cursor + width, length));
        }
        const char* field = data + cursor;
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
                std::size_t value_length = 0;
                while (value_length < width && field[value_length] != '\0') {
                    ++value_length;
                }
                for (std::size_t k = value_length; k < width; ++k) {
                    if (field[k] != '\0') {
                        throw common::Error(std::format(
                            "column '{}': padding contains garbage",
                            meta.column_name()));
                    }
                }
                logical.add_column(
                    tuple::Column(std::string(field, value_length)));
                break;
            }
        }
    }
    schema_.validate(logical);
    tuple = logical;
}

}
