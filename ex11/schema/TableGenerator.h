#ifndef EX11_TABLE_GENERATOR_H
#define EX11_TABLE_GENERATOR_H

#include "schema/TableSchema.h"
#include "tuple/Tuple.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace schema {

/// Generates `count` schema-shaped tuples with pseudo-random values that
/// always pass `validate`: Strings within their declared widths and free
/// of NUL, numerics in a small readable range. Deterministic for a given
/// seed. The first Integer column (position order) is a sequence starting
/// at `first_id` — the record's visible identity, the way PersonGenerator
/// numbered pids; every other column is random.
std::vector<tuple::Tuple> generate_tuples(const TableSchema& schema,
                                          std::size_t count,
                                          std::uint32_t seed,
                                          int first_id = 1);

}

#endif // EX11_TABLE_GENERATOR_H
