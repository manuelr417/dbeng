#include "exec/Planner.h"

namespace exec {

namespace {

bool has_index(const std::vector<CatalogIndex>& indexes,
               std::size_t column_index, CatalogIndex& found) {
    for (const CatalogIndex& index : indexes) {
        if (index.column_index == column_index) {
            found = index;
            return true;
        }
    }
    return false;
}

// True when the node is a bare literal (no column references).
bool is_literal(const BoundExpr& expr) {
    return expr.kind == BoundExpr::Kind::Literal;
}

// Literal-on-the-left comparisons read in the flipped direction.
sql::BinaryOp flip(sql::BinaryOp op) {
    switch (op) {
        case sql::BinaryOp::Lt: return sql::BinaryOp::Gt;
        case sql::BinaryOp::Le: return sql::BinaryOp::Ge;
        case sql::BinaryOp::Gt: return sql::BinaryOp::Lt;
        case sql::BinaryOp::Ge: return sql::BinaryOp::Le;
        default: return op;  // Eq and Ne are symmetric
    }
}

}  // namespace

bool IndexBounds::possible() const {
    if (lo && hi) {
        return compare_values(*lo, *hi) <= 0;
    }
    return true;
}

ScanPlan plan_where(BoundExprPtr where,
                    const std::vector<CatalogIndex>& indexes) {
    ScanPlan plan;
    if (!where) {
        return plan;  // no WHERE: heap scan, no residual
    }

    // Flatten the top-level AND chain (moving the nodes out). A top-level
    // OR selects the heap path with the whole predicate as residual.
    std::vector<BoundExprPtr> conjuncts;
    if (where->kind == BoundExpr::Kind::Binary &&
        where->bin_op == sql::BinaryOp::Or) {
        plan.residual.push_back(std::move(where));
        return plan;
    }
    if (where->kind == BoundExpr::Kind::Binary &&
        where->bin_op == sql::BinaryOp::And) {
        // Walk down the left spine collecting AND operands.
        std::vector<BoundExpr*> stack;
        stack.push_back(where.get());
        // First flatten into a temporary owned list: move children out.
        // Ownership juggling: the root owns left/right; we recursively
        // move them into conjuncts.
        std::vector<BoundExprPtr> owned;
        owned.push_back(std::move(where));
        while (!owned.empty()) {
            BoundExprPtr node = std::move(owned.back());
            owned.pop_back();
            if (node->kind == BoundExpr::Kind::Binary &&
                node->bin_op == sql::BinaryOp::And) {
                owned.push_back(std::move(node->right));
                owned.push_back(std::move(node->left));
                continue;
            }
            conjuncts.push_back(std::move(node));
        }
    } else {
        conjuncts.push_back(std::move(where));
    }

    std::optional<IndexBounds> bounds;
    for (BoundExprPtr& conjunct : conjuncts) {
        // Shape 1: column op literal. Shape 2: literal op column.
        const BoundExpr* column_side = nullptr;
        const BoundExpr* literal_side = nullptr;
        sql::BinaryOp op = conjunct->bin_op;
        if (conjunct->kind == BoundExpr::Kind::Binary &&
            conjunct->left->kind == BoundExpr::Kind::Column &&
            is_literal(*conjunct->right)) {
            column_side = conjunct->left.get();
            literal_side = conjunct->right.get();
        } else if (conjunct->kind == BoundExpr::Kind::Binary &&
                   conjunct->left->kind == BoundExpr::Kind::Literal &&
                   conjunct->right->kind == BoundExpr::Kind::Column) {
            column_side = conjunct->right.get();
            literal_side = conjunct->left.get();
            op = flip(op);
        } else {
            plan.residual.push_back(std::move(conjunct));
            continue;
        }

        CatalogIndex found;
        if (op == sql::BinaryOp::Ne ||
            !has_index(indexes, column_side->column_index, found)) {
            plan.residual.push_back(std::move(conjunct));
            continue;
        }
        if (!bounds) {
            bounds = IndexBounds{};
            bounds->column_index = column_side->column_index;
            bounds->index_name = found.name;
            plan.use_index = true;
        }
        if (bounds->column_index != column_side->column_index) {
            // A second indexed column: keep the first, residual the rest.
            plan.residual.push_back(std::move(conjunct));
            continue;
        }
        const Value value = literal_side->literal;
        switch (op) {
            case sql::BinaryOp::Eq:
                bounds->lo = value;
                bounds->lo_inclusive = true;
                bounds->hi = value;
                bounds->hi_inclusive = true;
                break;
            case sql::BinaryOp::Lt:
                if (!bounds->hi || compare_values(value, *bounds->hi) < 0 ||
                    (compare_values(value, *bounds->hi) == 0 &&
                     bounds->hi_inclusive)) {
                    bounds->hi = value;
                    bounds->hi_inclusive = false;
                }
                break;
            case sql::BinaryOp::Le:
                if (!bounds->hi || compare_values(value, *bounds->hi) <= 0) {
                    bounds->hi = value;
                    bounds->hi_inclusive = true;
                }
                break;
            case sql::BinaryOp::Gt:
                if (!bounds->lo || compare_values(value, *bounds->lo) > 0 ||
                    (compare_values(value, *bounds->lo) == 0 &&
                     bounds->lo_inclusive)) {
                    bounds->lo = value;
                    bounds->lo_inclusive = false;
                }
                break;
            case sql::BinaryOp::Ge:
                if (!bounds->lo || compare_values(value, *bounds->lo) >= 0) {
                    bounds->lo = value;
                    bounds->lo_inclusive = true;
                }
                break;
            default:
                break;
        }
    }
    if (plan.use_index && bounds) {
        plan.bounds = std::move(*bounds);
    }
    return plan;
}

}
