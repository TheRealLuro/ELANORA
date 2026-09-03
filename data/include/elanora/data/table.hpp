#pragma once
//
// An in-memory CSV table.
//
// CsvReader streams a row at a time, which is right for writing a session and
// wrong for analysis: every step from here on needs to join tables, pivot long
// to wide, and select a column by name. The datasets involved are thousands of
// rows, not millions, so holding one in memory costs nothing worth optimising.

#include <filesystem>
#include <string>
#include <vector>

namespace elanora::data {

class Table {
public:
    Table() = default;
    explicit Table(std::vector<std::string> header) : header_(std::move(header)) {}

    // Throws CsvError if the file is missing or empty.
    static Table read(const std::filesystem::path& path);

    // Returns an empty table instead of throwing when the file is absent, which
    // is the common case for a dataset that has not been built yet.
    static Table read_or_empty(const std::filesystem::path& path);

    void write(const std::filesystem::path& path) const;

    const std::vector<std::string>& header() const { return header_; }
    std::size_t rows() const { return rows_.size(); }
    std::size_t cols() const { return header_.size(); }
    bool empty() const { return rows_.empty(); }

    // -1 when the column is absent. Callers are expected to check: a missing
    // column means the file was written by an older build, and silently
    // treating it as zero would corrupt an analysis without any visible error.
    int column(const std::string& name) const;
    bool has(const std::string& name) const { return column(name) >= 0; }

    const std::string& at(std::size_t row, int col) const;
    std::string get(std::size_t row, const std::string& name) const;

    // Parsed access. `fallback` is returned for a missing column, a blank
    // field, or text that is not a number -- blank is how fmt6 writes a
    // non-finite value, so it has to round-trip as missing rather than as zero.
    double num(std::size_t row, const std::string& name, double fallback = 0.0) const;
    bool   is_blank(std::size_t row, const std::string& name) const;

    void add_row(std::vector<std::string> row);
    const std::vector<std::string>& row(std::size_t i) const { return rows_[i]; }

    // Every distinct value of a column, in first-seen order. Used to group by
    // session or subject without disturbing the file's ordering.
    std::vector<std::string> unique(const std::string& name) const;

private:
    std::vector<std::string> header_;
    std::vector<std::vector<std::string>> rows_;
};

}  // namespace elanora::data
