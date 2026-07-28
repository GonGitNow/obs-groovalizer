// GrooveCore audio analyzer — C++ implementation of SPEC.md §2.
//
// Header-only, no dependencies beyond the standard library, no allocation after
// construction. The Swift and Kotlin ports mirror this file line for line; if you
// change the maths here, change them there too and re-run the conformance vectors
// in shared/groove-spec/reference/.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace groove {

inline constexpr int   kFftSize       = 2048;
inline constexpr int   kFftBins       = kFftSize / 2;
inline constexpr int   kSpectrumBins  = 256;
inline constexpr int   kWaveformBins  = 512;
inline constexpr float kAnalysisRate  = 44100.0f;
inline constexpr float kFreqLo        = 30.0f;
inline constexpr float kFreqHi        = 16000.0f;
inline constexpr float kDbFloor       = -70.0f;
inline constexpr float kDbCeil        = -12.0f;
inline constexpr float kSpecAttack    = 0.55f;
inline constexpr float kSpecRelease   = 0.12f;

struct Frame {
    float time      = 0.0f;
    float dt        = 0.0f;
    float bass      = 0.0f;
    float lowMid    = 0.0f;
    float mid       = 0.0f;
    float highMid   = 0.0f;
    float treble    = 0.0f;
    float level     = 0.0f;
    float beat      = 0.0f;
    float pulse     = 0.0f;
    float bpm       = 0.0f;
    uint32_t beatCount = 0;

    // Persistent state (SPEC 2.9). A shader keeps nothing between frames, so
    // these are integrated here and handed in as uniforms. Port of the Swift
    // reference; behaviour must match to 1e-4 like the rest of the pipeline.
    float spin    = 0.0f;   ///< integrated angle, radians; never resets
    float turn    = 0.0f;   ///< bass latch that snaps up then relaxes
    float wanderX = 0.0f;   ///< smooth walks, re-targeted on beats
    float wanderY = 0.0f;
    float slow    = 0.0f;   ///< ~30 s integrator of level, 0..1

    std::array<float, kSpectrumBins> spectrum{};
    std::array<float, kWaveformBins> waveform{};
};

// ── Beat detector ────────────────────────────────────────────────────────────
// Port of the web build's BeatDetector (src/lib/presetMasher.ts), itself derived
// from MilkDrop 3's DoCustomSoundAnalysis: smoothed band levels measured against
// a slow long-term average, with an adaptive threshold that rises after each
// trigger so a sustained loud passage does not machine-gun.
//
// Two things differ from the web defaults, both deliberate:
//
//   1. The web build defaults to MilkDrop's "hardcut3" (treble mode, threshold
//      2.9, 1000 ms gap) because there the detector drives *preset switching*.
//      Driving a per-beat visual envelope from that config swallows four kicks
//      out of five. GrooveCore defaults to bass mode, threshold 1.5, 200 ms.
//   2. The web port collapsed MilkDrop's per-mode comparison into
//      `sum(immRel) > threshold * 3`, i.e. "all three bands spike at once".
//      That is far stricter than MilkDrop's own test and never fires on a normal
//      four-on-the-floor kick. Mode selection is restored here.
class BeatDetector {
public:
    enum class Mode { Bass, Treble, Combined };

    Mode  mode         = Mode::Bass;
    float sensitivity  = 1.0f;
    // Tuned by parameter sweep against the conformance signal (100 BPM kick):
    // this point sits in the middle of a wide plateau that locks every beat, not
    // on a knife edge. See shared/groove-spec/reference/.
    float base         = 1.4f;    // relative-energy threshold
    float minDelayMs   = 200.0f;  // 200 ms floor -> tracks up to 300 BPM
    float halflife     = 1.5f;    // seconds for the threshold to relax halfway
    float spikeLevel   = 4.5f;    // absolute override for very strong transients
    float retriggerBump = 1.10f;  // threshold multiplier applied after each trigger

