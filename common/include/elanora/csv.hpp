#pragma once
//
// Minimal RFC 4180 CSV reader/writer.
//
// Deliberately small. Every dataset in ELANORA passes through here, and the
// files are meant to be openable in any spreadsheet or analysis tool, so the
// format is boring on purpose: comma delimiter, '\n' line endings, '.' decimal
// separator, header row always present.

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace elanora {

// Format a double at 6 decimal places -- fixed, never scientific notation, so
// column widths stay predictable and no downstream parser has to cope with
// "1e-07". Non-finite values are written as empty (missing), never as "nan",
// because "nan" silently becomes a string column in most tools.
std::string fmt6(double v);

// Quote a field only if it needs it (contains comma, quote, CR or LF).
std::string csv_escape(const std::string& field);

class CsvError : public std::runtime_error {
public:
    explicit CsvError(const std::string& what) : std::runtime_error(what) {}
};

// ---------------------------------------------------------------------------

class CsvWriter {
public:
    // Creates parent directories as needed and writes the header immediately.
    CsvWriter(const std::filesystem::path& path, std::vector<std::string> header);

    // Throws CsvError if the row width does not match the header width. A
    // ragged CSV is far more painful to diagnose three months later than a
    // loud failure at write time.
    void row(const std::vector<std::string>& fields);

    // Convenience for all-numeric rows.
    void row(const std::vector<double>& values);

    void flush();
    std::size_t columns() const { return header_.size(); }
    std::size_t rows_written() const { return rows_; }

private:
    std::ofstream            out_;
    std::vector<std::string> header_;
    std::size_t              rows_ = 0;
    std::filesystem::path    path_;
};

// ---------------------------------------------------------------------------

class CsvReader {
public:
    // Throws CsvError if the file does not exist or is empty.
    explicit CsvReader(const std::filesystem::path& path);

    const std::vector<std::string>& header() const { return header_; }

    // Reads the next data row. Returns false at end of file. Blank trailing
    // lines are skipped rather than reported as empty rows.
    bool next(std::vector<std::string>& out);

    // Column index by name, or -1 if absent.
    int column(const std::string& name) const;

private:
    std::ifstream            in_;
    std::vector<std::string> header_;
    std::filesystem::path    path_;
};

// Split one CSV line into fields, honouring quotes. Exposed for testing.
std::vector<std::string> csv_split_line(const std::string& line);

}  // namespace elanora
