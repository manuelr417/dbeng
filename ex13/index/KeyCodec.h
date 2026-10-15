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

// Maps a column type to its header key-type tag.
// `type` — the column type of the indexed column.
// Returns the matching KeyType (Integer as the fallback for unknown).
KeyType key_type_of(tuple::ColumnType type);

class KeyCodec {
public:
    // Default: an unusable codec (width 0) — valid() stays false until
    // a column binds one.
    KeyCodec() = default;

    // Binds the codec to one column's type and key width.
    // `type` — the column type of the indexed column.
    // `string_width` — the declared VARCHAR width, used only when
    // `type` is String (Integer keys are 4 bytes, Double keys 8).
    KeyCodec(tuple::ColumnType type, std::size_t string_width);

    // True when the codec has a usable key width — always for
    // Integer/Double, only when a String column declared its width.
    bool valid() const { return width_ != 0; }

    // The fixed key width in bytes: 4 (Integer), 8 (Double), or the
    // declared String width.
    std::size_t width() const { return width_; }

    // The column type this codec encodes and compares.
    tuple::ColumnType column_type() const { return type_; }

    // The header-block tag for the bound column type.
    KeyType key_type() const { return key_type_of(type_); }

    // Encodes `value` into fixed-width key bytes (exactly width()).
    // `value` — the column value to encode.
    // `out` — caller storage of at least width() bytes.
    // Returns true on success; false with `error` set when the codec is
    // unbound, the value's type differs from the key type, or a String
    // is wider than the declared width or contains NUL.
    bool encode(const tuple::Column& value, char* out,
                std::string& error) const;

    // Decodes width() bytes back into a Column of the key type.
    // `in` — encoded key bytes, at least width() long.
    // Returns the decoded Column (Strings run to the first NUL, at most
    // width() bytes); an empty Column for an unknown type.
    tuple::Column decode(const char* in) const;

    // Total order over encoded keys: numeric for Integer/Double,
    // lexicographic bytes for String (NUL padding makes byte order equal
    // value order for NUL-free values).
    // `a`, `b` — encoded keys, each width() bytes.
    // Returns negative when a < b, zero when equal, positive when
    // a > b (0 for an unknown type).
    int compare(const char* a, const char* b) const;

    // A readable rendering for error messages and verify findings.
    // `in` — encoded key bytes, at least width() long.
    // Returns the value as text; strings are quoted.
    std::string display(const char* in) const;

private:
    tuple::ColumnType type_ = tuple::ColumnType::Integer;
    std::size_t width_ = 0;
};

}
