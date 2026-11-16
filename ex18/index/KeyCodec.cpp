#include "index/KeyCodec.h"

#include "common/Error.h"

#include <cstring>
#include <format>

namespace isam {

KeyType key_type_of(tuple::ColumnType type) {
    switch (type) {
        case tuple::ColumnType::Integer: return KeyType::Integer;
        case tuple::ColumnType::Double: return KeyType::Double;
        case tuple::ColumnType::String: return KeyType::String;
    }
    return KeyType::Integer;
}

KeyCodec::KeyCodec(tuple::ColumnType type, std::size_t string_width)
    : type_(type) {
    switch (type) {
        case tuple::ColumnType::Integer: width_ = 4; break;
        case tuple::ColumnType::Double: width_ = 8; break;
        case tuple::ColumnType::String: width_ = string_width; break;
    }
}

void KeyCodec::encode(const tuple::Column& value, char* out) const {
    if (width_ == 0) {
        throw common::Error("the key column has no declared width");
    }
    if (value.column_type() != type_) {
        throw common::Error(
            "key value type does not match the indexed column's type");
    }
    switch (type_) {
        case tuple::ColumnType::Integer: {
            const int v = std::get<int>(value.column_value());
            std::memcpy(out, &v, 4);
            return;
        }
        case tuple::ColumnType::Double: {
            const double v = std::get<double>(value.column_value());
            std::memcpy(out, &v, 8);
            return;
        }
        case tuple::ColumnType::String: {
            const std::string& v = std::get<std::string>(value.column_value());
            if (v.size() > width_) {
                throw common::Error(std::format(
                    "key value is {} characters, wider than the declared {}",
                    v.size(), width_));
            }
            if (v.find('\0') != std::string::npos) {
                throw common::Error("key value contains NUL");
            }
            std::memcpy(out, v.data(), v.size());
            std::memset(out + v.size(), 0, width_ - v.size());
            return;
        }
    }
    throw common::Error("unknown key type");
}

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
