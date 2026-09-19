#ifndef EX11_TABLE_SCHEMA_H
#define EX11_TABLE_SCHEMA_H

#include "schema/ColumnSchema.h"
#include "tuple/Tuple.h"
#include "tuple/TupleSerializer.h"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace schema {

/// Metadata about a table and its tuples: the catalog of columns
/// (column_info, in definition order), the wire layout (column_order), and
/// the name index (column_name_map). It maps the logical structure of the
/// table onto tuple::Tuple objects and drives serialization in a general
/// way — the wire format itself stays the one from tuple.md, section 6.
class TableSchema {
public:
    explicit TableSchema(const std::string& table_name);

    /// Appends a column: one entry in column_info (column_position = the
    /// current column count), one entry in column_name_map, and one slot
    /// appended to column_order (identity order until set_column_order
    /// says otherwise).
    /// Fails on a duplicate or empty column name.
    bool add_column(const std::string& name, tuple::ColumnType type,
                    std::string& error);

    /// Installs a wire layout: order[p] is the column_info index stored at
    /// wire position p. Accepts only a real permutation of
    /// 0..column_count()-1; on failure the previous order is kept.
    bool set_column_order(const std::vector<std::size_t>& order,
                          std::string& error);

    const std::string& table_name() const { return table_name_; }
    std::size_t column_count() const { return column_info_.size(); }

    /// Throws std::out_of_range for an index >= column_count().
    const ColumnSchema& column_info(std::size_t index) const;
    const std::vector<std::size_t>& column_order() const {
        return column_order_;
    }

    /// The column_name_map service: nullopt for an unknown name. The result
    /// indexes column_info; the column's tuple slot is
    /// column_info(index).column_position().
    std::optional<std::size_t> column_index(const std::string& name) const;

    /// True when the tuple matches the schema: same column count, and every
    /// tuple position p holds the type of the schema column whose
    /// column_position is p. Named error otherwise.
    bool validate(const tuple::Tuple& tuple, std::string& error) const;

    /// A fresh tuple in column_position order, every column at its default
    /// value (int 0, "", double 0.0) with the schema's types.
    tuple::Tuple make_tuple() const;

private:
    std::string table_name_;
    std::vector<ColumnSchema> column_info_;
    std::vector<std::size_t> column_order_; // wire position -> column index
    std::unordered_map<std::string, std::size_t> column_name_map_;

    // The JSON catalog loader (schema/TableSchemaJson.cpp) is the one
    // writer of declared column widths after add_column.
    friend bool parse_table_schema(const std::string& text, TableSchema& schema,
                                   std::string& error);
};

/// Schema-driven serialization: validate, permute the tuple into wire
/// order, then delegate to tuple::serialize (the one encoding path).
bool serialize(const TableSchema& schema, const tuple::Tuple& tuple,
               char* buffer, std::size_t max_len, std::string& error);

/// Schema-driven deserialization: tuple::deserialize, un-permute into
/// column_position order, validate against the schema. The out tuple is
/// cleared first and stays empty on any failure.
bool deserialize(const TableSchema& schema, const char* buffer,
                 std::size_t max_len, tuple::Tuple& tuple, std::string& error);

}

#endif // EX11_TABLE_SCHEMA_H
