#pragma once

#include <alsa/asoundlib.h>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <cerrno>
#include <stdexcept>
#include <string>
#include <vector>

// Native 8 kHz mono capture for BlueALSA SCO (CVSD), avoiding ALSA rate plugin
// resampling overhead while keeping the 48 kHz WebRTC RTP clock unchanged.
class G4c4BtCapture final {
public:
    static constexpr int64_t PERIODIC_FLUSH_MS = 5000;
    static constexpr unsigned int DEFAULT_RATE = 8000;
    static constexpr size_t DEFAULT_FRAME_SAMPLES = 160;

    explicit G4c4BtCapture(unsigned int rate = DEFAULT_RATE)
        : rate_(rate), frame_samples_(rate / 50) {}
    ~G4c4BtCapture() { close(); }
    G4c4BtCapture(const G4c4BtCapture&) = delete;
    G4c4BtCapture& operator=(const G4c4BtCapture&) = delete;

    unsigned int sample_rate() const { return rate_; }
    size_t frame_samples() const { return frame_samples_; }

    void reset() { close(); }

    void open() {
        if (pcm_) return;
        const char *configured = std::getenv("MAIX_WEBRTC_BT_CAPTURE_DEVICE");
        const char *device = configured && *configured ? configured
            : "bluealsa:DEV=6C:16:29:7B:00:D4,PROFILE=sco";
        int result = snd_pcm_open(&pcm_, device, SND_PCM_STREAM_CAPTURE,
                                  SND_PCM_NONBLOCK);
        if (result < 0) {
            pcm_ = nullptr;
            fail("open", result);
        }
        snd_pcm_hw_params_t *hw = nullptr;
        snd_pcm_hw_params_alloca(&hw);
        unsigned int rate = rate_;
        unsigned int period_us = 20000;
        unsigned int buffer_us = 80000;
        if ((result = snd_pcm_hw_params_any(pcm_, hw)) < 0 ||
            (result = snd_pcm_hw_params_set_rate_resample(pcm_, hw, 0)) < 0 ||
            (result = snd_pcm_hw_params_set_access(pcm_, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
            (result = snd_pcm_hw_params_set_format(pcm_, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
            (result = snd_pcm_hw_params_set_channels(pcm_, hw, 1)) < 0 ||
            (result = snd_pcm_hw_params_set_rate_near(pcm_, hw, &rate, nullptr)) < 0 ||
            (result = snd_pcm_hw_params_set_period_time_near(pcm_, hw, &period_us, nullptr)) < 0 ||
            (result = snd_pcm_hw_params_set_buffer_time_near(pcm_, hw, &buffer_us, nullptr)) < 0 ||
            (result = snd_pcm_hw_params(pcm_, hw)) < 0) {
            result = snd_pcm_set_params(pcm_, SND_PCM_FORMAT_S16_LE,
                SND_PCM_ACCESS_RW_INTERLEAVED, 1, rate_, 1, 80000);
        }
        if (result < 0) {
            close();
            fail("configure", result);
        }
        std::fprintf(stderr, "[Bluetooth microphone] Opened %s (%u Hz mono S16_LE)\n", device, rate);
        last_flush_ = std::chrono::steady_clock::now();
    }

    const std::vector<int16_t>& read() {
        open();
        int16_t input[960];
        const snd_pcm_uframes_t req = frame_samples_ <= 960 ? frame_samples_ : 960;
        snd_pcm_sframes_t count = snd_pcm_readi(pcm_, input, req);
        mono_.clear();
        if (count == -EAGAIN) {
            // Event-driven wait for next 20ms period instead of 5ms polling
            snd_pcm_wait(pcm_, 25);
            count = snd_pcm_readi(pcm_, input, req);
            if (count == -EAGAIN || count == 0) return mono_;
        } else if (count == 0) {
            return mono_;
        }
        if (count < 0) {
            const int result = snd_pcm_recover(pcm_, static_cast<int>(count), 1);
            if (result < 0) fail("read", result);
            return mono_;
        }
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush_).count() >= PERIODIC_FLUSH_MS) {
            last_flush_ = now;
            // Drain any stale queued periods in the non-blocking ALSA buffer, keeping the newest chunk.
            for (int i = 0; i < 4; ++i) {
                const snd_pcm_sframes_t newer = snd_pcm_readi(pcm_, input, req);
                if (newer <= 0) break;
                count = newer;
            }
        }
        mono_.assign(input, input + count);
        return mono_;
    }

private:
    static void fail(const char *operation, int result) {
        throw std::runtime_error(std::string("Bluetooth microphone ") +
                                 operation + ": " + snd_strerror(result));
    }
    void close() {
        if (pcm_) snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }
    unsigned int rate_ = DEFAULT_RATE;
    size_t frame_samples_ = DEFAULT_FRAME_SAMPLES;
    snd_pcm_t *pcm_ = nullptr;
    std::chrono::steady_clock::time_point last_flush_{};
    std::vector<int16_t> mono_;
};
