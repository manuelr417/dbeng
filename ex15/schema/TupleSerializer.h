#ifndef EX11_TUPLE_SERIALIZER_H
#define EX11_TUPLE_SERIALIZER_H

#include "schema/TableSchema.h"
#include "tuple/Tuple.h"

#include <cstddef>
#include <string>

namespace schema {

/// The one serializer in the engine, bound to one table schema at
/// construction: the schema supplies everything structural — the arity,
/// each column's type, and each String's declared width — so the record
/// bytes are values only, laid out in the schema's creation order. There
/// is no per-call schema parameter; every method works against the bound
/// schema. The bound schema must outlive the serializer. All failures
/// use the engine-wide bool + error convention.
class TupleSerializer {
public:
    explicit TupleSerializer(const TableSchema& schema);

    /// Total fixed record width of the bound schema: 4 bytes per Integer
    /// column, 8 per Double, column_size per String. Returns 0 for an
    /// empty schema or one with undeclared-width String columns;
    /// serialize/deserialize report the named error for those.
    std::size_t record_size() const;

    /// Validates `tuple` against the bound schema first and fails before
    /// touching the buffer (arity, types, widths, NUL; a String with no
    /// declared width cannot live in a record). max_len is a bound, not a
    /// target — trailing bytes are left untouched.
    bool serialize(const tuple::Tuple& tuple, char* buffer,
                   std::size_t max_len, std::string& error) const;

    /// Reads a record written by serialize(): walks the columns in
    /// creation order, reassembles the logical tuple, verifies that every
    /// String's padding is all NUL, and validates the result against the
    /// schema. The out tuple is cleared first and stays empty on any
    /// failure.
    bool deserialize(const char* buffer, std::size_t max_len,
                     tuple::Tuple& tuple, std::string& error) const;

private:
    const TableSchema& schema_; // must outlive the serializer
};

}

#endif // EX11_TUPLE_SERIALIZER_H
