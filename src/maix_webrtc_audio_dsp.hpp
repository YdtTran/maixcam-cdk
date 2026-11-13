#ifndef MAIX_WEBRTC_AUDIO_DSP_HPP
#define MAIX_WEBRTC_AUDIO_DSP_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>

namespace maix::webrtc {

// Compatibility name for the raw PCM frame accumulator. No signal processing
// is performed: complete microphone frames are copied unchanged into Opus.
class MaixAudioUplinkDSP {
public:
    static constexpr int SAMPLE_RATE = 48000;
    static constexpr int FRAME_SAMPLES = 960;
    static constexpr size_t FRAME_BYTES = FRAME_SAMPLES * sizeof(int16_t);
    static constexpr size_t MAX_ACCUMULATOR_SAMPLES = FRAME_SAMPLES * 5;
    static constexpr int64_t PERIODIC_FLUSH_MS = 5000;

    explicit MaixAudioUplinkDSP(int sample_rate = 48000, size_t frame_samples = 960)
        : sample_rate_(sample_rate),
          frame_samples_(frame_samples),
          frame_bytes_(frame_samples * sizeof(int16_t)),
          max_accumulator_samples_(frame_samples * 5),
          last_flush_(std::chrono::steady_clock::now()) {
        accumulator_.reserve(max_accumulator_samples_);
    }

    void configure(int sample_rate, size_t frame_samples) {
        std::lock_guard<std::mutex> lock(mutex_);
        sample_rate_ = sample_rate;
        frame_samples_ = frame_samples;
        frame_bytes_ = frame_samples * sizeof(int16_t);
        max_accumulator_samples_ = frame_samples * 5;
        accumulator_.clear();
        accumulator_.reserve(max_accumulator_samples_);
        last_flush_ = std::chrono::steady_clock::now();
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        accumulator_.clear();
        last_flush_ = std::chrono::steady_clock::now();
    }

    void ingest_bytes(const void *data, size_t bytes) {
        if (!data || bytes == 0) return;
        size_t samples_in = bytes / sizeof(int16_t);
        const auto *src = static_cast<const int16_t *>(data);
        std::lock_guard<std::mutex> lock(mutex_);
        if (samples_in > max_accumulator_samples_) {
            src += samples_in - max_accumulator_samples_;
            samples_in = max_accumulator_samples_;
            accumulator_.clear();
            overflow_count_.fetch_add(1, std::memory_order_relaxed);
        } else if (accumulator_.size() + samples_in > max_accumulator_samples_) {
            const size_t drop = accumulator_.size() + samples_in - max_accumulator_samples_;
            accumulator_.erase(accumulator_.begin(), accumulator_.begin() + drop);
            overflow_count_.fetch_add(1, std::memory_order_relaxed);
        }
        accumulator_.insert(accumulator_.end(), src, src + samples_in);

        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush_).count() >= PERIODIC_FLUSH_MS) {
            last_flush_ = now;
            if (accumulator_.size() >= frame_samples_ * 2) {
                const size_t drop = ((accumulator_.size() / frame_samples_) - 1) * frame_samples_;
                accumulator_.erase(accumulator_.begin(), accumulator_.begin() + drop);
                overflow_count_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    bool has_frame() {
        std::lock_guard<std::mutex> lock(mutex_);
        return accumulator_.size() >= frame_samples_;
    }

    size_t queued_samples() {
        std::lock_guard<std::mutex> lock(mutex_);
        return accumulator_.size();
    }

    size_t queued_bytes() { return queued_samples() * sizeof(int16_t); }

    bool pop_and_process_frame(int16_t *out_frame) {
        if (!out_frame) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (accumulator_.size() < frame_samples_) return false;
        std::memcpy(out_frame, accumulator_.data(), frame_bytes_);
        accumulator_.erase(accumulator_.begin(), accumulator_.begin() + frame_samples_);
        frames_bypassed_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    std::string get_metrics_json() const {
        std::stringstream ss;
        ss << "{\"mode\":\"bypass\",\"filters_removed\":true,"
           << "\"hpf_enabled\":false,\"speex_enabled\":false,"
           << "\"frames_processed\":0,\"frames_bypassed\":"
           << frames_bypassed_.load(std::memory_order_relaxed)
           << ",\"overflow_drops\":" << overflow_count_.load(std::memory_order_relaxed)
           << ",\"last_process_us\":0,\"avg_process_us\":0,"
           << "\"peak_process_us\":0,\"cpu_pct_core\":0}";
        return ss.str();
    }

private:
    int sample_rate_ = 48000;
    size_t frame_samples_ = FRAME_SAMPLES;
    size_t frame_bytes_ = FRAME_BYTES;
    size_t max_accumulator_samples_ = MAX_ACCUMULATOR_SAMPLES;
    std::mutex mutex_;
    std::vector<int16_t> accumulator_;
    std::chrono::steady_clock::time_point last_flush_;
    std::atomic<uint64_t> frames_bypassed_{0};
    std::atomic<uint64_t> overflow_count_{0};
};

} // namespace maix::webrtc

#endif // MAIX_WEBRTC_AUDIO_DSP_HPP
