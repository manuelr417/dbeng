#ifndef EX11_TUPLE_H
#define EX11_TUPLE_H

#include "tuple/Column.h"

#include <cstddef>
#include <string>
#include <vector>

namespace tuple {

/// An ordered collection of columns: the tuple IS a vector of columns, with
/// no schema, no capacity, and no cached sizes — the vector is the entire
/// state. Bounds errors throw (programming errors, like BufferPool misuse);
/// data errors in the serializer use the bool + error convention instead.
class Tuple {
public:
    void add_column(const Column& column);
    void add(int value);                   // convenience: build the Column here
    void add(double value);
    void add(const std::string& value);

    std::size_t column_count() const { return columns_.size(); }

    /// Throws std::out_of_range when index >= column_count().
    const Column& column(std::size_t index) const;

    void clear() { columns_.clear(); }

    friend bool operator==(const Tuple& a, const Tuple& b) {
        return a.columns_ == b.columns_;
    }

private:
    std::vector<Column> columns_;
};

}

#endif // EX11_TUPLE_H