    void reset() {
        avg_ = {0, 0, 0};
        longAvg_ = {0, 0, 0};
        immRel_ = {1, 1, 1};
        threshold_ = base * 1.5f;
        frames_ = 0;
        sinceTrigger_ = 1e9f;
    }

    // bands: {bass, mid, treble}, pre-auto-gain. dt in seconds.
    bool update(const std::array<float, 3>& bands, float dt) {
        frames_++;
        sinceTrigger_ += dt * 1000.0f;
        const float fps = dt > 1e-6f ? 1.0f / dt : 60.0f;
        const float fpsAdj = 30.0f / std::max(fps, 1.0f);

        const float attack = std::pow(0.2f, fpsAdj);
        const float decay  = std::pow(0.5f, fpsAdj);
        const float longBase = frames_ < 50 ? 0.9f : 0.992f;
        const float longRate = std::pow(longBase, fpsAdj);

        for (int i = 0; i < 3; i++) {
            const float rate = bands[i] > avg_[i] ? attack : decay;
            avg_[i] = avg_[i] * rate + bands[i] * (1.0f - rate);
            longAvg_[i] = longAvg_[i] * longRate + bands[i] * (1.0f - longRate);
            immRel_[i] = std::fabs(longAvg_[i]) < 0.001f ? 1.0f : bands[i] / longAvg_[i];
        }

        const float relax = std::exp(-1.3863f / std::max(halflife * fps, 1.0f));
        auto decayThreshold = [&] { threshold_ = (threshold_ - base) * relax + base; };

        // Warm-up: with no history the relative levels are meaningless and the
        // first second of every track would fire on every frame.
        if (frames_ < 60 || sinceTrigger_ < minDelayMs) {
            decayThreshold();
            return false;
        }

        const int band = mode == Mode::Treble ? 2 : 0;

        // Absolute-spike override: a genuinely huge transient beats the adaptive
        // threshold regardless of what the long-term average has settled to.
        if (bands[band] > spikeLevel) {
            trigger();
            return true;
        }

        const float value = mode == Mode::Combined
                                ? (immRel_[0] + immRel_[1] + immRel_[2]) / 3.0f
                                : immRel_[band];

        if (value * sensitivity > threshold_) {
            trigger();
            return true;
        }

        decayThreshold();
        return false;
    }

    float threshold() const { return threshold_; }
    float relative(int band) const { return immRel_[band]; }

private:
    void trigger() {
        sinceTrigger_ = 0.0f;
        // Raise the bar so a loud sustained passage does not retrigger every frame.
        threshold_ *= retriggerBump;
    }

    std::array<float, 3> avg_{0, 0, 0};
    std::array<float, 3> longAvg_{0, 0, 0};
    std::array<float, 3> immRel_{1, 1, 1};
    float threshold_ = 2.25f;
    int   frames_ = 0;
    float sinceTrigger_ = 1e9f;
};

// ── Analyzer ─────────────────────────────────────────────────────────────────
class Analyzer {
public:
    float reactivity  = 1.0f;   // 0..2, scales the band outputs
    float speed       = 1.0f;   // scene clock multiplier
    bool  autoGain    = true;

    Analyzer() {
        ring_.assign(kFftSize, 0.0f);
        window_.resize(kFftSize);
        for (int n = 0; n < kFftSize; n++) {
            window_[n] = 0.5f * (1.0f - std::cos(6.2831853f * float(n) / float(kFftSize - 1)));
        }
        re_.resize(kFftSize);
        im_.resize(kFftSize);
        mag_.resize(kFftBins);
        buildBandTable();
        buildBitReversal();
        detector_.reset();
    }

    void setSampleRate(float rate) {
        if (rate > 0.0f && std::fabs(rate - sourceRate_) > 0.5f) {
            sourceRate_ = rate;
            decimPhase_ = 0.0f;
        }
    }

