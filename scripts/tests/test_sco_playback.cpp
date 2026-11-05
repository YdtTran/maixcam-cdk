#include "g4c4_opus_rx.hpp"
#include <cassert>
#include <cmath>
#include <atomic>
#include <mutex>

static std::atomic<unsigned> configured_rate{0}, configured_channels{0};
static std::mutex output_mutex;
static std::vector<int16_t> output;
extern "C" {
int snd_pcm_open(snd_pcm_t **pcm, const char *, snd_pcm_stream_t, int) {
    *pcm = reinterpret_cast<snd_pcm_t *>(1);
    return 0;
}
int snd_pcm_set_params(snd_pcm_t *, snd_pcm_format_t, snd_pcm_access_t,
                       unsigned channels, unsigned rate, int, unsigned) {
    configured_channels = channels;
    configured_rate = rate;
    return 0;
}
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *, const void *pcm, snd_pcm_uframes_t frames) {
    std::lock_guard<std::mutex> lock(output_mutex);
    const auto *samples = static_cast<const int16_t *>(pcm);
    output.insert(output.end(), samples, samples + frames * configured_channels);
    return frames;
}
int snd_pcm_drop(snd_pcm_t *) { return 0; }
int snd_pcm_prepare(snd_pcm_t *) { return 0; }
int snd_pcm_close(snd_pcm_t *) { return 0; }
}

int main() {
    auto playback = std::make_shared<G4c4BtPlayback>(
        "bluealsa:DEV=6C:16:29:7B:00:D4,PROFILE=sco");
    for (int i = 0; i < 100 && !configured_rate; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (configured_rate != 8000 || configured_channels != 1) {
        std::fprintf(stderr, "FAIL: SCO configured as %u Hz / %u channels, expected 8000 / 1\n",
                     configured_rate.load(), configured_channels.load());
        return 1;
    }
    G4c4OpusRx rx(playback);
    int error = OPUS_OK;
    auto *encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_AUDIO, &error);
    assert(encoder && error == OPUS_OK);
    auto *reference = opus_decoder_create(8000, 1, &error);
    assert(reference && error == OPUS_OK);
    std::vector<int16_t> expected;
    for (int packet = 0; packet < 2; ++packet) {
        int16_t input[960], decoded[960];
        for (int i = 0; i < 960; ++i)
            input[i] = 2000 * std::sin(2 * 3.141592653589793 * 440 * (i + packet * 960) / 48000);
        unsigned char encoded[1275];
        int bytes = opus_encode(encoder, input, 960, encoded, sizeof(encoded));
        assert(bytes > 0);
        int samples = opus_decode(reference, encoded, bytes, decoded, 960, 0);
        assert(samples == 160);
        expected.insert(expected.end(), decoded, decoded + samples);
        rx.enqueue(encoded, bytes, packet * 960, 97);
    }
    bool received = false;
    for (int i = 0; i < 200; ++i) {
        {
            std::lock_guard<std::mutex> lock(output_mutex);
            if (output.size() >= expected.size()) {
                assert(output == expected);
                received = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(received);
    assert(rx.get_stats().resampler.process_calls == 0);
    opus_encoder_destroy(encoder);
    opus_decoder_destroy(reference);
    std::puts("Opus -> native SCO PCM, unchanged samples: PASS");
}
