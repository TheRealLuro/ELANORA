#pragma once
//
// Turns recorded raw CSVs into the feature and ML tables (Task 20).
//
// Everything this writes is regenerable from data/datasets/raw/. Nothing here
// ever modifies a raw file. When the feature definitions change -- and they
// will -- the fix is to rerun this, not to re-record a subject.
//
// Two stages, deliberately separate:
//
//   build_features()     raw -> long tables, one row per trial x sensor x period
//   build_ml_datasets()  long -> wide tables, one row per trial x sensor
//
// The long tables are what a person reads to check a session went well; the
// wide tables are what the models consume. Keeping them apart means a change to
// the response math does not force the expensive spectral work to run again.

#include <filesystem>
#include <string>
#include <vector>

namespace elanora::data {

struct BuildReport {
    int trials_seen      = 0;
    int trials_processed = 0;
    int trials_skipped   = 0;

    int brain_rows  = 0;
    int heart_rows  = 0;
    int breath_rows = 0;

    // Anything that caused a trial to be skipped or a value to be dropped.
    // Trials are never discarded silently: an unexplained row count is
    // indistinguishable from a bug.
    std::vector<std::string> warnings;

    bool ok() const { return trials_processed > 0; }
};

// `root` is the datasets directory -- the one holding trials.csv and raw/.
BuildReport build_features(const std::filesystem::path& root);
BuildReport build_ml_datasets(const std::filesystem::path& root);

// Both stages in order, with the reports merged.
BuildReport build_all(const std::filesystem::path& root);

// Guards log() against a band with no power at all. Small enough to be far
// below any real measurement, large enough that its log is not -700.
inline constexpr double kLogEpsilon = 1e-12;

// The centred log-ratio of a composition.
//
// clr(x)_i = log(x_i) - mean_j log(x_j)
//
// This is what makes relative power modellable. The five relative powers sum to
// one, so their five deltas sum to zero by construction and "alpha up, beta
// down" can be pure normalisation artifact. CLR removes the sum-to-one bound
// and makes ratios additive and Euclidean distance meaningful.
//
// It does NOT remove the linear dependency: CLR vectors sum to zero and their
// covariance stays singular. CLR fixes the geometry; only absolute log-power
// escapes the constraint, which is why absolute is the default modelling
// target and CLR is the secondary one.
std::vector<double> clr(const std::vector<double>& composition);

}  // namespace elanora::data
