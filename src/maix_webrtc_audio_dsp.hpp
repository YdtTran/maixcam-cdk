#ifndef MAIX_WEBRTC_AUDIO_DSP_HPP
#define MAIX_WEBRTC_AUDIO_DSP_HPP

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include <atomic>
#include <memory>
#include <mutex>
#include <chrono>
#include <string>
#include <sstream>

#include <speex/speex_preprocess.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace maix::webrtc {

/**
 * 2nd-order Butterworth IIR High-Pass Filter (Transposed Direct Form II).
 * Provides a clean 12 dB/octave rolloff below cutoff frequency with maximally flat passband.
 * Preserves filter state across 20 ms frame boundaries to prevent boundary clicks.
 */
class HighPassFilter2ndOrder {
public:
    HighPassFilter2ndOrder(float cutoff_hz = 50.0f, float sample_rate = 48000.0f, float q = 0.70710678f) {
        set_cutoff(cutoff_hz, sample_rate, q);
        reset();
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        s1_ = 0.0f;
        s2_ = 0.0f;
    }

    void set_cutoff(float cutoff_hz, float sample_rate = 48000.0f, float q = 0.70710678f) {
        if (cutoff_hz <= 0.0f || sample_rate <= 0.0f) return;
        const double w0 = 2.0 * M_PI * static_cast<double>(cutoff_hz) / static_cast<double>(sample_rate);
        const double cos_w0 = std::cos(w0);
        const double sin_w0 = std::sin(w0);
        const double alpha = sin_w0 / (2.0 * static_cast<double>(q));

        const double b0 = (1.0 + cos_w0) * 0.5;
        const double b1 = -(1.0 + cos_w0);
        const double b2 = (1.0 + cos_w0) * 0.5;
        const double a0 = 1.0 + alpha;
        const double a1 = -2.0 * cos_w0;
        const double a2 = 1.0 - alpha;

        std::lock_guard<std::mutex> lock(mutex_);
        b0_ = static_cast<float>(b0 / a0);
        b1_ = static_cast<float>(b1 / a0);
        b2_ = static_cast<float>(b2 / a0);
        a1_ = static_cast<float>(a1 / a0);
        a2_ = static_cast<float>(a2 / a0);
        cutoff_hz_ = cutoff_hz;
        sample_rate_ = sample_rate;
    }

    void process(int16_t *samples, size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        const float b0 = b0_, b1 = b1_, b2 = b2_;
        const float a1 = a1_, a2 = a2_;
        float s1 = s1_, s2 = s2_;

        for (size_t i = 0; i < count; ++i) {
            const float x = static_cast<float>(samples[i]);
            const float y = b0 * x + s1;
            s1 = b1 * x - a1 * y + s2;
            s2 = b2 * x - a2 * y;

            if (y > 32767.0f) {
                samples[i] = 32767;
            } else if (y < -32768.0f) {
                samples[i] = -32768;
            } else {
                samples[i] = static_cast<int16_t>(y >= 0.0f ? (y + 0.5f) : (y - 0.5f));
            }
        }
        s1_ = s1;
        s2_ = s2;
    }

    float cutoff_hz() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cutoff_hz_;
    }

private:
    mutable std::mutex mutex_;
    float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f;
    float a1_ = 0.0f, a2_ = 0.0f;
    float s1_ = 0.0f, s2_ = 0.0f;
    float cutoff_hz_ = 50.0f;
    float sample_rate_ = 48000.0f;
};

/**
 * SpeexDSP Preprocessor Wrapper configured for low CPU overhead.
 * Operates on 20 ms frames (960 samples @ 48 kHz).
 * Default noise suppression level: -6 dB.
 * AGC, VAD, Dereverb are disabled.
 */
class SpeexDenoiseProcessor {
public:
    SpeexDenoiseProcessor(int frame_samples = 960, int sample_rate = 48000, int suppress_db = -6)
        : frame_samples_(frame_samples), sample_rate_(sample_rate), suppress_db_(suppress_db) {
        init();
    }

    ~SpeexDenoiseProcessor() {
        cleanup();
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        cleanup();
        init();
    }

    void set_suppression_level(int suppress_db) {
        std::lock_guard<std::mutex> lock(mutex_);
        // Clamp to sensible range [-30, 0] and enforce negative dB
        int val_db = -std::abs(suppress_db);
        if (val_db < -30) val_db = -30;
        if (val_db > 0) val_db = 0;
        suppress_db_ = val_db;
        if (state_) {
            spx_int32_t val = suppress_db_;
            speex_preprocess_ctl(state_, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &val);
        }
    }

    void set_denoise(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        denoise_enabled_ = enabled;
        if (state_) {
            spx_int32_t val = enabled ? 1 : 0;
            speex_preprocess_ctl(state_, SPEEX_PREPROCESS_SET_DENOISE, &val);
        }
    }

