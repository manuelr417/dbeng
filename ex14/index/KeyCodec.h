#pragma once

// KeyCodec — the one encoding of index keys. A key is the value of the
// indexed column, written into a fixed-width byte string: 4 bytes for an
// Integer, 8 for a Double, the catalog's declared width for a String
// (value bytes, then NUL padding — the same shape record bytes use). One
// codec instance is bound to one column's type and width; comparisons
// are total on the encoded bytes per instance, so pages can binary-search
// raw bytes through the codec without decoding twice.

#include "schema/ColumnSchema.h"
#include "tuple/Column.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace isam {

// The on-disk key type tag stored in the header block.
enum class KeyType : std::uint32_t {
    Integer = 0,
    Double = 1,
    String = 2,
};

KeyType key_type_of(tuple::ColumnType type);

class KeyCodec {
public:
    KeyCodec() = default;
    KeyCodec(tuple::ColumnType type, std::size_t string_width);

    bool valid() const { return width_ != 0; }
    std::size_t width() const { return width_; }
    tuple::ColumnType column_type() const { return type_; }
    KeyType key_type() const { return key_type_of(type_); }

    // Encodes `value` into `out` (exactly width() bytes). Fails with a
    // named error when the value's type differs from the key type, or a
    // String is wider than the declared width or contains NUL.
    bool encode(const tuple::Column& value, char* out,
                std::string& error) const;

    // Decodes width() bytes back into a Column of the key type.
    tuple::Column decode(const char* in) const;

    // Total order over encoded keys: numeric for Integer/Double,
    // lexicographic bytes for String (NUL padding makes byte order equal
    // value order for NUL-free values). Negative when a < b.
    int compare(const char* a, const char* b) const;

    // A readable rendering for error messages and verify findings;
    // strings are quoted.
    std::string display(const char* in) const;

private:
    tuple::ColumnType type_ = tuple::ColumnType::Integer;
    std::size_t width_ = 0;
};

}
