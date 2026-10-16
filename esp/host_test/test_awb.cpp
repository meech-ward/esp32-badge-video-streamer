// Host test for streamer/white_balance.hpp: neutral-pixel analysis on synthetic images,
// the gain controller against a simulated sensor (integer registers,
// measured ratio = cast * gain^p, p = 2.1 as measured on the OV2640), the
// OV2640-to-OV5640 gain conversion and the per-sensor register mapping.
#include <cmath>
#include <cstdint>
#include <array>
#include <cstdio>
#include <utility>
#include <vector>

#include "streamer/white_balance.hpp"

using namespace streamer::awb;

static int g_checks = 0;
static int g_fail = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_fail;                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

namespace {

struct Image {
    std::vector<uint8_t> px;
    void add(int r, int g, int b, int n) {
        for (int i = 0; i < n; ++i) {
            px.push_back(static_cast<uint8_t>(r));
            px.push_back(static_cast<uint8_t>(g));
            px.push_back(static_cast<uint8_t>(b));
        }
    }
    size_t count() const { return px.size() / 3; }
    Means analyse(const AnalyseConfig& c = {}) const { return streamer::awb::analyse(px.data(), count(), c); }
};

bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

// Neutrality metric used by these scene tests, not part of the firmware API.
float green_ratio(const Means& m) {
    const float rb = (m.r + m.b) / 2;
    return rb > 0 ? m.g / rb : 0;
}

// ------------------------------------------------------------ analysis ----

void test_grey_scene_with_cast() {
    Image im;
    im.add(100, 119, 100, 2000);  // grey card under the green cast (G/avg 1.19)
    im.add(160, 190, 158, 1000);  // white wall, same cast
    const Means m = im.analyse();
    CHECK(m.total == 3000 && m.neutral == 3000);
    CHECK(near(green_ratio(m), 1.19f, 0.02f));
}

void test_coloured_light_and_objects_are_excluded() {
    Image im;
    im.add(100, 119, 100, 2000);  // neutral, green cast
    im.add(20, 200, 40, 1500);    // green LED display: strongly saturated
    im.add(30, 250, 30, 500);     // its clipped core
    im.add(200, 40, 40, 400);     // red object
    im.add(8, 10, 8, 600);        // shadows: too dark
    im.add(250, 252, 251, 300);   // blown highlights
    const Means m = im.analyse();
    CHECK(m.total == 5300);
    CHECK(m.neutral == 2000);
    CHECK(near(m.r, 100, 0.01f) && near(m.g, 119, 0.01f) && near(m.b, 100, 0.01f));
    // Plain grey-world over every pixel would see a strong green bias.
    Image all = im;
    AnalyseConfig none;
    none.dark = 0;
    none.bright = 256;
    none.loose = none.strict = 100;
    CHECK(green_ratio(all.analyse(none)) > 1.5f);
}

void test_strong_cast_still_finds_grey() {
    // The far-off manual gains seen on the device (G/avg 0.64): grey pixels
    // have chroma/luma ~0.45, which only passes once the cast is removed.
    Image im;
    im.add(103, 66, 98, 1500);
    im.add(150, 96, 143, 500);
    im.add(60, 140, 50, 300);  // a mildly green object, also cast-shifted
    const Means m = im.analyse();
    CHECK(m.neutral == 2000);
    CHECK(near(green_ratio(m), 0.64f, 0.02f));
}

void test_tinted_surface_does_not_become_neutral() {
    // A grey wall plus a larger mildly green surface (chroma/luma ~0.4, e.g.
    // lit by a green display). Without the raw-neutral seed, pass 1 averaged
    // both, and once the tinted area dominated pass 2 kept only it (G/avg 1.46).
    for (int n : {1000, 2500, 4000, 8000}) {
        Image im;
        im.add(100, 100, 100, 2500);
        im.add(80, 120, 85, n);
        const Means m = im.analyse();
        CHECK(m.neutral == 2500);
        CHECK(near(green_ratio(m), 1.0f, 0.01f));
    }
}

void test_empty_and_unusable_images() {
    Image im;
    CHECK(im.analyse().neutral == 0);
    im.add(0, 0, 0, 100);
    im.add(255, 255, 255, 100);
    Means m = im.analyse();
    CHECK(m.total == 200 && m.neutral == 0 && m.r == 0);
    Image sat;
    sat.add(0, 200, 0, 100);  // a channel at zero cannot be divided by
    CHECK(sat.analyse().neutral == 0);
}

// ---------------------------------------------------------- controller ----

// A sensor that sees a scene with a fixed per-channel cast; the measured
// neutral mean of each channel scales as (gain / 0x40)^p, using the integer
// register the firmware would write.
struct Plant {
    float cast[3];
    float p;
    Means measure(const Controller& c, uint32_t neutral = 3000, uint32_t total = 4800) const {
        Means m;
        float v[3];
        for (int i = 0; i < 3; ++i) v[i] = 100.0f * cast[i] * std::pow(static_cast<float>(c.reg(i)) / 64.0f, p);
        m.r = v[0];
        m.g = v[1];
        m.b = v[2];
        m.neutral = neutral;
        m.total = total;
        return m;
    }
};

float worst_error(const Means& m) {
    const float mean = (std::log(m.r) + std::log(m.g) + std::log(m.b)) / 3;
    return std::max({std::fabs(std::log(m.r) - mean), std::fabs(std::log(m.g) - mean),
                     std::fabs(std::log(m.b) - mean)});
}

float log_mean_gain(const Controller& c) {
    const Gains g = c.gains();
    return (std::log(g.r) + std::log(g.g) + std::log(g.b)) / 3;
}

// Runs until settled; returns steps taken (or -1). `crossings` counts
// overshoots: a channel's log error changing sign while outside the deadband
// on both sides, i.e. a visible swing from one cast to the opposite one.
int converge(Controller& c, const Plant& p, int max_steps, int& crossings) {
    crossings = 0;
    float prev[3] = {0, 0, 0};
    const float band = c.config().deadband;
    for (int i = 0; i < max_steps; ++i) {
        const Means m = p.measure(c);
        const float lm[3] = {std::log(m.r), std::log(m.g), std::log(m.b)};
        const float mean = (lm[0] + lm[1] + lm[2]) / 3;
        for (int ch = 0; ch < 3; ++ch) {
            const float e = lm[ch] - mean;
            if (i && std::fabs(e) > band && std::fabs(prev[ch]) > band && (e > 0) != (prev[ch] > 0)) ++crossings;
            prev[ch] = e;
        }
        if (c.step(m) == Result::Settled) return i;
    }
    return -1;
}

void test_converges_from_green_cast_without_oscillation() {
    Controller c;
    // Scene that gives G/avg ~1.15 at equal gains: green-heavy sensor.
    const Plant plant{{0.92f, 1.10f, 0.98f}, 2.1f};
    const float ref = log_mean_gain(c);
    int crossings = 0;
    const int steps = converge(c, plant, 40, crossings);
    CHECK(steps > 0 && steps <= 15);  // at 3 Hz: settled within ~5 s
    CHECK(crossings == 0);
    const Means m = plant.measure(c);
    CHECK(worst_error(m) < c.config().deadband);
    CHECK(near(green_ratio(m), 1.0f, 0.05f));
    CHECK(near(log_mean_gain(c), ref, 0.01f));  // brightness reference held
    // Once settled it stays settled: no register changes on repeated frames.
    const uint8_t r0 = c.reg(0), g0 = c.reg(1), b0 = c.reg(2);
    for (int i = 0; i < 50; ++i) CHECK(c.step(plant.measure(c)) == Result::Settled);
    CHECK(c.reg(0) == r0 && c.reg(1) == g0 && c.reg(2) == b0);
}

void test_converges_for_the_measured_lights() {
    // Office preset neutral under light A (G/avg 0.875) and blue under light B (0.636).
    for (float cast_g : {0.875f, 0.636f, 1.19f}) {
        Controller c(Gains{0x52, 0x41, 0x66});
        const Plant plant{{1.0f, cast_g, 1.0f}, 2.1f};
        int crossings = 0;
        const int steps = converge(c, plant, 60, crossings);
        CHECK(steps > 0 && steps <= 20);
        CHECK(crossings == 0);
        CHECK(near(green_ratio(plant.measure(c)), 1.0f, 0.05f));
    }
}

void test_stable_for_a_steeper_sensor_response() {
    // If the real response is steeper than measured, the loop still settles.
    for (float p : {1.0f, 3.0f, 6.0f}) {
        Controller c;
        const Plant plant{{0.9f, 1.2f, 0.95f}, p};
        int crossings = 0;
        const int steps = converge(c, plant, 120, crossings);
        CHECK(steps > 0);
        if (p <= 3.0f) CHECK(crossings == 0);
    }
}

void test_error_shrinks_every_step() {
    Controller c;
    const Plant plant{{0.85f, 1.25f, 0.9f}, 2.1f};
    float prev = worst_error(plant.measure(c));
    int adjusted = 0;
    while (c.step(plant.measure(c)) == Result::Adjusted && adjusted < 30) {
        ++adjusted;
        const float now = worst_error(plant.measure(c));
        CHECK(now < prev);
        prev = now;
    }
    CHECK(adjusted >= 3 && c.settled());
}

void test_deadband_and_resume() {
    // The band is on the largest channel ratio (here G/B), not on each
    // channel's distance from the mean: G/avg(R,B) can never exceed it.
    Controller c(Gains{0x50, 0x48, 0x60});
    Means m;
    m.r = 100;
    m.g = 102;  // G/B 3.0 % (log 0.0298): inside the 0.03 band
    m.b = 99;
    m.neutral = 1000;
    m.total = 4800;
    CHECK(c.step(m) == Result::Settled);
    CHECK(c.reg(0) == 0x50 && c.reg(1) == 0x48 && c.reg(2) == 0x60);
    // G/B 4.0 %: outside the entry band but inside the resume band (0.045)
    // once settled.
    m.g = 103;
    CHECK(c.step(m) == Result::Settled);
    CHECK(c.reg(1) == 0x48);
    // Beyond deadband * resume the gains move again (G/B 7 %).
    m.g = 106;
    CHECK(c.step(m) == Result::Adjusted);
    CHECK(c.reg(1) < 0x48);
    // Never settled: 4 % is outside the entry band.
    Controller fresh(Gains{0x50, 0x48, 0x60});
    m.g = 103;
    CHECK(fresh.step(m) == Result::Adjusted);
    // A pure green cast just over 3 % is not left standing (the old
    // per-channel band let G/avg 1.046 settle).
    Controller g(Gains{0x50, 0x48, 0x60});
    m.r = m.b = 100;
    m.g = 103.5f;
    CHECK(g.step(m) == Result::Adjusted);
}

void test_too_few_pixels_skips() {
    Controller c(Gains{0x50, 0x48, 0x60});
    Means m;
    m.r = 80;
    m.g = 120;
    m.b = 80;
    m.total = 4800;
    m.neutral = 239;  // under 5 % of 4800
    CHECK(c.step(m) == Result::TooFew);
    m.total = 100;
    m.neutral = 39;  // under the 40-pixel floor
    CHECK(c.step(m) == Result::TooFew);
    CHECK(c.reg(0) == 0x50 && c.reg(1) == 0x48 && c.reg(2) == 0x60);
    m.neutral = 40;
    CHECK(c.step(m) == Result::Adjusted);
    Means zero;
    zero.neutral = zero.total = 1000;
    CHECK(c.step(zero) == Result::TooFew);
}

void test_step_size_is_limited() {
    Controller c(Gains{0x40, 0x40, 0x40});
    Means m;
    m.r = 20;  // absurd cast
    m.g = 200;
    m.b = 20;
    m.neutral = m.total = 1000;
    const Gains before = c.gains();
    CHECK(c.step(m) == Result::Adjusted);
    const Gains after = c.gains();
    const float step = c.config().max_step;
    CHECK(std::fabs(std::log(after.r / before.r)) <= step + 1e-4f);
    CHECK(std::fabs(std::log(after.g / before.g)) <= step + 1e-4f);
    CHECK(std::fabs(std::log(after.b / before.b)) <= step + 1e-4f);
    CHECK(after.g < before.g && after.r > before.r && after.b > before.b);
}

void test_gain_limits() {
    Controller c(Gains{0x40, 0x40, 0x40});
    const Plant plant{{0.02f, 3.0f, 0.02f}, 2.1f};  // uncorrectable within the limits
    for (int i = 0; i < 200; ++i) c.step(plant.measure(c));
    for (int ch = 0; ch < 3; ++ch) CHECK(c.reg(ch) >= 0x20 && c.reg(ch) <= 0xC0);
    CHECK(c.reg(1) == 0x20 && c.reg(0) == 0xC0 && c.reg(2) == 0xC0);
    // Start values outside the limits are clamped.
    Controller d(Gains{1, 500, 0x40});
    CHECK(d.reg(0) == 0x20 && d.reg(1) == 0xC0 && d.reg(2) == 0x40);
}

void test_pipeline_on_a_scene_with_an_led_display() {
    // End to end: synthetic frame = neutral wall + green display, both seen
    // through gains. The display is strongly saturated (chroma/luma ~1.6), so
    // pass 1's loose limit already drops it; the moderately tinted case is
    // test_pipeline_with_a_tinted_majority.
    Controller c;
    const float cast[3] = {0.95f, 1.15f, 0.97f};
    for (int i = 0; i < 40; ++i) {
        Image im;
        float k[3];
        for (int ch = 0; ch < 3; ++ch) k[ch] = cast[ch] * std::pow(static_cast<float>(c.reg(ch)) / 64.0f, 2.1f);
        auto clip = [](float v) { return static_cast<int>(std::min(255.0f, std::round(v))); };
        im.add(clip(60 * k[0]), clip(60 * k[1]), clip(60 * k[2]), 2500);     // wall
        im.add(clip(10 * k[0]), clip(140 * k[1]), clip(20 * k[2]), 2000);    // display, unclipped
        im.add(clip(5 * k[0]), clip(5 * k[1]), clip(5 * k[2]), 300);         // dark
        if (c.step(im.analyse()) == Result::Settled) break;
    }
    CHECK(c.settled());
    const float wr = cast[0] * std::pow(static_cast<float>(c.reg(0)) / 64.0f, 2.1f);
    const float wg = cast[1] * std::pow(static_cast<float>(c.reg(1)) / 64.0f, 2.1f);
    const float wb = cast[2] * std::pow(static_cast<float>(c.reg(2)) / 64.0f, 2.1f);
    CHECK(near(wg / ((wr + wb) / 2), 1.0f, 0.05f));
}

void test_pipeline_with_a_tinted_majority() {
    // Most of the frame is a surface tinted by the display (chroma/luma
    // ~0.4-0.6 under the cast), the grey wall is a minority: the balance must
    // still come from the wall. Cast given at the start gains (G/avg ~1.17).
    Controller c;
    const float cast[3] = {0.95f, 1.12f, 0.97f};
    const uint8_t start[3] = {c.reg(0), c.reg(1), c.reg(2)};
    auto k = [&](int ch) {
        return cast[ch] * std::pow(static_cast<float>(c.reg(ch)) / static_cast<float>(start[ch]), 2.1f);
    };
    auto clip = [](float v) { return static_cast<int>(std::min(255.0f, std::round(v))); };
    for (int i = 0; i < 40; ++i) {
        Image im;
        im.add(clip(60 * k(0)), clip(60 * k(1)), clip(60 * k(2)), 1500);   // wall
        im.add(clip(45 * k(0)), clip(70 * k(1)), clip(48 * k(2)), 5000);   // tinted surface
        im.add(clip(10 * k(0)), clip(140 * k(1)), clip(20 * k(2)), 1000);  // display
        if (c.step(im.analyse()) == Result::Settled) break;
    }
    CHECK(c.settled());
    CHECK(near(k(1) / ((k(0) + k(2)) / 2), 1.0f, 0.035f));
}

// Timed loop: frames start every frame_us; a sample returns a frame that
// started latency_us before the request (driver queue + send before the tap);
// a frame uses the gains written before it started. The AWB task samples every
// 330 ms and, with the gate, re-samples (one frame later) while stale.
struct TimedRun {
    int steps = -1;  // controller steps until settled, -1 = never
    int crossings = 0;
};

TimedRun run_timed(int64_t frame_us, int64_t latency_us, bool gated) {
    Controller c(Gains{0x52, 0x41, 0x66});
    const float cast[3] = {1.0f, 0.636f, 1.0f};  // the blue light measured with these gains
    const uint8_t start[3] = {c.reg(0), c.reg(1), c.reg(2)};
    std::vector<std::pair<int64_t, std::array<uint8_t, 3>>> writes = {{INT64_MIN, {start[0], start[1], start[2]}}};
    FreshGate gate(50'000);
    TimedRun out;
    float prev[3] = {0, 0, 0};
    int64_t t = 0;
    for (int i = 0; i < 120; ++i) {
        t += 330'000;
        int64_t fs = ((t - latency_us) / frame_us) * frame_us;
        for (int tries = 0; gated && !gate.fresh(fs) && tries < 8; ++tries) {
            t += frame_us;
            fs = ((t - latency_us) / frame_us) * frame_us;
        }
        if (gated && !gate.fresh(fs)) continue;
        std::array<uint8_t, 3> regs = writes.front().second;
        for (const auto& w : writes)
            if (w.first <= fs) regs = w.second;
        Means m;
        float v[3];
        for (int ch = 0; ch < 3; ++ch)
            v[ch] = 100.0f * cast[ch] * std::pow(static_cast<float>(regs[static_cast<size_t>(ch)]) / static_cast<float>(start[ch]), 2.1f);
        m.r = v[0];
        m.g = v[1];
        m.b = v[2];
        m.neutral = 3000;
        m.total = 4800;
        const float mean = (std::log(v[0]) + std::log(v[1]) + std::log(v[2])) / 3;
        const float band = c.config().deadband;
        for (int ch = 0; ch < 3; ++ch) {
            const float e = std::log(v[ch]) - mean;
            if (i && std::fabs(e) > band && std::fabs(prev[ch]) > band && (e > 0) != (prev[ch] > 0)) ++out.crossings;
            prev[ch] = e;
        }
        const Result r = c.step(m);
        if (r == Result::Settled) {
            out.steps = i;
            return out;
        }
        if (r == Result::Adjusted) {
            t += 20'000;  // decode + analysis before the write
            writes.push_back({t, {c.reg(0), c.reg(1), c.reg(2)}});
            gate.wrote(t);
        }
    }
    return out;
}

// ----------------------------------------------------------- registers ----

void test_ov5640_gain_conversion() {
    // 1.0x on both sensors, and the driver's own light presets line up:
    // esp32-camera writes office R/G/B 0x52/0x41/0x66 on the OV2640 and
    // 0x520/0x410/0x660 on the OV5640 (sunny 5E/41/54 vs 0x5e0/0x410/0x540,
    // home 42/3F/71 vs 0x420/0x3f0/0x710).
    CHECK(ov5640_gain(0x40) == 0x400);
    CHECK(ov5640_gain(0x52) == 0x520 && ov5640_gain(0x41) == 0x410 && ov5640_gain(0x66) == 0x660);
    CHECK(ov5640_gain(0x5E) == 0x5e0 && ov5640_gain(0x54) == 0x540);
    CHECK(ov5640_gain(0x42) == 0x420 && ov5640_gain(0x3F) == 0x3f0 && ov5640_gain(0x71) == 0x710);
    // The pair split the driver's write_reg16 makes: high nibble in the even
    // register, low byte in the odd one.
    CHECK((ov5640_gain(0x4A) >> 8) == 0x04 && (ov5640_gain(0x4A) & 0xFF) == 0xA0);
    // Every controller output fits 12 bits; the limits and start gains map inside them.
    CHECK(ov5640_gain(0xFF) == 0xFF0 && ov5640_gain(0xFF) <= ov5640::kGainMax);
    CHECK(ov5640_gain(0) == 0);
    ControllerConfig cfg;
    CHECK(ov5640_gain(static_cast<unsigned>(cfg.min_gain)) == 0x200);
    CHECK(ov5640_gain(static_cast<unsigned>(cfg.max_gain)) == 0xC00);
    CHECK(ov5640_gain(static_cast<unsigned>(kStartGains.r)) == 0x520);
    CHECK(ov5640_gain(static_cast<unsigned>(kStartGains.g)) == 0x4A0);
    CHECK(ov5640_gain(static_cast<unsigned>(kStartGains.b)) == 0x660);
    // Wider values clamp to 12 bits rather than wrapping.
    CHECK(ov5640_gain(0x100) == 0xFFF && ov5640_gain(0xFFFF) == 0xFFF);
    // Monotonic over the whole 8-bit range, so the controller's ordering holds.
    for (unsigned v = 1; v < 256; ++v) CHECK(ov5640_gain(v) > ov5640_gain(v - 1));
    static_assert(ov5640_gain(0x40) == 0x400, "constexpr");
}

void test_sensor_register_mapping() {
    CHECK(ov2640::kCtrlReg == 0x0C7 && ov2640::kManualBit == 0x40);
    CHECK(ov5640::kCtrlReg == 0x3406 && ov5640::kManualBit == 0x01);
    for (int c = 0; c < 3; ++c) CHECK(ov5640::kGainReg[c] == 0x3400 + 2 * c && ov2640::kGainReg[c] == 0x0CC + c);
}

// ----------------------------------------------------------- freshness ----

void test_fresh_gate() {
    FreshGate g(50'000);
    CHECK(g.fresh(-1'000'000));  // nothing written yet
    g.wrote(1'000'000);
    CHECK(!g.fresh(900'000));
    CHECK(!g.fresh(1'049'999));
    CHECK(g.fresh(1'050'000));
}

void test_measurement_latency_is_gated() {
    // 25, 6 and 4 fps (dim light), latency from none to 6 frames.
    for (int64_t frame_us : {40'000, 166'000, 250'000}) {
        for (int frames_late : {0, 1, 2, 4, 6}) {
            const TimedRun r = run_timed(frame_us, frames_late * frame_us, true);
            CHECK(r.steps > 0 && r.steps <= 20);
            CHECK(r.crossings == 0);
        }
    }
    // Without the gate, frames 1 s old (4 fps, 4 frames late: three AWB
    // periods) are measured before earlier writes show: the loop limit-cycles.
    const TimedRun ungated = run_timed(250'000, 4 * 250'000, false);
    CHECK(ungated.crossings > 0 && ungated.steps < 0);
}

}  // namespace

int main() {
    test_grey_scene_with_cast();
    test_coloured_light_and_objects_are_excluded();
    test_strong_cast_still_finds_grey();
    test_tinted_surface_does_not_become_neutral();
    test_empty_and_unusable_images();
    test_converges_from_green_cast_without_oscillation();
    test_converges_for_the_measured_lights();
    test_stable_for_a_steeper_sensor_response();
    test_error_shrinks_every_step();
    test_deadband_and_resume();
    test_too_few_pixels_skips();
    test_step_size_is_limited();
    test_gain_limits();
    test_pipeline_on_a_scene_with_an_led_display();
    test_pipeline_with_a_tinted_majority();
    test_ov5640_gain_conversion();
    test_sensor_register_mapping();
    test_fresh_gate();
    test_measurement_latency_is_gated();
    std::printf("awb: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
