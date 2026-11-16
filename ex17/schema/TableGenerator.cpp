#include "schema/TableGenerator.h"

#include "schema/ColumnSchema.h"
#include "tuple/Column.h"

#include <cmath>
#include <random>
#include <string>

namespace schema {

std::vector<tuple::Tuple> generate_tuples(const TableSchema& schema,
                                          std::size_t count,
                                          std::uint32_t seed,
                                          int first_id) {
    std::mt19937 engine(seed);
    std::uniform_int_distribution<int> integer_dist(0, 999);
    std::uniform_int_distribution<int> letter_dist(0, 25);
    std::uniform_real_distribution<double> double_dist(0.0, 100.0);

    std::vector<tuple::Tuple> tuples;
    tuples.reserve(count);
    int sequence_position = -1; // the first Integer column, found once
    for (std::size_t i = 0; i < count; ++i) {
        tuple::Tuple tuple;
        for (std::size_t p = 0; p < schema.column_count(); ++p) {
            const ColumnSchema& column = schema.column_info(p);
            switch (column.column_type()) {
                case tuple::ColumnType::Integer: {
                    // The first Integer column is the record's visible
                    // identity: every record counts upward from first_id.
                    if (sequence_position < 0) {
                        sequence_position = static_cast<int>(p);
                    }
                    if (static_cast<int>(p) == sequence_position) {
                        tuple.add(first_id + static_cast<int>(i));
                    } else {
                        tuple.add(integer_dist(engine));
                    }
                    break;
                }
                case tuple::ColumnType::Double: {
                    const double value =
                        std::round(double_dist(engine) * 10.0) / 10.0;
                    tuple.add(value);
                    break;
                }
                case tuple::ColumnType::String: {
                    // Always valid by construction: length in [1, width],
                    // letters only — no NUL, never wider than the column.
                    const std::size_t width =
                        column.column_size() > 0 ? column.column_size() : 8;
                    std::uniform_int_distribution<std::size_t>
                        length_dist(1, width);
                    std::string value;
                    const std::size_t length = length_dist(engine);
                    for (std::size_t k = 0; k < length; ++k) {
                        value += static_cast<char>('a' + letter_dist(engine));
                    }
                    tuple.add(value);
                    break;
                }
            }
        }
        tuples.push_back(std::move(tuple));
    }
    return tuples;
}

}
