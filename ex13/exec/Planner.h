#pragma once

// Planner — picks the access path for one SELECT's WHERE clause. The
// rule set (design D6): flatten top-level AND conjuncts; a conjunct of
// the form `column op literal` (or the flipped literal form) whose
// column carries a catalog index yields bounds — equality a point,
// < <= > >= open or closed bounds; conjuncts on the same column
// intersect (an empty intersection stays an index scan that answers
// nothing). Every other conjunct — OR anywhere at the top level,
// non-literal sides, unindexed columns — becomes residual, evaluated
// per fetched row. When no conjunct is indexable the plan is a heap
// scan. Correctness never depends on the choice: both paths must
// answer identically, which the check suite's oracle enforces.

#include "exec/Binder.h"
#include "schema/TableSchema.h"

#include <optional>
#include <string>
#include <vector>

namespace exec {

// One index declared in the table's catalog, resolved to its column.
struct CatalogIndex {
    std::string name;
    std::size_t column_index = 0;
};

struct IndexBounds {
    std::size_t column_index = 0;
    std::string index_name;
    std::optional<Value> lo;
    bool lo_inclusive = true;
    std::optional<Value> hi;
    bool hi_inclusive = true;

    // False when the intersected bounds admit no key (lo > hi).
    bool possible() const;
};

struct ScanPlan {
    bool use_index = false;
    IndexBounds bounds;                     // valid when use_index
    std::vector<BoundExprPtr> residual;     // evaluated per fetched row
};

// Consumes `where` (moved in) and produces the plan. `indexes` are the
// table's catalog-declared indexes.
ScanPlan plan_where(BoundExprPtr where,
                    const std::vector<CatalogIndex>& indexes);

}

