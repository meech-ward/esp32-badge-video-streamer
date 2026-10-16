// Software auto white balance: pick near-neutral pixels from a small RGB888
// image and steer the sensor's manual R/G/B gains towards R = G = B. The
// controller works in OV2640 DSP units (0xCC/0xCD/0xCE, 0x40 ~ unity); the
// OV5640 takes the same numbers through ov5640_gain() below. Pure C++, no
// ESP-IDF; host-tested in host_test/test_awb.cpp.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace streamer::awb {

// --------------------------------------------------------------- analysis ----

struct AnalyseConfig {
    int dark = 24;        // luma below this: too noisy to judge colour
    int bright = 245;     // any channel at or above this: may be clipped
    float loose = 0.6f;   // pass 1: (max - min) / luma limit
    float strict = 0.2f;  // pass 2: same limit, after removing the pass-1 cast
    // Pass 1 uses only raw-neutral pixels (raw chroma/luma < strict) when at
    // least this many / this share of the usable pixels are raw-neutral.
    uint32_t seed_min = 40;
    float seed_fraction = 0.05f;
};

struct Means {
    float r = 0, g = 0, b = 0;  // mean 8-bit values (gamma-encoded) of the neutral pixels
    uint32_t neutral = 0;       // pixels used
    uint32_t total = 0;         // pixels looked at
};

namespace detail {
struct Px {
    int r, g, b, y;
};
inline bool usable(const uint8_t* p, const AnalyseConfig& c, Px& out) {
    const int r = p[0], g = p[1], b = p[2];
    if (std::max({r, g, b}) >= c.bright) return false;
    const int y = (r + 2 * g + b) / 4;
    if (y < c.dark) return false;
    out = {r, g, b, y};
    return true;
}
}  // namespace detail

// Means of the near-neutral pixels of an RGB888 image (R, G, B byte order).
// WHY two passes: the frame carries the very cast being corrected, so grey
// pixels are themselves tinted (G/avg up to ~1.2 seen; a far-off manual gain
// gave 0.64). Pass 1 drops dark, clipped and strongly coloured pixels (a green
// LED display, a red toy) with a loose limit and estimates the cast; pass 2
// removes that cast from each pixel before the strict neutral test, so the
// test means "grey under this light" rather than "grey in the raw image".
// WHY pass 1 prefers raw-neutral pixels: the loose set's mean is "the
// dominant mildly coloured area", so a large tinted surface (a wall lit by a
// green display, chroma/luma ~0.4) would become the cast estimate and pass 2
// would keep that surface and reject the real grey. Raw-neutral pixels are
// only absent when the cast itself is strong (the loop starting far off), and
// then the loose estimate is the best available; once the loop has removed
// most of the cast, greys are raw-neutral and anchor the estimate.
inline Means analyse(const uint8_t* rgb, size_t pixels, const AnalyseConfig& c = {}) {
    Means m;
    m.total = static_cast<uint32_t>(pixels);
    uint64_t sl[3] = {0, 0, 0}, ss[3] = {0, 0, 0};  // loose / raw-strict sums
    uint32_t nl = 0, ns = 0, usable = 0;
    detail::Px p;
    for (size_t i = 0; i < pixels; ++i) {
        if (!detail::usable(rgb + 3 * i, c, p)) continue;
        ++usable;
        const float chroma = static_cast<float>(std::max({p.r, p.g, p.b}) - std::min({p.r, p.g, p.b}));
        if (chroma > c.loose * static_cast<float>(p.y)) continue;
        sl[0] += static_cast<uint64_t>(p.r);
        sl[1] += static_cast<uint64_t>(p.g);
        sl[2] += static_cast<uint64_t>(p.b);
        ++nl;
        if (chroma > c.strict * static_cast<float>(p.y)) continue;
        ss[0] += static_cast<uint64_t>(p.r);
        ss[1] += static_cast<uint64_t>(p.g);
        ss[2] += static_cast<uint64_t>(p.b);
        ++ns;
    }
    const bool seed_strict = ns >= c.seed_min && static_cast<float>(ns) >= c.seed_fraction * static_cast<float>(usable);
    const uint64_t* s1 = seed_strict ? ss : sl;
    const uint32_t n1 = seed_strict ? ns : nl;
    if (n1 == 0 || s1[0] == 0 || s1[1] == 0 || s1[2] == 0) return m;
    const float avg = static_cast<float>(s1[0] + s1[1] + s1[2]) / 3.0f;
    const float kr = avg / static_cast<float>(s1[0]);
    const float kg = avg / static_cast<float>(s1[1]);
    const float kb = avg / static_cast<float>(s1[2]);

    uint64_t s2[3] = {0, 0, 0};
    uint32_t n2 = 0;
    for (size_t i = 0; i < pixels; ++i) {
        if (!detail::usable(rgb + 3 * i, c, p)) continue;
        const float r = static_cast<float>(p.r) * kr, g = static_cast<float>(p.g) * kg,
                    b = static_cast<float>(p.b) * kb;
        const float y = (r + 2 * g + b) / 4;
        if (std::max({r, g, b}) - std::min({r, g, b}) > c.strict * y) continue;
        s2[0] += static_cast<uint64_t>(p.r);
        s2[1] += static_cast<uint64_t>(p.g);
        s2[2] += static_cast<uint64_t>(p.b);
        ++n2;
    }
    m.neutral = n2;
    if (n2) {
        m.r = static_cast<float>(s2[0]) / static_cast<float>(n2);
        m.g = static_cast<float>(s2[1]) / static_cast<float>(n2);
        m.b = static_cast<float>(s2[2]) / static_cast<float>(n2);
    }
    return m;
}

