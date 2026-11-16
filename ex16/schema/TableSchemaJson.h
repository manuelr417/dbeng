#ifndef EX11_TABLE_SCHEMA_JSON_H
#define EX11_TABLE_SCHEMA_JSON_H

#include "schema/TableSchema.h"

#include <string>
#include <vector>

namespace schema {

/// One declared index in the catalog's optional "indexes" array: the
/// index's name, the one column it orders, and its structure kind —
/// "hash" (extensible hashing) or "isam". A catalog entry without a
/// kind loads as "isam" (the ex13-era shape).
struct TableIndex {
    std::string name;
    std::string column;
    std::string kind = "isam";
};

/// Parse catalog text — the JSON shape of tablecatalog/person.json: an
/// object with "table_name" and a "columns" array of objects, each with
/// "column_name", "column_type" (Integer/String/Double), and "size" —
/// into a fresh schema. The optional "indexes" array (objects with
/// "name", "column", and an optional "kind": "hash" or "isam", absent
/// meaning "isam") lands in `indexes`; a catalog without one loads
/// with an empty list. On failure: false, a named error ("line N: ..."),
/// `schema` left empty and `indexes` cleared. On success the caller's
/// schema is replaced wholesale: the table name, one add_column per
/// entry in file order, each column's declared size set.
bool parse_table_schema(const std::string& text, TableSchema& schema,
                        std::vector<TableIndex>& indexes, std::string& error);

/// The two-argument form: same parse, indexes discarded (the shape the
/// ex11-era callers and checks use).
bool parse_table_schema(const std::string& text, TableSchema& schema,
                        std::string& error);

/// Read the file at `path` and parse_table_schema it. File errors
/// (missing, unreadable) are named errors like any other.
bool load_table_schema(const std::string& path, TableSchema& schema,
                       std::vector<TableIndex>& indexes, std::string& error);

/// The two-argument form: same load, indexes discarded.
bool load_table_schema(const std::string& path, TableSchema& schema,
                       std::string& error);

/// Serialize `schema` and `indexes` to the catalog JSON shape and install
/// it at `path` atomically: the text is built and validated first (empty
/// schema, unnamed or duplicate indexes, indexes naming unknown columns
/// are named errors), then written to `path + ".tmp"` and renamed over
/// `path` — a failed save never leaves a half-written catalog behind.
bool save_table_schema(const std::string& path, const TableSchema& schema,
                       const std::vector<TableIndex>& indexes,
                       std::string& error);

}

#endif // EX11_TABLE_SCHEMA_JSON_H
