#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>

#include "elanora/csv.hpp"

using namespace elanora;
namespace fs = std::filesystem;

namespace {

// Unique scratch path per test so a failure never leaves a file that makes the
// next run pass or fail for the wrong reason.
fs::path scratch(const std::string& stem) {
    static std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream name;
    name << "elanora_test_" << stem << "_" << rng() << ".csv";
    return fs::temp_directory_path() / name.str();
}

struct Cleanup {
    fs::path p;
    ~Cleanup() { std::error_code ec; fs::remove(p, ec); }
};

std::string read_all_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::in | std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

}  // namespace

TEST_CASE("fmt6 formats at six decimal places without scientific notation", "[csv]") {
    REQUIRE(fmt6(1.0 / 3.0) == "0.333333");
    REQUIRE(fmt6(0.0) == "0.000000");
    REQUIRE(fmt6(-2.5) == "-2.500000");
    // Tiny values must not collapse to "1e-07", which downstream tools read
    // as text rather than a number.
    REQUIRE(fmt6(1e-7).find('e') == std::string::npos);
}

TEST_CASE("fmt6 writes non-finite values as missing, not as nan", "[csv]") {
    // "nan" in a numeric column silently turns the whole column into strings
    // in most spreadsheet and analysis tools. Empty is unambiguous.
    REQUIRE(fmt6(std::nan("")).empty());
    REQUIRE(fmt6(std::numeric_limits<double>::infinity()).empty());
}

TEST_CASE("a three row file round-trips", "[csv]") {
    const auto path = scratch("roundtrip");
    Cleanup guard{path};

    {
        CsvWriter w(path, {"trial_id", "frequency_hz", "condition"});
        w.row(std::vector<std::string>{"001", fmt6(8.0), "stim"});
        w.row(std::vector<std::string>{"002", fmt6(11.3), "stim"});
        w.row(std::vector<std::string>{"003", fmt6(0.0), "control_tone"});
        REQUIRE(w.rows_written() == 3);
    }

    CsvReader r(path);
    REQUIRE(r.header() == std::vector<std::string>{"trial_id", "frequency_hz", "condition"});
    REQUIRE(r.column("condition") == 2);
    REQUIRE(r.column("not_a_column") == -1);

    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    while (r.next(row)) rows.push_back(row);

    REQUIRE(rows.size() == 3);
    REQUIRE(rows[0][0] == "001");
    REQUIRE(std::stod(rows[1][1]) == Catch::Approx(11.3));
    REQUIRE(rows[2][2] == "control_tone");
}

TEST_CASE("fields containing commas and quotes survive a round-trip", "[csv]") {
    const auto path = scratch("quoting");
    Cleanup guard{path};

    const std::string tricky = R"(slight movement, eyes open)";
    const std::string quoted = R"(subject said "too loud")";

    {
        CsvWriter w(path, {"trial_id", "notes"});
        w.row(std::vector<std::string>{"001", tricky});
        w.row(std::vector<std::string>{"002", quoted});
    }

    CsvReader r(path);
    std::vector<std::string> row;
    REQUIRE(r.next(row));
    REQUIRE(row.size() == 2);
    REQUIRE(row[1] == tricky);
    REQUIRE(r.next(row));
    REQUIRE(row[1] == quoted);
}

TEST_CASE("line endings are LF even on Windows", "[csv]") {
    // Opened in binary mode precisely so the platform cannot inject CRLF.
    // A mixed-ending dataset is a nuisance to diff and to parse.
    const auto path = scratch("endings");
    Cleanup guard{path};
    {
        CsvWriter w(path, {"a", "b"});
        w.row(std::vector<std::string>{"1", "2"});
    }
    const auto bytes = read_all_bytes(path);
    REQUIRE(bytes == "a,b\n1,2\n");
}

TEST_CASE("a ragged row is refused rather than silently written", "[csv]") {
    const auto path = scratch("ragged");
    Cleanup guard{path};
    CsvWriter w(path, {"a", "b", "c"});
    REQUIRE_THROWS_AS(w.row(std::vector<std::string>{"1", "2"}), CsvError);
}

TEST_CASE("reading a missing file throws rather than yielding nothing", "[csv]") {
    // Silently returning zero rows would make a mistyped path look like an
    // empty dataset, which is the hardest kind of bug to notice.
    REQUIRE_THROWS_AS(CsvReader(scratch("absent")), CsvError);
}

TEST_CASE("blank trailing lines do not become phantom rows", "[csv]") {
    const auto path = scratch("trailing");
    Cleanup guard{path};
    {
        std::ofstream out(path, std::ios::binary);
        out << "a,b\n1,2\n\n\n";
    }
    CsvReader r(path);
    std::vector<std::string> row;
    int count = 0;
    while (r.next(row)) ++count;
    REQUIRE(count == 1);
}

TEST_CASE("csv_split_line handles empty fields and trailing separators", "[csv]") {
    auto f = csv_split_line("a,,c,");
    REQUIRE(f.size() == 4);
    REQUIRE(f[1].empty());
    REQUIRE(f[3].empty());
}