    // Push interleaved-or-mono float PCM. Channels are averaged to mono.
    void pushAudio(const float* const* planes, int channels, int frames) {
        if (channels <= 0 || frames <= 0 || planes == nullptr) return;
        // Decimate to the fixed analysis rate so band edges land on the same bins
        // on every device regardless of the hardware sample rate.
        const float step = sourceRate_ > 0.0f ? sourceRate_ / kAnalysisRate : 1.0f;
        for (int i = 0; i < frames; i++) {
            decimPhase_ += 1.0f;
            if (decimPhase_ < step) continue;
            decimPhase_ -= step;

            float s = 0.0f;
            int used = 0;
            for (int c = 0; c < channels; c++) {
                if (planes[c] == nullptr) continue;
                s += planes[c][i];
                used++;
            }
            if (used > 1) s /= float(used);
            ring_[writePos_] = s;
            writePos_ = (writePos_ + 1) % kFftSize;
            filled_ = std::min(filled_ + 1, kFftSize);
        }
    }

    void pushSilence() { silentFrames_++; }

    // Run one analysis pass and fill `out`. Call once per rendered frame.
    void analyze(Frame& out, float dt) {
        dt = std::clamp(dt, 1.0f / 480.0f, 0.25f);
        sceneTime_ += dt * speed;
        out.dt = dt;
        out.time = sceneTime_;

        // Copy the ring into linear order, oldest first, and window it.
        float rms = 0.0f;
        for (int n = 0; n < kFftSize; n++) {
            const float s = ring_[(writePos_ + n) % kFftSize];
            rms += s * s;
            re_[n] = s * window_[n];
            im_[n] = 0.0f;
        }
        rms = std::sqrt(rms / float(kFftSize));

        fft();

        const float binHz = kAnalysisRate / float(kFftSize);
        for (int k = 0; k < kFftBins; k++) {
            mag_[k] = std::sqrt(re_[k] * re_[k] + im_[k] * im_[k]) * 2.0f / float(kFftSize);
        }
        (void)binHz;

        // Log-fold into the spectrum, peak-hold per bucket (mean smears transients).
        const float aFast = frameRateAdjust(kSpecAttack, dt);
        const float aSlow = frameRateAdjust(kSpecRelease, dt);
        for (int i = 0; i < kSpectrumBins; i++) {
            float peak = 0.0f;
            for (int k = bandLo_[i]; k < bandHi_[i]; k++) peak = std::max(peak, mag_[k]);
            const float db = 20.0f * std::log10(peak + 1e-9f);
            const float norm = std::clamp((db - kDbFloor) / (kDbCeil - kDbFloor), 0.0f, 1.0f);
            const float a = norm > spec_[i] ? aFast : aSlow;
            spec_[i] += (norm - spec_[i]) * a;
            out.spectrum[i] = spec_[i];
        }

        // Waveform, decimated from the ring (newest window, oldest first).
        for (int i = 0; i < kWaveformBins; i++) {
            const int n = i * (kFftSize / kWaveformBins);
            out.waveform[i] = ring_[(writePos_ + n) % kFftSize];
        }

        // Five bands from the folded spectrum.
        float raw[5];  // pre-gain, reused by the beat detector below
        raw[0] = meanRange(0, 66);      // bass      30 – 160 Hz
        raw[1] = meanRange(66, 118);    // lowMid   160 – 600
        raw[2] = meanRange(118, 170);   // mid      600 – 2400
        raw[3] = meanRange(170, 210);   // highMid 2400 – 7000
        raw[4] = meanRange(210, 256);   // treble  7000 – 16000

        const float lg = frameRateAdjust(0.0015f, dt);
        for (int b = 0; b < 5; b++) {
            longAvg_[b] += (raw[b] - longAvg_[b]) * lg;
            float g = 1.0f;
            if (autoGain) {
                g = std::clamp(0.35f / std::max(longAvg_[b], 0.02f), 0.5f, 4.0f);
            }
            // Deliberately allows >1: overshoot is what gives kicks their punch.
            bands_[b] = std::clamp(raw[b] * g * reactivity, 0.0f, 2.0f);
        }

        out.bass = bands_[0];
        out.lowMid = bands_[1];
        out.mid = bands_[2];
        out.highMid = bands_[3];
        out.treble = bands_[4];
        out.level = std::clamp(std::sqrt(rms) * 2.2f, 0.0f, 1.0f);

        // Beat. Fed the RAW pre-auto-gain bands, scaled x4 to match the operating
        // point of the web build (which passes byte-FFT levels * 4). Auto-gain
        // exists precisely to flatten dynamics for the shaders — handing its
        // output to a relative-energy detector leaves it nothing to detect.
        const std::array<float, 3> trio{
            raw[0] * 4.0f,
            (raw[1] + raw[2]) * 2.0f,
            (raw[3] + raw[4]) * 2.0f,
        };
        if (detector_.update(trio, dt)) {
            beatEnv_ = 1.0f;
            pulseEnv_ = 1.0f;
            beatCount_++;
            if (lastBeatTime_ > 0.0f) {
                const float ibi = wallTime_ - lastBeatTime_;
                if (ibi > 0.25f && ibi < 2.0f) {
                    ibiHistory_[ibiPos_ % kIbiCount] = ibi;
                    ibiPos_++;
                }
            }
            lastBeatTime_ = wallTime_;
        }
        wallTime_ += dt;
        beatEnv_ *= std::exp(-6.0f * dt);
        pulseEnv_ *= std::exp(-2.2f * dt);

        out.beat = beatEnv_;
        out.pulse = pulseEnv_;
        out.beatCount = beatCount_;
        out.bpm = estimateBpm();

        updateState(out, dt);
    }