// ------------------------------------------------------------- controller ----

struct Gains {
    float r, g, b;  // OV2640 DSP register units (0x40 is roughly unity)
};

// Starting point: the esp32-camera "office" preset 52/41/66 with green raised
// to 0x4A, between the two neutral points measured on the device (0x4E-0x52
// under one light, below 0x41 under another), so either light converges in a
// few steps.
constexpr Gains kStartGains = {0x52, 0x4A, 0x66};

struct ControllerConfig {
    // Fraction of the log error removed per step. WHY 0.25: on the device the
    // measured G/avg(R,B) moved as gain^2.1 (0xCD 0x41 -> 0.636, 0x52 ->
    // 1.037), so one step shrinks the error by about 1 - 0.25 * 2.1 = 0.48 with
    // no overshoot; it stays stable (if oscillating) up to an exponent of 8.
    float k = 0.25f;
    // Spread of the channel logs (max - min, i.e. the largest pairwise ratio
    // such as G/R; G/avg(R,B) is within it) below `deadband` -> settle and stop
    // moving; only a spread above `deadband * resume` moves the gains again.
    // WHY 0.03: a 1-LSB gain step near 0x40 changes a ratio by ~3 % (1.6 % x
    // 2.1), so a tighter band would hunt between two integer codes (seen in
    // simulation from 0.02 at exponent 2.1 and from 0.03 at exponent 3).
    float deadband = 0.03f;
    float resume = 1.5f;
    float max_step = 0.08f;  // largest log change per channel per step (~8 %)
    float min_gain = 0x20, max_gain = 0xC0;
    uint32_t min_pixels = 40;    // fewer neutral pixels than this -> skip
    float min_fraction = 0.05f;  // ... or fewer than this share of the image
};

enum class Result : uint8_t { Adjusted, Settled, TooFew };

// Moves the three gains in log space so that the neutral means become equal.
// The corrections sum to zero in log space, so the geometric mean of the gains
// (overall brightness) stays put and the sensor's AEC/AGC is not fought; only
// a channel pinned at a gain limit can move it.
class Controller {
public:
    explicit Controller(Gains start = kStartGains, ControllerConfig cfg = {}) : cfg_(cfg) { reset(start); }

    void reset(Gains start) {
        lg_[0] = std::log(clampg(start.r));
        lg_[1] = std::log(clampg(start.g));
        lg_[2] = std::log(clampg(start.b));
        settled_ = false;
    }

