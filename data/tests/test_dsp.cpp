// The DSP layer underpins every feature in the project. If Welch normalisation
// is wrong, every absolute band power is wrong by the same factor and nothing
// downstream can detect it -- the numbers stay self-consistent and plausible.
// These tests pin the absolute scale to a signal whose power is known exactly.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numeric>

#include "elanora/data/dsp.hpp"
#include "test_helpers.hpp"

using namespace elanora;
using namespace elanora::data;
using namespace elanora::test;
using Catch::Approx;

TEST_CASE("the FFT round-trips through its own inverse") {
    std::vector<double> re = {1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<double> im(8, 0.0);
    const std::vector<double> original = re;

    fft_radix2(re, im, false);
    fft_radix2(re, im, true);

    for (std::size_t i = 0; i < re.size(); ++i) {
        REQUIRE(re[i] == Approx(original[i]).margin(1e-9));
        REQUIRE(im[i] == Approx(0.0).margin(1e-9));
    }
}

TEST_CASE("integrating the PSD recovers the signal's power") {
    // A unit-amplitude sine has mean square 1/2. If this scale is wrong every
    // absolute band power in the dataset is wrong by the same silent factor.
    const std::vector<double> s = make_sine(10.0, 256, 30.0, 1.0);
    const Psd psd = welch_psd(s, 256.0, 1024);
    REQUIRE_FALSE(psd.empty());

    double total = 0.0;
    for (double p : psd.power) total += p * psd.df;
    REQUIRE(total == Approx(0.5).epsilon(0.05));
}

TEST_CASE("PSD power scales with amplitude squared") {
    const Psd a = welch_psd(make_sine(10.0, 256, 30.0, 1.0), 256.0, 1024);
    const Psd b = welch_psd(make_sine(10.0, 256, 30.0, 2.0), 256.0, 1024);
    const double pa = band_power(a, 8.0, 13.0);
    const double pb = band_power(b, 8.0, 13.0);
    REQUIRE(pb == Approx(4.0 * pa).epsilon(0.05));
}

TEST_CASE("band power lands in the band that contains the tone") {
    const Psd psd = welch_psd(make_sine(10.0, 256, 30.0), 256.0, 1024);
    REQUIRE(band_power(psd, 8.0, 13.0) > 10.0 * band_power(psd, 13.0, 30.0));
    REQUIRE(band_power(psd, 8.0, 13.0) > 10.0 * band_power(psd, 4.0, 8.0));
}

TEST_CASE("a band edge falling mid-bin does not quantise the result") {
    // Two adjacent bands that abut must sum to the band spanning both. Whole-bin
    // integration would double count or drop the boundary bin.
    const Psd psd = welch_psd(make_noise(256, 30.0, 1.0), 256.0, 1024);
    const double lower = band_power(psd, 4.0, 8.0);
    const double upper = band_power(psd, 8.0, 13.0);
    const double both  = band_power(psd, 4.0, 13.0);
    REQUIRE(lower + upper == Approx(both).epsilon(1e-9));
}

TEST_CASE("the periodogram resolves a slow oscillation Welch would smear") {
    // 0.2 Hz over 30 s: this is the breathing case, and the reason breathing
    // uses a single transform rather than averaged segments.
    const std::vector<double> s = make_sine(0.2, 52, 30.0);
    const Psd p = periodogram(s, 52.0);
    REQUIRE(peak_frequency(p, 0.1, 0.5) == Approx(0.2).margin(0.02));
}

TEST_CASE("parabolic interpolation beats the raw bin spacing") {
    // 0.23 Hz does not sit on a bin centre for a 30 s record. Without
    // interpolation the answer would snap to the nearest bin.
    const Psd p = periodogram(make_sine(0.23, 52, 30.0), 52.0);
    const double f = peak_frequency(p, 0.1, 0.5);
    REQUIRE(f == Approx(0.23).margin(0.01));
}

TEST_CASE("a wide Butterworth band-pass is flat in its passband") {
    // 1-45 Hz is the band the EEG chain actually uses. A tone in the middle of
    // a wide band must come through at essentially full amplitude.
    const std::vector<double> mid = make_sine(10.0, 256, 4.0, 1.0);
    REQUIRE(rms_of(bandpass(mid, 1.0, 45.0, 256.0)) == Approx(0.707).margin(0.02));
}

TEST_CASE("the band-pass rejects hard outside its band") {
    // A 4th-order pair at 45 Hz puts 60 Hz about 0.031 RMS through in theory.
    // The measured figure is slightly above that because mirror padding leaves
    // a small edge artifact; 0.05 is the bound that residual has to stay under.
    const std::vector<double> mains = make_sine(60.0, 256, 4.0, 1.0);
    const std::vector<double> drift = make_sine(0.05, 256, 4.0, 1.0);
    REQUIRE(rms_of(bandpass(mains, 1.0, 45.0, 256.0)) < 0.05);
    REQUIRE(rms_of(bandpass(drift, 1.0, 45.0, 256.0)) < 0.05);
}

TEST_CASE("padding does not drag stopband energy back into the window") {
    // Regression guard. Point reflection offsets the pad by 2*x[endpoint]; the
    // high-pass rings on that step, the forward pass leaves the ringing just
    // outside the data, and the reverse pass pulls it back in. That bug leaked
    // 0.154 here against a true value near 0.031, and grew with a longer pad --
    // so a length sweep is the check that would have caught it.
    for (double seconds : {2.0, 4.0, 8.0}) {
        const double leaked = rms_of(bandpass(make_sine(60.0, 256, seconds, 1.0),
                                              1.0, 45.0, 256.0));
        REQUIRE(leaked < 0.05);
    }
}

TEST_CASE("a narrow band costs passband amplitude, which is why bands are split spectrally") {
    // 8-13 Hz is narrower than a 4th-order Butterworth's transition width, so
    // even the band centre sits about 2.3 dB down after the forward-backward
    // pass. That is correct filter behaviour, not a defect -- and it is exactly
    // why band_powers() integrates a PSD instead of running a filter bank.
    // Filtering into five narrow bands would attenuate each one differently and
    // the relative powers would no longer sum to the total.
    const std::vector<double> tone = make_sine(10.0, 256, 4.0, 1.0);
    const double narrow = rms_of(bandpass(tone, 8.0, 13.0, 256.0));
    const double wide   = rms_of(bandpass(tone, 1.0, 45.0, 256.0));

    REQUIRE(narrow < wide);
    REQUIRE(narrow == Approx(0.54).margin(0.03));

    // The filters used on real signals are all wide enough for this loss to be
    // negligible: heart spans 0.5-4 Hz and breathing 0.1-0.5 Hz, both 5:1 or
    // better, where the centre passes essentially untouched.
    REQUIRE(rms_of(bandpass(make_sine(1.2, 64, 30.0, 1.0), 0.5, 4.0, 64.0))
            == Approx(0.707).margin(0.05));
    REQUIRE(rms_of(bandpass(make_sine(0.2, 52, 60.0, 1.0), 0.1, 0.5, 52.0))
            == Approx(0.707).margin(0.05));
}

TEST_CASE("filtfilt is zero phase, so a peak does not move") {
    // A causal filter shifts every peak by its group delay. At 0.1 Hz that is
    // seconds, which would bias every breathing interval identically -- a bias
    // no downstream check could see.
    std::vector<double> s = make_sine(1.0, 256, 8.0);
    const std::vector<Biquad> sos = butter_bandpass(4, 0.5, 4.0, 256.0);

    const std::vector<double> zp = filtfilt(sos, s);

    auto argmax_near_start = [](const std::vector<double>& v, std::size_t upto) {
        std::size_t best = 0;
        for (std::size_t i = 1; i < upto && i < v.size(); ++i) {
            if (v[i] > v[best]) best = i;
        }
        return best;
    };
    // The first peak of a 1 Hz sine at 256 Hz sits at sample 64.
    const std::size_t peak = argmax_near_start(zp, 200);
    REQUIRE(static_cast<int>(peak) == Approx(64).margin(4));
}

TEST_CASE("linear detrend removes a ramp without touching the oscillation") {
    std::vector<double> s = make_sine(10.0, 256, 4.0, 1.0);
    for (std::size_t i = 0; i < s.size(); ++i) {
        s[i] += 0.01 * static_cast<double>(i);   // drift
    }
    detrend_linear(s);
    REQUIRE(mean_of(s) == Approx(0.0).margin(1e-9));
    REQUIRE(rms_of(s) == Approx(0.707).margin(0.05));
}

TEST_CASE("a gap is interpolated and reported, never counted as silence") {
    std::vector<double> s = make_sine(10.0, 256, 4.0, 1.0);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (int i = 100; i < 110; ++i) s[static_cast<std::size_t>(i)] = nan;

    const double missing = fill_gaps(s);
    REQUIRE(missing == Approx(10.0 / static_cast<double>(s.size())).margin(1e-9));
    REQUIRE(all_finite(s));
    // Interpolation stays on the signal's own scale rather than snapping to 0,
    // which would inject a step edge and a broadband spike with it.
    for (int i = 100; i < 110; ++i) REQUIRE(std::abs(s[static_cast<std::size_t>(i)]) <= 1.01);
}

TEST_CASE("an all-NaN window becomes zeros and reports itself fully missing") {
    std::vector<double> s(256, std::numeric_limits<double>::quiet_NaN());
    REQUIRE(fill_gaps(s) == Approx(1.0));
    REQUIRE(all_finite(s));
    REQUIRE(rms_of(s) == Approx(0.0));
}

TEST_CASE("leading and trailing gaps are held, not extrapolated") {
    std::vector<double> s = {std::numeric_limits<double>::quiet_NaN(), 2.0, 4.0,
                             std::numeric_limits<double>::quiet_NaN()};
    fill_gaps(s);
    REQUIRE(s[0] == Approx(2.0));
    REQUIRE(s[3] == Approx(4.0));
}

TEST_CASE("degenerate inputs return empty rather than garbage") {
    REQUIRE(welch_psd({}, 256.0).empty());
    REQUIRE(welch_psd(make_sine(10, 256, 1.0), 0.0).empty());
    REQUIRE(periodogram({1.0, 2.0}, 52.0).empty());
    REQUIRE(butter_lowpass(3, 10.0, 256.0).empty());     // odd order unsupported
    REQUIRE(butter_lowpass(4, 200.0, 256.0).empty());    // cutoff above Nyquist
    REQUIRE(band_power(Psd{}, 1.0, 4.0) == Approx(0.0));
}

TEST_CASE("median resists a one-sided artifact that would move a mean") {
    std::vector<double> x = {1, 1, 1, 1, 1, 1, 1, 1, 1, 500};
    REQUIRE(median_of(x) == Approx(1.0));
    REQUIRE(mean_of(x) > 50.0);
}