    BeatDetector& beatDetector() { return detector_; }
    void resetClock() { sceneTime_ = 0.0f; }

private:
    static float frameRateAdjust(float aAt60, float dt) {
        // A coefficient tuned at 60 fps must decay at the same wall-clock rate on
        // a 30 fps TV and a 120 fps phone, or the visuals feel different.
        return 1.0f - std::pow(1.0f - aAt60, 60.0f * dt);
    }

    float meanRange(int lo, int hi) const {
        float s = 0.0f;
        for (int i = lo; i < hi; i++) s += spec_[i];
        return s / float(std::max(hi - lo, 1));
    }

    void buildBandTable() {
        const float binHz = kAnalysisRate / float(kFftSize);
        for (int i = 0; i < kSpectrumBins; i++) {
            const float t0 = float(i) / float(kSpectrumBins);
            const float t1 = float(i + 1) / float(kSpectrumBins);
            const float f0 = kFreqLo * std::pow(kFreqHi / kFreqLo, t0);
            const float f1 = kFreqLo * std::pow(kFreqHi / kFreqLo, t1);
            int k0 = int(std::floor(f0 / binHz));
            int k1 = int(std::ceil(f1 / binHz));
            k0 = std::clamp(k0, 0, kFftBins - 1);
            k1 = std::clamp(std::max(k1, k0 + 1), 1, kFftBins);
            bandLo_[i] = k0;
            bandHi_[i] = k1;
        }
    }

    void buildBitReversal() {
        rev_.resize(kFftSize);
        int bits = 0;
        while ((1 << bits) < kFftSize) bits++;
        for (int i = 0; i < kFftSize; i++) {
            int r = 0;
            for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev_[i] = r;
        }
    }

