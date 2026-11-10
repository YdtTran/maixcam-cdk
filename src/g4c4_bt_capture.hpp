#pragma once

#include <alsa/asoundlib.h>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <cerrno>
#include <stdexcept>
#include <string>
#include <vector>

// Request 48 kHz mono through the BlueALSA PCM conversion plugin,
// keeping the existing uplink DSP and RTP clock unchanged.
class G4c4BtCapture final {
public:
    G4c4BtCapture() = default;
    ~G4c4BtCapture() { close(); }
    G4c4BtCapture(const G4c4BtCapture&) = delete;
    G4c4BtCapture& operator=(const G4c4BtCapture&) = delete;

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
        unsigned int rate = 48000;
        unsigned int period_us = 20000;
        unsigned int buffer_us = 80000;
        if ((result = snd_pcm_hw_params_any(pcm_, hw)) < 0 ||
            (result = snd_pcm_hw_params_set_rate_resample(pcm_, hw, 1)) < 0 ||
            (result = snd_pcm_hw_params_set_access(pcm_, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
            (result = snd_pcm_hw_params_set_format(pcm_, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
            (result = snd_pcm_hw_params_set_channels(pcm_, hw, 1)) < 0 ||
            (result = snd_pcm_hw_params_set_rate_near(pcm_, hw, &rate, nullptr)) < 0 ||
            (result = snd_pcm_hw_params_set_period_time_near(pcm_, hw, &period_us, nullptr)) < 0 ||
            (result = snd_pcm_hw_params_set_buffer_time_near(pcm_, hw, &buffer_us, nullptr)) < 0 ||
            (result = snd_pcm_hw_params(pcm_, hw)) < 0) {
            result = snd_pcm_set_params(pcm_, SND_PCM_FORMAT_S16_LE,
                SND_PCM_ACCESS_RW_INTERLEAVED, 1, 48000, 1, 80000);
        }
        if (result < 0) {
            close();
            fail("configure", result);
        }
        std::fprintf(stderr, "[Bluetooth microphone] Opened %s (48000 Hz mono S16_LE)\n", device);
    }

    const std::vector<int16_t>& read() {
        open();
        int16_t input[960];
        const snd_pcm_sframes_t count = snd_pcm_readi(pcm_, input, 960);
        mono_.clear();
        if (count == -EAGAIN || count == 0) return mono_;
        if (count < 0) {
            const int result = snd_pcm_recover(pcm_, static_cast<int>(count), 1);
            if (result < 0) fail("read", result);
            return mono_;
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
    snd_pcm_t *pcm_ = nullptr;
    std::vector<int16_t> mono_;
};
