#include "elanora/csv.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace elanora {

namespace fs = std::filesystem;

std::string fmt6(double v) {
    if (!std::isfinite(v)) return {};  // missing, not "nan"
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    return std::string(buf);
}

std::string csv_escape(const std::string& field) {
    const bool needs_quotes =
        field.find_first_of(",\"\r\n") != std::string::npos;
    if (!needs_quotes) return field;

    std::string out;
    out.reserve(field.size() + 2);
    out.push_back('"');
    for (char c : field) {
        if (c == '"') out.push_back('"');  // doubled per RFC 4180
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::vector<std::string> csv_split_line(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;
    bool in_quotes = false;

    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (in_quotes) {
            if (c == '"') {
                // A doubled quote is a literal quote; a lone one closes.
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    current.push_back('"');
                    ++i;
                } else {
                    in_quotes = false;
                }
            } else {
                current.push_back(c);
            }
        } else if (c == '"') {
            in_quotes = true;
        } else if (c == ',') {
            fields.push_back(current);
            current.clear();
        } else if (c != '\r') {  // tolerate CRLF input
            current.push_back(c);
        }
    }
    fields.push_back(current);
    return fields;
}

// ---------------------------------------------------------------------------

CsvWriter::CsvWriter(const fs::path& path, std::vector<std::string> header)
    : header_(std::move(header)), path_(path) {
    if (header_.empty()) {
        throw CsvError("CsvWriter: header must not be empty: " + path.string());
    }
    if (path.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
    }
    // Binary mode so Windows cannot silently turn '\n' into "\r\n"; the line
    // ending is written explicitly and identically on every platform.
    out_.open(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out_) {
        throw CsvError("CsvWriter: cannot open for writing: " + path.string());
    }
    for (std::size_t i = 0; i < header_.size(); ++i) {
        if (i) out_.put(',');
        out_ << csv_escape(header_[i]);
    }
    out_.put('\n');
}

void CsvWriter::row(const std::vector<std::string>& fields) {
    if (fields.size() != header_.size()) {
        throw CsvError("CsvWriter: row has " + std::to_string(fields.size()) +
                       " fields but header has " + std::to_string(header_.size()) +
                       " in " + path_.string());
    }
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i) out_.put(',');
        out_ << csv_escape(fields[i]);
    }
    out_.put('\n');
    ++rows_;
}

void CsvWriter::row(const std::vector<double>& values) {
    std::vector<std::string> fields;
    fields.reserve(values.size());
    for (double v : values) fields.push_back(fmt6(v));
    row(fields);
}

void CsvWriter::flush() { out_.flush(); }

// ---------------------------------------------------------------------------

CsvReader::CsvReader(const fs::path& path) : path_(path) {
    if (!fs::exists(path)) {
        throw CsvError("CsvReader: file does not exist: " + path.string());
    }
    in_.open(path, std::ios::in | std::ios::binary);
    if (!in_) {
        throw CsvError("CsvReader: cannot open for reading: " + path.string());
    }
    std::string line;
    if (!std::getline(in_, line)) {
        throw CsvError("CsvReader: file is empty: " + path.string());
    }
    header_ = csv_split_line(line);
}

bool CsvReader::next(std::vector<std::string>& out) {
    std::string line;
    while (std::getline(in_, line)) {
        // Skip blank trailing lines rather than emitting phantom rows.
        if (line.empty() || line == "\r") continue;
        out = csv_split_line(line);
        return true;
    }
    return false;
}

int CsvReader::column(const std::string& name) const {
    const auto it = std::find(header_.begin(), header_.end(), name);
    if (it == header_.end()) return -1;
    return static_cast<int>(std::distance(header_.begin(), it));
}

}  // namespace elanora
