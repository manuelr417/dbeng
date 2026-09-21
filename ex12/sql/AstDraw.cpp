#include "AstDraw.h"

#include "AstPrinter.h"

#include <algorithm>
#include <string>
#include <vector>

namespace sql {

namespace {

// A row of cells; every cell renders as exactly one terminal column, so
// widths and offsets stay honest even with multi-byte box characters.
using Row = std::vector<std::string>;

struct Block {
    std::vector<Row> rows;
    std::size_t center = 0;  // column of the root label's center in rows[0]

    std::size_t width() const { return rows.empty() ? 0 : rows[0].size(); }
};

Row spaces(std::size_t count) { return Row(count, " "); }

Row text_cells(const std::string& text) {
    Row cells;
    cells.reserve(text.size());
    for (const char c : text) cells.emplace_back(1, c);
    return cells;
}

Row padded(Row cells, std::size_t width) {
    while (cells.size() < width) cells.push_back(" ");
    return cells;
}

std::string join_row(const Row& cells) {
    std::string text;
    for (const std::string& cell : cells) text += cell;
    while (!text.empty() && text.back() == ' ') text.pop_back();
    return text;
}

Block leaf_block(const std::string& label) {
    return {{text_cells(label)}, (label.size() - 1) / 2};
}

// One child, centered; a single │ stub row joins parent to child.
Block unary_block(const std::string& label, Block child) {
    const std::size_t label_width = label.size();
    const std::size_t width = std::max(label_width, child.width());
    const std::size_t parent_off = (width - label_width) / 2;
    const std::size_t child_off = (width - child.width()) / 2;

    Block out;
    Row parent = spaces(parent_off);
    for (const std::string& cell : text_cells(label)) parent.push_back(cell);
    out.rows.push_back(padded(std::move(parent), width));

    Row stub = spaces(child_off + child.center);
    stub.push_back("│");
    out.rows.push_back(padded(std::move(stub), width));

    for (const Row& row : child.rows) {
        Row placed = spaces(child_off);
        for (const std::string& cell : row) placed.push_back(cell);
        out.rows.push_back(padded(std::move(placed), width));
    }
    out.center = parent_off + (label_width - 1) / 2;
    return out;
}

// Two children side by side; ┌ at the left child's center, ┴ at the
// parent's center, ┐ at the right child's center, dashes between. The
// gap widens until the parent label fits over the branch row.
Block binary_block(const std::string& label, Block left, Block right) {
    const std::size_t label_width = label.size();
    const std::size_t left_width = left.width();
    const std::size_t right_width = right.width();
    std::size_t gap = 2;
    while (left_width + gap + right_width < label_width) ++gap;
    const std::size_t span = left_width + gap + right_width;
    const std::size_t left_center = left.center;
    const std::size_t right_center = left_width + gap + right.center;
    const std::size_t pivot = (left_center + right_center) / 2;
    const std::size_t parent_off =
        pivot >= (label_width - 1) / 2 ? pivot - (label_width - 1) / 2 : 0;

    Block out;
    Row parent = spaces(parent_off);
    for (const std::string& cell : text_cells(label)) parent.push_back(cell);
    out.rows.push_back(padded(std::move(parent), span));

    Row branch = spaces(left_center);
    branch.push_back("┌");
    for (std::size_t i = left_center + 1; i < pivot; ++i) branch.push_back("─");
    branch.push_back("┴");
    for (std::size_t i = pivot + 1; i < right_center; ++i) branch.push_back("─");
    branch.push_back("┐");
    out.rows.push_back(padded(std::move(branch), span));

    const std::size_t height = std::max(left.rows.size(), right.rows.size());
    for (std::size_t i = 0; i < height; ++i) {
        Row row = padded(i < left.rows.size() ? left.rows[i] : Row{}, left_width);
        for (std::size_t g = 0; g < gap; ++g) row.push_back(" ");
        if (i < right.rows.size()) {
            for (const std::string& cell : right.rows[i]) row.push_back(cell);
        }
        out.rows.push_back(padded(std::move(row), span));
    }
    out.center = parent_off + (label_width - 1) / 2;
    return out;
}

// --- expression labels and blocks ---

std::string column_text(const Expr& expr) {
    return expr.table.empty() ? expr.column : expr.table + "." + expr.column;
}

// The AST name of an operator (Eq, Ge, And, …) — the drawing speaks the
// tree's vocabulary, like sql.md's worked examples, not SQL punctuation.
std::string op_name(BinaryOp op) {
    switch (op) {
        case BinaryOp::Eq: return "Eq";
        case BinaryOp::Ne: return "Ne";
        case BinaryOp::Lt: return "Lt";
        case BinaryOp::Le: return "Le";
        case BinaryOp::Gt: return "Gt";
        case BinaryOp::Ge: return "Ge";
        case BinaryOp::And: return "And";
        case BinaryOp::Or: return "Or";
        case BinaryOp::Plus: return "Plus";
        case BinaryOp::Minus: return "Minus";
        case BinaryOp::Mul: return "Mul";
        case BinaryOp::Div: return "Div";
        case BinaryOp::Mod: return "Mod";
    }
    return "?";
}

// The one-line label for any expression kind; composites use their top
// label only (rows in INSERT reach this for literals, never composites).
std::string leaf_label(const Expr& expr) {
    switch (expr.kind) {
        case Expr::Kind::Literal:
            return "Literal{ " + literal_text(expr.value) + " }";
        case Expr::Kind::ColumnRef:
            return "ColumnRef{ " + column_text(expr) + " }";
        case Expr::Kind::Function:
            if (expr.arg == nullptr) return "Function{ " + function_name(expr.fn) + "(*) }";
            return "Function{ " + function_name(expr.fn) + " }";
        case Expr::Kind::Binary:
            return "Binary{ " + op_name(expr.bin_op) + " }";
        case Expr::Kind::Unary:
            return expr.un_op == UnaryOp::Not ? "Unary{ Not }" : "Unary{ Neg }";
    }
    return "?";
}

Block draw_expr_block(const Expr& expr) {
    switch (expr.kind) {
        case Expr::Kind::Literal:
        case Expr::Kind::ColumnRef:
            return leaf_block(leaf_label(expr));
        case Expr::Kind::Binary:
            return binary_block(leaf_label(expr), draw_expr_block(*expr.left),
                                draw_expr_block(*expr.right));
        case Expr::Kind::Unary:
            return unary_block(leaf_label(expr), draw_expr_block(*expr.operand));
        case Expr::Kind::Function:
            if (expr.arg == nullptr) return leaf_block(leaf_label(expr));
            return unary_block(leaf_label(expr), draw_expr_block(*expr.arg));
    }
    return leaf_block("?");
}

// --- statement outline ---

struct Section {
    std::string label;
    std::string inline_text;
    std::vector<Section> children;
    std::vector<std::string> block;
};

void render_section(std::vector<std::string>& out, const Section& section,
                    const std::string& gutter, bool last) {
    std::string line = gutter + (last ? "└─ " : "├─ ") + section.label;
    if (!section.inline_text.empty()) line += "  " + section.inline_text;
    out.push_back(line);
    const std::string inner = gutter + (last ? "    " : "│   ");
    for (std::size_t i = 0; i < section.children.size(); ++i) {
        render_section(out, section.children[i], inner, i + 1 == section.children.size());
    }
    for (const std::string& text : section.block) {
        out.push_back(inner + "      " + text);
    }
}

// Leaves join their clause line; composites hang beneath as blocks.
void attach_expr(Section& section, const Expr& expr) {
    const Block block = draw_expr_block(expr);
    if (block.rows.size() == 1) {
        section.inline_text = join_row(block.rows[0]);
    } else {
        for (const Row& row : block.rows) section.block.push_back(join_row(row));
    }
}

std::string table_ref_text(const TableRef& table) {
    return table.alias.empty() ? "TableRef{ " + table.name + " }"
                               : "TableRef{ " + table.name + " AS " + table.alias + " }";
}

std::string data_type_text(const ColumnDef& column) {
    switch (column.type) {
        case DataType::Integer: return "INTEGER(" + std::to_string(column.size) + ")";
        case DataType::Double: return "DOUBLE(" + std::to_string(column.size) + ")";
        case DataType::String: return "VARCHAR(" + std::to_string(column.size) + ")";
    }
    return "";
}

std::vector<Section> select_sections(const SelectStatement& statement) {
    std::vector<Section> sections;

    Section items;
    items.label = "items";
    for (std::size_t i = 0; i < statement.items.size(); ++i) {
        const SelectItem& item = statement.items[i];
        Section section;
        section.label = "SelectItem[" + std::to_string(i) + "]";
        if (item.star) {
            section.inline_text = "*";
        } else {
            attach_expr(section, *item.expr);
            if (!item.alias.empty()) {
                section.inline_text += section.inline_text.empty()
                                           ? "AS " + item.alias
                                           : " AS " + item.alias;
            }
        }
        items.children.push_back(std::move(section));
    }
    sections.push_back(std::move(items));

    Section from;
    from.label = "from";
    from.inline_text = table_ref_text(statement.from);
    sections.push_back(std::move(from));

    if (!statement.joins.empty()) {
        Section joins;
        joins.label = "joins";
        for (const JoinClause& join : statement.joins) {
            Section clause;
            clause.label = "JoinClause";
            clause.inline_text = table_ref_text(join.table);
            Section on;
            on.label = "on";
            attach_expr(on, *join.on);
            clause.children.push_back(std::move(on));
            joins.children.push_back(std::move(clause));
        }
        sections.push_back(std::move(joins));
    }

    if (statement.where != nullptr) {
        Section where;
        where.label = "where";
        attach_expr(where, *statement.where);
        sections.push_back(std::move(where));
    }

    if (!statement.group_by.empty()) {
        Section group_by;
        group_by.label = "group_by";
        for (std::size_t i = 0; i < statement.group_by.size(); ++i) {
            if (i > 0) group_by.inline_text += ", ";
            group_by.inline_text += column_text(*statement.group_by[i]);
        }
        sections.push_back(std::move(group_by));
    }

    if (!statement.order_by.empty()) {
        Section order_by;
        order_by.label = "order_by";
        for (std::size_t i = 0; i < statement.order_by.size(); ++i) {
            if (i > 0) order_by.inline_text += ", ";
            order_by.inline_text += column_text(*statement.order_by[i].expr);
            if (statement.order_by[i].descending) order_by.inline_text += " DESC";
        }
        sections.push_back(std::move(order_by));
    }

    if (statement.has_limit) {
        Section limit;
        limit.label = "limit";
        limit.inline_text = std::to_string(statement.limit);
        sections.push_back(std::move(limit));
    }
    return sections;
}

std::vector<Section> create_sections(const CreateTableStatement& statement) {
    Section table;
    table.label = "table";
    table.inline_text = statement.table;
    Section columns;
    columns.label = "columns";
    for (const ColumnDef& column : statement.columns) {
        Section section;
        section.label = column.name;
        section.inline_text = data_type_text(column);
        columns.children.push_back(std::move(section));
    }
    return {std::move(table), std::move(columns)};
}

std::vector<Section> insert_sections(const InsertStatement& statement) {
    Section table;
    table.label = "table";
    table.inline_text = statement.table;
    Section rows;
    rows.label = "rows";
    for (std::size_t i = 0; i < statement.rows.size(); ++i) {
        Section row;
        row.label = "row[" + std::to_string(i) + "]";
        for (std::size_t j = 0; j < statement.rows[i].size(); ++j) {
            if (j > 0) row.inline_text += ", ";
            row.inline_text += leaf_label(*statement.rows[i][j]);
        }
        rows.children.push_back(std::move(row));
    }
    return {std::move(table), std::move(rows)};
}

std::vector<Section> update_sections(const UpdateStatement& statement) {
    Section table;
    table.label = "table";
    table.inline_text = statement.table;
    Section sets;
    sets.label = "sets";
    for (const SetClause& set : statement.sets) {
        Section section;
        section.label = "SetClause{ " + set.column + " }";
        const Block value = draw_expr_block(*set.value);
        if (value.rows.size() == 1) {
            section.inline_text = "= " + join_row(value.rows[0]);
        } else {
            for (const Row& row : value.rows) section.block.push_back(join_row(row));
        }
        sets.children.push_back(std::move(section));
    }
    std::vector<Section> sections{std::move(table), std::move(sets)};
    if (statement.where != nullptr) {
        Section where;
        where.label = "where";
        attach_expr(where, *statement.where);
        sections.push_back(std::move(where));
    }
    return sections;
}

std::vector<Section> delete_sections(const DeleteStatement& statement) {
    Section table;
    table.label = "table";
    table.inline_text = statement.table;
    std::vector<Section> sections{std::move(table)};
    if (statement.where != nullptr) {
        Section where;
        where.label = "where";
        attach_expr(where, *statement.where);
        sections.push_back(std::move(where));
    }
    return sections;
}

}

std::string draw(const Statement& statement) {
    std::string title;
    std::vector<Section> sections;
    switch (statement.kind) {
        case Statement::Kind::CreateTable:
            title = "CreateTableStatement";
            sections = create_sections(static_cast<const CreateTableStatement&>(statement));
            break;
        case Statement::Kind::DropTable: {
            title = "DropTableStatement";
            Section table;
            table.label = "table";
            table.inline_text = static_cast<const DropTableStatement&>(statement).table;
            sections.push_back(std::move(table));
            break;
        }
        case Statement::Kind::Insert:
            title = "InsertStatement";
            sections = insert_sections(static_cast<const InsertStatement&>(statement));
            break;
        case Statement::Kind::Update:
            title = "UpdateStatement";
            sections = update_sections(static_cast<const UpdateStatement&>(statement));
            break;
        case Statement::Kind::Delete:
            title = "DeleteStatement";
            sections = delete_sections(static_cast<const DeleteStatement&>(statement));
            break;
        case Statement::Kind::Select:
            title = "SelectStatement";
            sections = select_sections(static_cast<const SelectStatement&>(statement));
            break;
    }
    std::vector<std::string> lines{title};
    for (std::size_t i = 0; i < sections.size(); ++i) {
        render_section(lines, sections[i], "", i + 1 == sections.size());
    }
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out += "\n";
        out += lines[i];
    }
    return out;
}

}
