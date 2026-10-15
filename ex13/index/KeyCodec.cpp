#include "index/KeyCodec.h"

#include <cstring>

namespace isam {

// Maps a column type to its header key-type tag.
// `type` — the column type of the indexed column.
// Returns the matching KeyType; Integer as the fallback for unknown.
KeyType key_type_of(tuple::ColumnType type) {
    switch (type) {
        case tuple::ColumnType::Integer: return KeyType::Integer;
        case tuple::ColumnType::Double: return KeyType::Double;
        case tuple::ColumnType::String: return KeyType::String;
    }
    return KeyType::Integer;
}

// Binds the codec to a column type and derives the key width from it:
// 4 for Integer, 8 for Double, the declared width for String.
// `type` — the column type; `string_width` — the declared width for a
// String key (unused otherwise).
KeyCodec::KeyCodec(tuple::ColumnType type, std::size_t string_width)
    : type_(type) {
    switch (type) {
        case tuple::ColumnType::Integer: width_ = 4; break;
        case tuple::ColumnType::Double: width_ = 8; break;
        case tuple::ColumnType::String: width_ = string_width; break;
    }
}

// Encodes one value into fixed-width key bytes: Integer/Double by
// memcpy, String as value bytes plus NUL padding to width().
// `value` — the value to encode; `out` — width() bytes of caller
// storage.
// Returns true on success; false with `error` set for an unbound codec,
// a type mismatch, a String wider than the width, or a String holding
// NUL.
bool KeyCodec::encode(const tuple::Column& value, char* out,
                      std::string& error) const {
    if (width_ == 0) {
        error = "the key column has no declared width";
        return false;
    }
    if (value.column_type() != type_) {
        error = "key value type does not match the indexed column's type";
        return false;
    }
    switch (type_) {
        case tuple::ColumnType::Integer: {
            const int v = std::get<int>(value.column_value());
            std::memcpy(out, &v, 4);
            return true;
        }
        case tuple::ColumnType::Double: {
            const double v = std::get<double>(value.column_value());
            std::memcpy(out, &v, 8);
            return true;
        }
        case tuple::ColumnType::String: {
            const std::string& v = std::get<std::string>(value.column_value());
            if (v.size() > width_) {
                error = "key value is " + std::to_string(v.size()) +
                        " characters, wider than the declared " +
                        std::to_string(width_);
                return false;
            }
            if (v.find('\0') != std::string::npos) {
                error = "key value contains NUL";
                return false;
            }
            std::memcpy(out, v.data(), v.size());
            std::memset(out + v.size(), 0, width_ - v.size());
            return true;
        }
    }
    error = "unknown key type";
    return false;
}

// Decodes width() bytes into a Column of the key type; Strings run to
// the first NUL, at most width() bytes.
// `in` — the encoded key bytes.
// Returns the decoded Column; an empty Column for an unknown type.
tuple::Column KeyCodec::decode(const char* in) const {
    switch (type_) {
        case tuple::ColumnType::Integer: {
            int v = 0;
            std::memcpy(&v, in, 4);
            return tuple::Column(v);
        }
        case tuple::ColumnType::Double: {
            double v = 0.0;
            std::memcpy(&v, in, 8);
            return tuple::Column(v);
        }
        case tuple::ColumnType::String: {
            std::size_t length = 0;
            while (length < width_ && in[length] != '\0') {
                ++length;
            }
            return tuple::Column(std::string(in, length));
        }
    }
    return tuple::Column();
}

// Numeric order for Integer/Double (decoded first, negatives included),
// byte order for String.
// `a`, `b` — encoded keys, each width() bytes.
// Returns negative when a < b, zero when equal, positive when a > b;
// 0 for an unknown type.
int KeyCodec::compare(const char* a, const char* b) const {
    switch (type_) {
        case tuple::ColumnType::Integer: {
            int x = 0;
            int y = 0;
            std::memcpy(&x, a, 4);
            std::memcpy(&y, b, 4);
            return (x > y) - (x < y);
        }
        case tuple::ColumnType::Double: {
            double x = 0.0;
            double y = 0.0;
            std::memcpy(&x, a, 8);
            std::memcpy(&y, b, 8);
            return (x > y) - (x < y);
        }
        case tuple::ColumnType::String:
            return std::memcmp(a, b, width_) == 0 ? 0
                 : (std::memcmp(a, b, width_) < 0 ? -1 : 1);
    }
    return 0;
}

// Renders an encoded key for messages: decode plus type-shaped text,
// strings single-quoted.
// `in` — the encoded key bytes.
// Returns the display text ("" for an unknown type).
std::string KeyCodec::display(const char* in) const {
    switch (type_) {
        case tuple::ColumnType::Integer:
            return std::to_string(std::get<int>(decode(in).column_value()));
        case tuple::ColumnType::Double:
            return std::to_string(std::get<double>(decode(in).column_value()));
        case tuple::ColumnType::String:
            return "'" + std::get<std::string>(decode(in).column_value()) + "'";
    }
    return "";
}

}
