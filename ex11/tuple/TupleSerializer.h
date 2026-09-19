#ifndef EX11_TUPLE_SERIALIZER_H
#define EX11_TUPLE_SERIALIZER_H

#include "tuple/Tuple.h"

#include <cstddef>
#include <string>

namespace tuple {

/// Byte count serialize() would write: 4 bytes for the column count plus,
/// per column, the 8-byte [type][length] header and the payload bytes.
std::size_t serialized_size(const Tuple& tuple);

/// Writes the tuple into `buffer` (native-endian memcpy fields; the layout
/// is documented in tuple.md, section 6). Fails when max_len is too small or
/// a column's type/length mirror is broken. max_len is a bound, not a
/// target — trailing buffer bytes are left untouched.
bool serialize(const Tuple& tuple, char* buffer, std::size_t max_len,
               std::string& error);

/// Reads a tuple written by serialize(). The target `tuple` is cleared
/// first and stays empty on failure. Per column, the type tag must be a
/// known ColumnType, the stored length must match the type, and the payload
/// must fit the buffer. Trailing bytes after the last column are ignored
/// (a buffer may be a whole 4096-byte block holding one tuple).
bool deserialize(const char* buffer, std::size_t max_len, Tuple& tuple,
                 std::string& error);

}

#endif // EX11_TUPLE_SERIALIZER_H
