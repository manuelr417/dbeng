#include "tuple/Tuple.h"

#include <stdexcept>

namespace tuple {

void Tuple::add_column(const Column& column) {
    columns_.push_back(column);
}

void Tuple::add(int value) {
    add_column(Column(value));
}

void Tuple::add(double value) {
    add_column(Column(value));
}

void Tuple::add(const std::string& value) {
    add_column(Column(value));
}

const Column& Tuple::column(std::size_t index) const {
    if (index >= columns_.size()) {
        throw std::out_of_range("tuple column index out of range");
    }
    return columns_[index];
}

}
