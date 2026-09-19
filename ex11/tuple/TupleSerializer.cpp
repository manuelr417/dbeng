#include "tuple/TupleSerializer.h"

#include "tuple/Column.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace tuple {

namespace {

// The wire format is single-host, native endian (see tuple.md, section 6);
// these static asserts pin the size assumptions the memcpy encoding makes.
static_assert(sizeof(int) == 4, "int columns serialize as 4 bytes");
static_assert(sizeof(double) == 8, "double columns serialize as 8 bytes");

constexpr std::size_t kCountSize = 4;         // tuple header: column count
constexpr std::size_t kColumnHeaderSize = 8;  // type tag + length, 4 bytes each

void put_int32(char* dst, std::int32_t value) {
    std::memcpy(dst, &value, sizeof(value));
}

std::int32_t get_int32(const char* src) {
    std::int32_t value;
    std::memcpy(&value, src, sizeof(value));
    return value;
}

// The payload size implied by the value itself — the mirror that
// column_length must equal for serialize() to accept the column.
std::size_t value_payload_size(const Column& column) {
    switch (column.column_value().index()) {
        case 0: return sizeof(int);                                         // int
        case 1: return std::get<std::string>(column.column_value()).size(); // string
        default: return sizeof(double);                                     // double
    }
}

}

std::size_t serialized_size(const Tuple& tuple) {
    std::size_t size = kCountSize;
    for (std::size_t i = 0; i < tuple.column_count(); ++i) {
        size += kColumnHeaderSize + tuple.column(i).column_length();
    }
    return size;
}

bool serialize(const Tuple& tuple, char* buffer, std::size_t max_len,
               std::string& error) {
    error.clear();

    // The constructors make a broken type/length mirror unrepresentable, but
    // this is the boundary where bytes leave — so the mirror is re-checked
    // here as defense in depth.
    for (std::size_t i = 0; i < tuple.column_count(); ++i) {
        const Column& column = tuple.column(i);
        if (static_cast<int>(column.column_type()) !=
            static_cast<int>(column.column_value().index())) {
            error = "column " + std::to_string(i) +
                    ": type does not match the value";
            return false;
        }
        if (column.column_length() != value_payload_size(column)) {
            error = "column " + std::to_string(i) +
                    ": length does not match the value";
            return false;
        }
    }

    const std::size_t need = serialized_size(tuple);
    if (max_len < need) {
        error = "buffer too small: need " + std::to_string(need) +
                " bytes, have " + std::to_string(max_len);
        return false;
    }

    put_int32(buffer, static_cast<std::int32_t>(tuple.column_count()));
    char* cursor = buffer + kCountSize;
    for (std::size_t i = 0; i < tuple.column_count(); ++i) {
        const Column& column = tuple.column(i);
        put_int32(cursor, static_cast<std::int32_t>(column.column_type()));
        put_int32(cursor + 4, static_cast<std::int32_t>(column.column_length()));
        cursor += kColumnHeaderSize;
        switch (static_cast<ColumnType>(column.column_value().index())) {
            case ColumnType::Integer: {
                const int value = std::get<int>(column.column_value());
                std::memcpy(cursor, &value, sizeof(value));
                cursor += sizeof(value);
                break;
            }
            case ColumnType::String: {
                const std::string& value =
                    std::get<std::string>(column.column_value());
                std::memcpy(cursor, value.data(), value.size());
                cursor += value.size();
                break;
            }
            case ColumnType::Double: {
                const double value = std::get<double>(column.column_value());
                std::memcpy(cursor, &value, sizeof(value));
                cursor += sizeof(value);
                break;
            }
        }
    }
    return true;
}

bool deserialize(const char* buffer, std::size_t max_len, Tuple& tuple,
                 std::string& error) {
    error.clear();
    tuple.clear();

    // Every failure funnels through here so the target tuple comes back
    // empty, as documented.
    auto fail = [&](const std::string& message) {
        error = message;
        tuple.clear();
        return false;
    };

    if (max_len < kCountSize) {
        return fail("buffer too small for the column count");
    }
    const std::int32_t count = get_int32(buffer);
    if (count < 0) {
        return fail("negative column count " + std::to_string(count));
    }

    std::size_t cursor = kCountSize;
    for (std::int32_t i = 0; i < count; ++i) {
        if (max_len < cursor + kColumnHeaderSize) {
            return fail("column " + std::to_string(i) + " truncated: header");
        }
        const std::int32_t tag = get_int32(buffer + cursor);
        const std::int32_t length = get_int32(buffer + cursor + 4);
        cursor += kColumnHeaderSize;

        if (tag < 0 || tag > static_cast<std::int32_t>(ColumnType::Double)) {
            return fail("unknown column type " + std::to_string(tag) +
                        " in column " + std::to_string(i));
        }
        const ColumnType type = static_cast<ColumnType>(tag);

        const auto fixed = fixed_payload_size(type);
        if (fixed && length != static_cast<std::int32_t>(*fixed)) {
            return fail("column " + std::to_string(i) + ": length " +
                        std::to_string(length) + " does not match its type");
        }
        if (!fixed && length < 0) {
            return fail("column " + std::to_string(i) + ": negative length " +
                        std::to_string(length));
        }
        if (max_len < cursor + static_cast<std::size_t>(length)) {
            return fail("column " + std::to_string(i) + " truncated: payload");
        }

        switch (type) {
            case ColumnType::Integer: {
                int value;
                std::memcpy(&value, buffer + cursor, sizeof(value));
                tuple.add(value);
                cursor += sizeof(value);
                break;
            }
            case ColumnType::String: {
                tuple.add(std::string(buffer + cursor,
                                      static_cast<std::size_t>(length)));
                cursor += static_cast<std::size_t>(length);
                break;
            }
            case ColumnType::Double: {
                double value;
                std::memcpy(&value, buffer + cursor, sizeof(value));
                tuple.add(value);
                cursor += sizeof(value);
                break;
            }
        }
    }
    return true;
}

}
