#ifndef EX11_TABLE_CSV_H
#define EX11_TABLE_CSV_H

#include "schema/TableSchema.h"
#include "tuple/Tuple.h"

#include <cstddef>
#include <ostream>
#include <string>
#include <vector>

namespace schema {

/// One parsed CSV line as a schema-shaped tuple; a bad line throws a
/// named reason (no line prefix — the caller adds "line N: "). The out
/// tuple is cleared first and stays empty on failure.
void parse_csv_row(const TableSchema& schema, const std::string& line,
                   tuple::Tuple& tuple);

/// Everything one CSV load reports: the valid rows in file order and how
/// many lines were skipped.
struct CsvLoadResult {
    std::vector<tuple::Tuple> tuples;
    std::size_t skipped = 0;
};

/// Loads CSV rows shaped by `schema`: one record per line, comma-separated
/// with one token per column in position order. Invalid rows print
/// "line N: reason" to `diagnostics` and are skipped, never fatal; blank
/// lines are skipped silently; a trailing \r is stripped. Throws a named
/// error only when the file itself cannot be read.
void load_csv(const TableSchema& schema, const std::string& path,
              CsvLoadResult& result, std::ostream& diagnostics);

}

#endif // EX11_TABLE_CSV_H