    Result step(const Means& m) {
        const float need = std::max(static_cast<float>(cfg_.min_pixels),
                                    cfg_.min_fraction * static_cast<float>(m.total));
        if (static_cast<float>(m.neutral) < need || m.r <= 0 || m.g <= 0 || m.b <= 0) return Result::TooFew;
        const float lm[3] = {std::log(m.r), std::log(m.g), std::log(m.b)};
        const float mean = (lm[0] + lm[1] + lm[2]) / 3;
        float e[3], worst = 0;
        for (int c = 0; c < 3; ++c) {
            e[c] = lm[c] - mean;
            worst = std::max(worst, std::fabs(e[c]));
        }
        const float spread = std::max({lm[0], lm[1], lm[2]}) - std::min({lm[0], lm[1], lm[2]});
        const float band = settled_ ? cfg_.deadband * cfg_.resume : cfg_.deadband;
        if (spread < band) {
            settled_ = true;
            return Result::Settled;
        }
        settled_ = false;
        // The errors sum to zero, so these steps do too. A large step is
        // scaled down as a whole (not clipped per channel) to keep that sum.
        const float scale = std::min(1.0f, cfg_.max_step / (cfg_.k * worst));
        const float lo = std::log(cfg_.min_gain), hi = std::log(cfg_.max_gain);
        for (int c = 0; c < 3; ++c) lg_[c] = std::clamp(lg_[c] - cfg_.k * e[c] * scale, lo, hi);
        return Result::Adjusted;
    }

    Gains gains() const { return {std::exp(lg_[0]), std::exp(lg_[1]), std::exp(lg_[2])}; }
    // Register value for channel 0 = R, 1 = G, 2 = B.
    uint8_t reg(int c) const {
        const float v = std::round(std::exp(lg_[std::clamp(c, 0, 2)]));
        return static_cast<uint8_t>(std::clamp(v, cfg_.min_gain, cfg_.max_gain));
    }
    bool settled() const { return settled_; }
    const ControllerConfig& config() const { return cfg_; }

private:
    float clampg(float v) const { return std::clamp(v, cfg_.min_gain, cfg_.max_gain); }

    ControllerConfig cfg_;
    float lg_[3] = {0, 0, 0};  // log gains; float so sub-LSB steps accumulate
    bool settled_ = false;
};

// ------------------------------------------------------------ freshness ----

// Decides whether a sampled frame already shows the last gain write.
// WHY: the frame a sample returns can be several frame periods old (up to two
// finished frames wait in the driver queue, plus the send before the tap), so
// at low fps it may predate the previous write. Feeding the controller such a
// frame makes it correct the same error twice: with 2-3 steps of measurement
// delay the loop overshoots and limit-cycles. Times are the esp_timer clock;
// the frame time is the driver's capture-start timestamp.
class FreshGate {
public:
    explicit FreshGate(int64_t settle_us) : settle_us_(settle_us) {}
    void wrote(int64_t now_us) { ready_us_ = now_us + settle_us_; }
    bool fresh(int64_t frame_start_us) const { return frame_start_us >= ready_us_; }

private:
    int64_t settle_us_;
    int64_t ready_us_ = INT64_MIN;
};

// ------------------------------------------------------------- registers ----
// The manual-gain registers camera_source writes, per sensor, kept here so the
// unit conversion and sensor mapping stay together and are host-testable.

namespace ov2640 {
// DSP bank (esp32-camera's (bank << 8) | addr form, bank 0): 8-bit gains,
// 0x40 ~ 1.0x; 0xC7 bit 6 = manual gains in use.
constexpr int kGainReg[3] = {0x0CC, 0x0CD, 0x0CE};  // R, G, B
constexpr int kCtrlReg = 0x0C7;
constexpr int kManualBit = 0x40;
}  // namespace ov2640

namespace ov5640 {
// 16-bit addresses: AWB_R_GAIN 0x3400/0x3401, AWB_G_GAIN 0x3402/0x3403,
// AWB_B_GAIN 0x3404/0x3405, 12 bits each with the high nibble in the even
// register, 0x400 = 1.0x; AWB_MANUAL_CONTROL 0x3406 bit 0 = manual gains.
constexpr int kGainReg[3] = {0x3400, 0x3402, 0x3404};  // even (high) register of each pair
constexpr int kCtrlReg = 0x3406;
constexpr int kManualBit = 0x01;
constexpr unsigned kGainMax = 0xFFF;
}  // namespace ov5640

// OV2640 DSP units to an OV5640 12-bit gain. WHY << 4: both sensors put 1.0x
// at bit 6 of their mantissa (0x40 vs 0x400), and esp32-camera's light presets
// are the same numbers on both (office R/G/B 0x52/0x41/0x66 on the OV2640,
// 0x520/0x410/0x660 on the OV5640), so the controller, its start gains and
// its 0x20-0xC0 limits carry over unchanged. Clamped to 12 bits for callers
// with wider values; a uint8_t (at most 0xFF0) never clips.
constexpr uint16_t ov5640_gain(unsigned v) { return static_cast<uint16_t>(std::min(v << 4, ov5640::kGainMax)); }

}  // namespace streamer::awb
