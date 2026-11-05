#ifndef MAIX_WEBRTC_AUDIO_DSP_HPP
#define MAIX_WEBRTC_AUDIO_DSP_HPP

#include <algorithm>
#include <atomic>
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

    MaixAudioUplinkDSP() { accumulator_.reserve(MAX_ACCUMULATOR_SAMPLES); }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        accumulator_.clear();
    }

    void ingest_bytes(const void *data, size_t bytes) {
        if (!data || bytes == 0) return;
        size_t samples_in = bytes / sizeof(int16_t);
        const auto *src = static_cast<const int16_t *>(data);
        std::lock_guard<std::mutex> lock(mutex_);
        if (samples_in > MAX_ACCUMULATOR_SAMPLES) {
            src += samples_in - MAX_ACCUMULATOR_SAMPLES;
            samples_in = MAX_ACCUMULATOR_SAMPLES;
            accumulator_.clear();
            overflow_count_.fetch_add(1, std::memory_order_relaxed);
        } else if (accumulator_.size() + samples_in > MAX_ACCUMULATOR_SAMPLES) {
            const size_t drop = accumulator_.size() + samples_in - MAX_ACCUMULATOR_SAMPLES;
            accumulator_.erase(accumulator_.begin(), accumulator_.begin() + drop);
            overflow_count_.fetch_add(1, std::memory_order_relaxed);
        }
        accumulator_.insert(accumulator_.end(), src, src + samples_in);
    }

    bool has_frame() {
        std::lock_guard<std::mutex> lock(mutex_);
        return accumulator_.size() >= FRAME_SAMPLES;
    }

    size_t queued_samples() {
        std::lock_guard<std::mutex> lock(mutex_);
        return accumulator_.size();
    }

    size_t queued_bytes() { return queued_samples() * sizeof(int16_t); }

    bool pop_and_process_frame(int16_t *out_frame) {
        if (!out_frame) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (accumulator_.size() < FRAME_SAMPLES) return false;
        std::memcpy(out_frame, accumulator_.data(), FRAME_BYTES);
        accumulator_.erase(accumulator_.begin(), accumulator_.begin() + FRAME_SAMPLES);
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
    std::mutex mutex_;
    std::vector<int16_t> accumulator_;
    std::atomic<uint64_t> frames_bypassed_{0};
    std::atomic<uint64_t> overflow_count_{0};
};

} // namespace maix::webrtc

#endif // MAIX_WEBRTC_AUDIO_DSP_HPP