    void process(int16_t *samples) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!state_) return;
        speex_preprocess_run(state_, reinterpret_cast<spx_int16_t *>(samples));
    }

    int suppression_level() const { return suppress_db_; }
    bool is_denoise_enabled() const { return denoise_enabled_; }

private:
    void init() {
        state_ = speex_preprocess_state_init(frame_samples_, sample_rate_);
        if (state_) {
            spx_int32_t denoise = denoise_enabled_ ? 1 : 0;
            speex_preprocess_ctl(state_, SPEEX_PREPROCESS_SET_DENOISE, &denoise);

            spx_int32_t suppress = suppress_db_;
            speex_preprocess_ctl(state_, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &suppress);

            spx_int32_t agc = 0;
            speex_preprocess_ctl(state_, SPEEX_PREPROCESS_SET_AGC, &agc);

            spx_int32_t dereverb = 0;
            speex_preprocess_ctl(state_, SPEEX_PREPROCESS_SET_DEREVERB, &dereverb);
        }
    }

    void cleanup() {
        if (state_) {
            speex_preprocess_state_destroy(state_);
            state_ = nullptr;
        }
    }

    std::mutex mutex_;
    SpeexPreprocessState *state_ = nullptr;
    int frame_samples_ = 960;
    int sample_rate_ = 48000;
    int suppress_db_ = -6;
    bool denoise_enabled_ = true;
};

/**
 * DSP Mode for uplink microphone pipeline.
 */
enum class UplinkDspMode {
    BYPASS = 0,    // Raw microphone PCM untouched
    HPF_ONLY = 1,  // 2nd-order IIR HPF at 50 Hz only
    FULL = 2       // 2nd-order IIR HPF (50 Hz) + SpeexDSP (-6 dB) [Default]
};

/**
 * Complete Uplink Audio DSP Pipeline with Bounded Frame Accumulator.
 * Sits directly between MaixCAM microphone capture and Opus WebRTC encoder.
 */
class MaixAudioUplinkDSP {
public:
    static constexpr int SAMPLE_RATE = 48000;
    static constexpr int FRAME_SAMPLES = 960; // 20 ms
    static constexpr size_t FRAME_BYTES = FRAME_SAMPLES * sizeof(int16_t);
    static constexpr size_t MAX_ACCUMULATOR_SAMPLES = FRAME_SAMPLES * 5; // 100 ms max bound

    MaixAudioUplinkDSP(UplinkDspMode mode = UplinkDspMode::FULL, float hpf_cutoff = 50.0f, int speex_suppress_db = -6)
        : mode_(mode),
          hpf_(hpf_cutoff, static_cast<float>(SAMPLE_RATE)),
          speex_(FRAME_SAMPLES, SAMPLE_RATE, speex_suppress_db) {
        accumulator_.reserve(MAX_ACCUMULATOR_SAMPLES);
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        accumulator_.clear();
        hpf_.reset();
        speex_.reset();
    }

    void set_mode(UplinkDspMode mode) {
        mode_.store(mode, std::memory_order_relaxed);
    }

    UplinkDspMode get_mode() const {
        return mode_.load(std::memory_order_relaxed);
    }

    void set_hpf_cutoff(float cutoff_hz) {
        if (cutoff_hz < 10.0f || cutoff_hz > 500.0f) return;
        hpf_.set_cutoff(cutoff_hz, static_cast<float>(SAMPLE_RATE));
    }

    float get_hpf_cutoff() const {
        return hpf_.cutoff_hz();
    }

    void set_speex_suppression(int suppress_db) {
        speex_.set_suppression_level(suppress_db);
    }

    int get_speex_suppression() const {
        return speex_.suppression_level();
    }

    /**
     * Ingest arbitrary chunk sizes from microphone capture handle.
     * Enforces bounded accumulator to avoid latency accumulation.
     */
    void ingest_bytes(const void *data, size_t bytes) {
        if (!data || bytes == 0) return;
        const size_t samples_in = bytes / sizeof(int16_t);
        const int16_t *src = reinterpret_cast<const int16_t *>(data);

        std::lock_guard<std::mutex> lock(mutex_);
        if (accumulator_.size() + samples_in > MAX_ACCUMULATOR_SAMPLES) {
            // Drop oldest samples to preserve latency bound
            const size_t overflow = (accumulator_.size() + samples_in) - MAX_ACCUMULATOR_SAMPLES;
            const size_t drop = (overflow > accumulator_.size()) ? accumulator_.size() : overflow;
            accumulator_.erase(accumulator_.begin(), accumulator_.begin() + drop);
            overflow_count_.fetch_add(1, std::memory_order_relaxed);
        }
        accumulator_.insert(accumulator_.end(), src, src + samples_in);
    }

    /**
     * Check if at least one complete 20 ms frame (960 samples) is ready.
     */
    bool has_frame() {
        std::lock_guard<std::mutex> lock(mutex_);
        return accumulator_.size() >= FRAME_SAMPLES;
    }