    // In-place iterative radix-2 Cooley-Tukey. 2048 points is ~40 µs; not worth a
    // dependency on a vendored FFT library.
    void fft() {
        for (int i = 0; i < kFftSize; i++) {
            const int j = rev_[i];
            if (j > i) { std::swap(re_[i], re_[j]); std::swap(im_[i], im_[j]); }
        }
        for (int len = 2; len <= kFftSize; len <<= 1) {
            const float ang = -6.2831853f / float(len);
            const float wr = std::cos(ang), wi = std::sin(ang);
            for (int i = 0; i < kFftSize; i += len) {
                float cr = 1.0f, ci = 0.0f;
                for (int j = 0; j < len / 2; j++) {
                    const int a = i + j, b = i + j + len / 2;
                    const float xr = re_[b] * cr - im_[b] * ci;
                    const float xi = re_[b] * ci + im_[b] * cr;
                    re_[b] = re_[a] - xr; im_[b] = im_[a] - xi;
                    re_[a] += xr;         im_[a] += xi;
                    const float nr = cr * wr - ci * wi;
                    ci = cr * wi + ci * wr;
                    cr = nr;
                }
            }
        }
    }

    /// Advance the dynamical state. Every rate is per-second and multiplied by
    /// dt, so behaviour is identical at any frame rate — OBS may run at 30 or 60.
    void updateState(Frame& out, float dt) {
        stSpin_ += dt * (0.06f + out.bass * 0.55f);
        // Deliberately NOT wrapped — see the Swift reference. Wrapping made
        // every non-periodic use snap every ~15 s, defeating the purpose.

        if (out.bass > stTurn_) {
            stTurn_ = out.bass;
        } else {
            stTurn_ += (0.15f - stTurn_) * (1.0f - std::exp(-dt / 0.55f));
        }

        if (out.beat > 0.5f) {
            stTargetX_ = nextRandom();
            stTargetY_ = nextRandom();
        }
        const float k = 1.0f - std::exp(-dt / 1.6f);
        stWanderX_ += (stTargetX_ - stWanderX_) * k;
        stWanderY_ += (stTargetY_ - stWanderY_) * k;

        stSlow_ += (out.level - stSlow_) * (1.0f - std::exp(-dt / 30.0f));

        out.spin    = stSpin_;
        out.turn    = stTurn_;
        out.wanderX = stWanderX_;
        out.wanderY = stWanderY_;
        out.slow    = std::clamp(stSlow_, 0.0f, 1.0f);
    }

    float nextRandom() {
        stRng_ = stRng_ * 1664525u + 1013904223u;
        return float(stRng_ >> 8) / float(1 << 24) * 2.0f - 1.0f;
    }

    float stSpin_ = 0.0f, stTurn_ = 0.0f;
    float stWanderX_ = 0.0f, stWanderY_ = 0.0f;
    float stTargetX_ = 0.0f, stTargetY_ = 0.0f;
    float stSlow_ = 0.0f;
    uint32_t stRng_ = 0x9E3779B9u;

    float estimateBpm() const {
        int n = std::min<int>(ibiPos_, kIbiCount);
        if (n < 4) return 0.0f;
        std::array<float, kIbiCount> tmp{};
        for (int i = 0; i < n; i++) tmp[i] = ibiHistory_[i];
        std::sort(tmp.begin(), tmp.begin() + n);
        const float median = tmp[n / 2];
        if (median <= 0.0f) return 0.0f;
        return std::clamp(60.0f / median, 60.0f, 200.0f);
    }

    static constexpr int kIbiCount = 16;

    std::vector<float> ring_, window_, re_, im_, mag_;
    std::vector<int> rev_;
    std::array<int, kSpectrumBins> bandLo_{}, bandHi_{};
    std::array<float, kSpectrumBins> spec_{};
    std::array<float, 5> bands_{}, longAvg_{};
    std::array<float, kIbiCount> ibiHistory_{};
    int ibiPos_ = 0;

    int writePos_ = 0;
    int filled_ = 0;
    int silentFrames_ = 0;
    float sourceRate_ = 48000.0f;
    float decimPhase_ = 0.0f;
    float sceneTime_ = 0.0f;
    float wallTime_ = 0.0f;
    float beatEnv_ = 0.0f;
    float pulseEnv_ = 0.0f;
    float lastBeatTime_ = 0.0f;
    uint32_t beatCount_ = 0;
    BeatDetector detector_;
};

}  // namespace groove
