#ifndef EX11_TABLE_SCHEMA_H
#define EX11_TABLE_SCHEMA_H

#include "schema/ColumnSchema.h"
#include "tuple/Tuple.h"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace schema {

/// Metadata about a table and its tuples: the catalog of columns
/// (column_info, in creation order) and the name index (column_name_map).
/// It maps the logical structure of the table onto tuple::Tuple objects.
/// Columns are addressed exactly two ways: by creation-order index and by
/// name. Record bytes are written and read only by TupleSerializer, the
/// one serializer, bound to a schema at construction.
class TableSchema {
public:
    explicit TableSchema(const std::string& table_name);

    /// Appends a column: one entry in column_info (the column's
    /// creation-order index is the current column count) and one entry in
    /// column_name_map.
    /// Fails on a duplicate or empty column name.
    bool add_column(const std::string& name, tuple::ColumnType type,
                    std::string& error);

    const std::string& table_name() const { return table_name_; }
    std::size_t column_count() const { return column_info_.size(); }

    /// Throws std::out_of_range for an index >= column_count().
    const ColumnSchema& column_info(std::size_t index) const;

    /// The column_name_map service: nullopt for an unknown name. The
    /// result indexes column_info and is also the column's tuple slot.
    std::optional<std::size_t> column_index(const std::string& name) const;

    /// True when the tuple matches the schema: same column count, and
    /// tuple slot i holds the type of schema column i (creation order).
    /// Named error otherwise.
    bool validate(const tuple::Tuple& tuple, std::string& error) const;

    /// A fresh tuple in creation order, every column at its default
    /// value (int 0, "", double 0.0) with the schema's types.
    tuple::Tuple make_tuple() const;

private:
    std::string table_name_;
    std::vector<ColumnSchema> column_info_;
    std::unordered_map<std::string, std::size_t> column_name_map_;

    // The JSON catalog loader (schema/TableSchemaJson.cpp) is the one
    // writer of declared column widths after add_column.
    friend bool parse_table_schema(const std::string& text, TableSchema& schema,
                                   std::string& error);
};

}

#endif // EX11_TABLE_SCHEMA_H
