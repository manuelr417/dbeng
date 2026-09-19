#ifndef EX11_RECORD_CODEC_H
#define EX11_RECORD_CODEC_H

#include "schema/TableSchema.h"
#include "tuple/Tuple.h"

#include <cstddef>
#include <string>

namespace schema {

/// Total fixed record width for `schema`: 4 bytes per Integer column, 8
/// per Double, column_size per String. Returns 0 for an empty schema or
/// one with undeclared-width String columns; pack/unpack report the
/// named error for those.
std::size_t record_size(const TableSchema& schema);

/// validate, then pack every column at its fixed offset (heap.md,
/// section 3): Integer/Double at their natural widths, Strings NUL-padded
/// to their declared width. Fails before touching the buffer when the
/// tuple does not match the schema (arity, types, string width, NUL) or
/// when max_len is too small.
bool pack(const TableSchema& schema, const tuple::Tuple& tuple,
          char* buffer, std::size_t max_len, std::string& error);

/// Unpack a record into a fresh tuple in column_position order. Verifies
/// that every String field's padding is all NUL — the fixed-format
/// integrity check that replaces the wire format's type tags. The out
/// tuple is cleared first and stays empty on any failure.
bool unpack(const TableSchema& schema, const char* buffer,
            std::size_t max_len, tuple::Tuple& tuple, std::string& error);

}

#endif // EX11_RECORD_CODEC_H
