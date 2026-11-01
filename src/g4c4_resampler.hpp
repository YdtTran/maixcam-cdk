#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

/**
 * @brief Software Audio Resampler: 8000 Hz Mono S16 -> 48000 Hz Stereo S16_LE.
 *
 * Implements exact 6:1 upsampling with smooth cubic Hermite interpolation
 * across frame boundaries, followed by mono-to-stereo channel duplication.
 *
 * Designed specifically for MaixCAM CV182x / BlueALSA SBC A2DP playback:
 * - Deterministic, allocation-free per-frame processing.
 * - Zero external library dependency (bypasses missing ALSA plug rate plugins).
 * - Clamps outputs to signed 16-bit range [-32768, 32767].
 * - Integrated monotonic metrics tracking for PLAN-07 audio pipeline instrumentation.
 */
class G4c4Resampler final {
public:
    static constexpr int IN_RATE = 8000;
    static constexpr int OUT_RATE = 48000;
    static constexpr int RATIO = OUT_RATE / IN_RATE; // 6
    static constexpr int IN_CHANNELS = 1;
    static constexpr int OUT_CHANNELS = 2;

    struct Stats {
        uint64_t frames_in = 0;
        uint64_t frames_out = 0;
        uint64_t process_calls = 0;
        uint64_t clips = 0;
        uint64_t resets = 0;
    };

    G4c4Resampler() {
        reset();
    }

    void reset() {
        has_history_ = false;
        p0_ = 0;
        p1_ = 0;
        p2_ = 0;
        p3_ = 0;
        resets_.fetch_add(1, std::memory_order_relaxed);
    }

    /**
     * @brief Resamples mono 8 kHz PCM buffer to stereo 48 kHz PCM buffer.
     *
     * @param in_samples Pointer to mono S16 input samples.
     * @param in_count Number of mono samples in input.
     * @param out_stereo Destination vector for interleaved stereo S16 samples.
     *                   Resized to in_count * RATIO * 2 samples.
     */
    void process(const int16_t* in_samples, size_t in_count, std::vector<int16_t>& out_stereo) {
        if (!in_samples || in_count == 0) {
            out_stereo.clear();
            return;
        }

        process_calls_.fetch_add(1, std::memory_order_relaxed);
        frames_in_.fetch_add(in_count, std::memory_order_relaxed);

        const size_t out_frames = in_count * RATIO;
        out_stereo.resize(out_frames * OUT_CHANNELS);
        frames_out_.fetch_add(out_frames, std::memory_order_relaxed);

        size_t out_idx = 0;
        uint64_t local_clips = 0;

        for (size_t i = 0; i < in_count; ++i) {
            int32_t y0, y1, y2, y3;

            if (!has_history_) {
                y1 = in_samples[i];
                y0 = y1;
                y2 = (i + 1 < in_count) ? in_samples[i + 1] : y1;
                y3 = (i + 2 < in_count) ? in_samples[i + 2] : y2;
                has_history_ = true;
            } else {
                y0 = p0_;
                y1 = p1_;
                y2 = in_samples[i];
                y3 = (i + 1 < in_count) ? in_samples[i + 1] : y2;
            }

            // Hermite cubic interpolation coefficients
            // Interpolating between y1 (t=0) and y2 (t=1)
            const double c0 = y1;
            const double c1 = 0.5 * (y2 - y0);
            const double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
            const double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);

            for (int step = 0; step < RATIO; ++step) {
                const double t = static_cast<double>(step) / static_cast<double>(RATIO);
                const double val = ((c3 * t + c2) * t + c1) * t + c0;

                int32_t ival = static_cast<int32_t>(val > 0.0 ? val + 0.5 : val - 0.5);
                if (ival < -32768) {
                    ival = -32768;
                    ++local_clips;
                } else if (ival > 32767) {
                    ival = 32767;
                    ++local_clips;
                }
                const int16_t sample = static_cast<int16_t>(ival);

                // Stereo duplicate (Left = Right)
                out_stereo[out_idx++] = sample;
                out_stereo[out_idx++] = sample;
            }

            p0_ = y1;
            p1_ = y2;
            p2_ = y3;
            p3_ = (i + 2 < in_count) ? in_samples[i + 2] : y3;
        }

        if (local_clips > 0) {
            clips_.fetch_add(local_clips, std::memory_order_relaxed);
        }
    }

    Stats get_stats() const {
        Stats s;
        s.frames_in = frames_in_.load(std::memory_order_relaxed);
        s.frames_out = frames_out_.load(std::memory_order_relaxed);
        s.process_calls = process_calls_.load(std::memory_order_relaxed);
        s.clips = clips_.load(std::memory_order_relaxed);
        s.resets = resets_.load(std::memory_order_relaxed);
        return s;
    }

private:
    bool has_history_ = false;
    int32_t p0_ = 0;
    int32_t p1_ = 0;
    int32_t p2_ = 0;
    int32_t p3_ = 0;

    std::atomic<uint64_t> frames_in_{0};
    std::atomic<uint64_t> frames_out_{0};
    std::atomic<uint64_t> process_calls_{0};
    std::atomic<uint64_t> clips_{0};
    std::atomic<uint64_t> resets_{0};
};
