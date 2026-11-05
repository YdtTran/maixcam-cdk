#include "maix_webrtc_audio_dsp.hpp"
#include <array>
#include <cassert>
#include <vector>

int main() {
    maix::webrtc::MaixAudioUplinkDSP audio;
    std::vector<int16_t> source(1920);
    for (size_t i = 0; i < source.size(); ++i) {
        // DC, quiet samples and full-scale samples must all survive unchanged.
        constexpr int16_t values[] = {0, 1, -1, 1200, 1200, 32767, -32768};
        source[i] = values[i % 7];
    }
    audio.ingest_bytes(source.data(), 137 * sizeof(int16_t));
    assert(!audio.has_frame());
    audio.ingest_bytes(source.data() + 137, (source.size() - 137) * sizeof(int16_t));
    std::array<int16_t, 960> frame{};
    for (size_t offset = 0; offset < source.size(); offset += frame.size()) {
        assert(audio.pop_and_process_frame(frame.data()));
        for (size_t i = 0; i < frame.size(); ++i) assert(frame[i] == source[offset + i]);
    }
    assert(!audio.has_frame());
    audio.ingest_bytes(source.data(), 100);
    audio.reset();
    assert(audio.queued_bytes() == 0);
    std::vector<int16_t> overflow(6000, 123);
    audio.ingest_bytes(overflow.data(), overflow.size() * sizeof(int16_t));
    assert(audio.queued_samples() == audio.MAX_ACCUMULATOR_SAMPLES);
    assert(audio.get_metrics_json().find("\"filters_removed\":true") != std::string::npos);
}
