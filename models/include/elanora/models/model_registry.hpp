#pragma once
//
// The six models (Task 28).
//
//   Model 1-4  one per EEG sensor, each predicting the change in all 5 bands
//   Model 5    heart, predicting the change in BPM
//   Model 6    breathing, predicting the change in breaths per minute
//
// Six logical models, 22 single-output regressors underneath: a GP is
// single-output by construction, so each brain model owns five. That is an
// implementation detail everywhere except the optimizer, which scores all 22
// outputs against one candidate frequency.
//
// Only `stim` rows train these. Control rows exist to answer "did the stimulus
// do anything at all", which is Milestone 4's question, not this one -- and a
// control has no frequency to regress against.

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "elanora/models/gp.hpp"
#include "elanora/models/ridge.hpp"
#include "elanora/models/validation.hpp"
#include "elanora/types.hpp"

namespace elanora::models {

enum class ModelId { BrainS1, BrainS2, BrainS3, BrainS4, Heart, Breathing };
inline constexpr int kModelCount = 6;

const char* model_id_name(ModelId m);
bool parse_model_id(const std::string& s, ModelId& out);
int  outputs_for(ModelId m);              // 5 for brain, 1 for heart and breathing
std::string output_name(ModelId m, int output_index);

// One trained regressor: the chosen rung, its fitted state, and everything the
// optimizer needs to weigh its prediction against the others.
struct TrainedOutput {
    ModelKind kind = ModelKind::MeanBaseline;
    Standardizer standardizer;
    Ridge ridge;                  // used by Ridge and PolyRidge
    GaussianProcess gp;
    double mean_fallback = 0.0;   // MeanBaseline's constant prediction

    CvResult cv_session;          // leave-one-session-out
    CvResult cv_subject;          // leave-one-subject-out
    bool has_subject_cv = false;

    // Spread of the training targets. The optimizer divides every goal term by
    // this so band powers (~0.01) and BPM (~5) are commensurable; without it
    // heart rate would silently dominate every score.
    double output_sd = 1.0;

    int  n_samples = 0;
    bool trained   = false;

    // Returns the predicted response and, through `sd`, how unsure it is.
    double predict(const VectorXd& x, double* sd = nullptr) const;

    // Mean and spread of the baseline feature across the training rows. The
    // caller uses these to tell whether a supplied baseline is a value these
    // models have ever seen.
    double baseline_mean = 0.0;
    double baseline_sd   = 1.0;

    // How many standard deviations `baseline` sits from the training mean.
    double baseline_z(double baseline) const;
};

struct TrainOptions {
    double min_quality_eeg       = 0.5;
    double min_quality_heart     = 0.5;
    double min_confidence_breath = 0.5;
    std::string response = "d";     // "d" | "post_d" | "recovery"
    double ladder_margin = 0.02;
};

class ModelRegistry {
public:
    // Reads ml/*.csv under `datasets` and trains all 22 outputs.
    void train_all(const std::filesystem::path& datasets, const TrainOptions& opt = {});

    bool save(const std::filesystem::path& dir, std::string& err) const;
    bool load(const std::filesystem::path& dir, std::string& err);

    bool trained(ModelId m) const;
    int  n_samples(ModelId m) const;
    const std::string& untrained_reason(ModelId m) const;

    const TrainedOutput& output(ModelId m, int output_index) const;
    CvResult  cv(ModelId m, int output_index) const;
    ModelKind kind(ModelId m, int output_index) const;
    double    output_sd(ModelId m, int output_index) const;

    // The observed frequency span of the training data, in Hz. The optimizer
    // clamps its search to this: GP variance saturates, so uncertainty alone
    // cannot prevent a confident-looking recommendation far outside the range
    // anything was ever measured at.
    std::pair<double, double> trained_freq_range() const;

    // Every frequency actually presented, sorted. Used for the extrapolation
    // penalty, which measures distance to the nearest one.
    const std::vector<double>& trained_frequencies() const { return freqs_; }

    // Standard deviation of the outcome measured on CONTROL trials only. This
    // is the effect-size floor: a predicted change smaller than the noise seen
    // when no rhythmic stimulus was present is not an effect.
    double control_sd(ModelId m, int output_index) const;

    bool any_trained() const;
    const TrainOptions& options() const { return opt_; }

private:
    struct ModelState {
        std::vector<TrainedOutput> outputs;
        std::string reason;
        int n_samples = 0;
    };

    ModelState& state(ModelId m) { return states_[static_cast<std::size_t>(m)]; }
    const ModelState& state(ModelId m) const { return states_[static_cast<std::size_t>(m)]; }

    std::array<ModelState, kModelCount> states_;
    std::array<std::vector<double>, kModelCount> control_sd_;
    std::vector<double> freqs_;
    TrainOptions opt_;
};

}  // namespace elanora::models
