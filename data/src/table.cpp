#include "elanora/data/table.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "elanora/csv.hpp"

namespace elanora::data {

namespace fs = std::filesystem;

namespace {
const std::string kEmpty;
}

Table Table::read(const fs::path& path) {
    Table t;
    CsvReader r(path);
    t.header_ = r.header();
    std::vector<std::string> row;
    while (r.next(row)) {
        // A short row is padded rather than rejected. A spreadsheet round-trip
        // routinely drops trailing empty fields, and losing a whole session to
        // that would be a poor trade.
        row.resize(t.header_.size());
        t.rows_.push_back(row);
    }
    return t;
}

Table Table::read_or_empty(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return Table{};
    try {
        return read(path);
    } catch (const CsvError&) {
        return Table{};
    }
}

void Table::write(const fs::path& path) const {
    CsvWriter w(path, header_);
    for (const std::vector<std::string>& r : rows_) w.row(r);
}

int Table::column(const std::string& name) const {
    for (std::size_t i = 0; i < header_.size(); ++i) {
        if (header_[i] == name) return static_cast<int>(i);
    }
    return -1;
}

const std::string& Table::at(std::size_t row, int col) const {
    if (col < 0 || row >= rows_.size()) return kEmpty;
    const std::vector<std::string>& r = rows_[row];
    const auto c = static_cast<std::size_t>(col);
    return (c < r.size()) ? r[c] : kEmpty;
}

std::string Table::get(std::size_t row, const std::string& name) const {
    return at(row, column(name));
}

bool Table::is_blank(std::size_t row, const std::string& name) const {
    return at(row, column(name)).empty();
}

double Table::num(std::size_t row, const std::string& name, double fallback) const {
    const std::string& s = at(row, column(name));
    if (s.empty()) return fallback;
    try {
        std::size_t used = 0;
        const double v = std::stod(s, &used);
        if (used == 0) return fallback;
        return v;
    } catch (...) {
        return fallback;
    }
}

void Table::add_row(std::vector<std::string> row) {
    row.resize(header_.size());
    rows_.push_back(std::move(row));
}

std::vector<std::string> Table::unique(const std::string& name) const {
    const int c = column(name);
    std::vector<std::string> out;
    if (c < 0) return out;
    std::unordered_set<std::string> seen;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        const std::string& v = at(i, c);
        if (seen.insert(v).second) out.push_back(v);
    }
    return out;
}

}  // namespace elanora::data