    size_t queued_samples() {
        std::lock_guard<std::mutex> lock(mutex_);
        return accumulator_.size();
    }

    size_t queued_bytes() {
        std::lock_guard<std::mutex> lock(mutex_);
        return accumulator_.size() * sizeof(int16_t);
    }

    /**
     * Extracts and processes one 20 ms frame.
     * Output buffer out_frame must hold at least 960 int16_t samples.
     * Returns true if frame was popped and processed, false if insufficient samples.
     */
    bool pop_and_process_frame(int16_t *out_frame) {
        if (!out_frame) return false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (accumulator_.size() < FRAME_SAMPLES) return false;
            std::memcpy(out_frame, accumulator_.data(), FRAME_BYTES);
            accumulator_.erase(accumulator_.begin(), accumulator_.begin() + FRAME_SAMPLES);
        }

        const auto start_time = std::chrono::steady_clock::now();
        const UplinkDspMode cur_mode = mode_.load(std::memory_order_relaxed);

        if (cur_mode == UplinkDspMode::FULL) {
            hpf_.process(out_frame, FRAME_SAMPLES);
            speex_.process(out_frame);
            frames_processed_.fetch_add(1, std::memory_order_relaxed);
        } else if (cur_mode == UplinkDspMode::HPF_ONLY) {
            hpf_.process(out_frame, FRAME_SAMPLES);
            frames_processed_.fetch_add(1, std::memory_order_relaxed);
        } else {
            // BYPASS: Raw PCM passed untouched
            frames_bypassed_.fetch_add(1, std::memory_order_relaxed);
        }

        const auto end_time = std::chrono::steady_clock::now();
        const uint32_t elapsed_us = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count()
        );

        last_process_us_.store(elapsed_us, std::memory_order_relaxed);
        total_process_us_.fetch_add(elapsed_us, std::memory_order_relaxed);
        uint32_t prev_peak = peak_process_us_.load(std::memory_order_relaxed);
        while (elapsed_us > prev_peak && !peak_process_us_.compare_exchange_weak(prev_peak, elapsed_us, std::memory_order_relaxed)) {}

        return true;
    }

    /**
     * Telemetry and JSON status reporting.
     */
    std::string get_metrics_json() const {
        std::stringstream ss;
        const UplinkDspMode m = mode_.load(std::memory_order_relaxed);
        const char *mode_str = (m == UplinkDspMode::FULL) ? "full" : ((m == UplinkDspMode::HPF_ONLY) ? "hpf_only" : "bypass");

        const uint64_t processed = frames_processed_.load(std::memory_order_relaxed);
        const uint64_t bypassed = frames_bypassed_.load(std::memory_order_relaxed);
        const uint64_t total_us = total_process_us_.load(std::memory_order_relaxed);
        const uint32_t last_us = last_process_us_.load(std::memory_order_relaxed);
        const uint32_t peak_us = peak_process_us_.load(std::memory_order_relaxed);
        const uint64_t overflows = overflow_count_.load(std::memory_order_relaxed);
        const double avg_us = (processed > 0) ? (static_cast<double>(total_us) / processed) : 0.0;
        const double cpu_pct = (avg_us / 20000.0) * 100.0;

        ss << "{"
           << "\"mode\":\"" << mode_str << "\","
           << "\"hpf_enabled\":" << ((m == UplinkDspMode::FULL || m == UplinkDspMode::HPF_ONLY) ? "true" : "false") << ","
           << "\"hpf_cutoff_hz\":" << hpf_.cutoff_hz() << ","
           << "\"speex_enabled\":" << (m == UplinkDspMode::FULL ? "true" : "false") << ","
           << "\"speex_suppress_db\":" << speex_.suppression_level() << ","
           << "\"frames_processed\":" << processed << ","
           << "\"frames_bypassed\":" << bypassed << ","
           << "\"overflow_drops\":" << overflows << ","
           << "\"last_process_us\":" << last_us << ","
           << "\"avg_process_us\":" << avg_us << ","
           << "\"peak_process_us\":" << peak_us << ","
           << "\"cpu_pct_core\":" << cpu_pct
           << "}";
        return ss.str();
    }

private:
    std::atomic<UplinkDspMode> mode_{UplinkDspMode::FULL};
    std::mutex mutex_;
    std::vector<int16_t> accumulator_;
    HighPassFilter2ndOrder hpf_;
    SpeexDenoiseProcessor speex_;

    std::atomic<uint64_t> frames_processed_{0};
    std::atomic<uint64_t> frames_bypassed_{0};
    std::atomic<uint64_t> overflow_count_{0};
    std::atomic<uint64_t> total_process_us_{0};
    std::atomic<uint32_t> last_process_us_{0};
    std::atomic<uint32_t> peak_process_us_{0};
};

} // namespace maix::webrtc

#endif // MAIX_WEBRTC_AUDIO_DSP_HPP
