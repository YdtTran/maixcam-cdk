#include <iostream>
#include <vector>
#include <cmath>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sys/resource.h>
#include <speex/speex_preprocess.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// 2nd-order Butterworth IIR High-Pass Filter (Transposed Direct Form II, Single Precision)
class HighPassFilter2ndOrder {
public:
    HighPassFilter2ndOrder(float cutoff_hz = 50.0f, float sample_rate = 48000.0f, float q = 0.70710678f) {
        set_cutoff(cutoff_hz, sample_rate, q);
        reset();
    }

    void reset() {
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

        b0_ = static_cast<float>(b0 / a0);
        b1_ = static_cast<float>(b1 / a0);
        b2_ = static_cast<float>(b2 / a0);
        a1_ = static_cast<float>(a1 / a0);
        a2_ = static_cast<float>(a2 / a0);
        cutoff_hz_ = cutoff_hz;
        sample_rate_ = sample_rate;
    }

    void process(int16_t *samples, size_t count) {
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

    float cutoff_hz() const { return cutoff_hz_; }

private:
    float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f;
    float a1_ = 0.0f, a2_ = 0.0f;
    float s1_ = 0.0f, s2_ = 0.0f;
    float cutoff_hz_ = 50.0f;
    float sample_rate_ = 48000.0f;
};

// SpeexDSP Preprocessor Wrapper
class SpeexDenoiseProcessor {
public:
    SpeexDenoiseProcessor(int frame_samples = 960, int sample_rate = 48000, int suppress_db = -6)
        : frame_samples_(frame_samples), sample_rate_(sample_rate), suppress_db_(-std::abs(suppress_db)) {
        init();
    }

    ~SpeexDenoiseProcessor() {
        cleanup();
    }

    void reset() {
        cleanup();
        init();
    }

    void set_suppression_level(int suppress_db) {
        suppress_db_ = -std::abs(suppress_db);
        if (state_) {
            spx_int32_t val = suppress_db_;
            speex_preprocess_ctl(state_, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &val);
        }
    }

    void set_denoise(bool enabled) {
        denoise_enabled_ = enabled;
        if (state_) {
            spx_int32_t val = enabled ? 1 : 0;
            speex_preprocess_ctl(state_, SPEEX_PREPROCESS_SET_DENOISE, &val);
        }
    }

    void process(int16_t *samples) {
        if (!state_) return;
        speex_preprocess_run(state_, reinterpret_cast<spx_int16_t *>(samples));
    }

    int suppression_level() const { return suppress_db_; }

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

    SpeexPreprocessState *state_ = nullptr;
    int frame_samples_ = 960;
    int sample_rate_ = 48000;
    int suppress_db_ = -6;
    bool denoise_enabled_ = true;
};

// Simple WAV header structures
#pragma pack(push, 1)
struct WavHeader {
    char riff[4];
    uint32_t overall_size;
    char wave[4];
    char fmt_chunk_marker[4];
    uint32_t length_of_fmt;
    uint16_t format_type;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byterate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data_chunk_header[4];
    uint32_t data_size;
};
#pragma pack(pop)

bool load_wav(const std::string &path, std::vector<int16_t> &samples, int &sample_rate) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    WavHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (std::memcmp(header.riff, "RIFF", 4) != 0 || std::memcmp(header.wave, "WAVE", 4) != 0) {
        return false;
    }

    sample_rate = header.sample_rate;
    size_t num_samples = header.data_size / sizeof(int16_t);
    samples.resize(num_samples);
    file.read(reinterpret_cast<char*>(samples.data()), header.data_size);
    return true;
}

bool write_wav(const std::string &path, const std::vector<int16_t> &samples, int sample_rate) {
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;

    WavHeader header;
    std::memcpy(header.riff, "RIFF", 4);
    header.overall_size = sizeof(WavHeader) - 8 + samples.size() * sizeof(int16_t);
    std::memcpy(header.wave, "WAVE", 4);
    std::memcpy(header.fmt_chunk_marker, "fmt ", 4);
    header.length_of_fmt = 16;
    header.format_type = 1; // PCM
    header.channels = 1; // Mono
    header.sample_rate = sample_rate;
    header.byterate = sample_rate * sizeof(int16_t);
    header.block_align = sizeof(int16_t);
    header.bits_per_sample = 16;
    std::memcpy(header.data_chunk_header, "data", 4);
    header.data_size = samples.size() * sizeof(int16_t);

    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(samples.data()), header.data_size);
    return true;
}

struct AudioMetrics {
    double rms_dbfs;
    double peak_dbfs;
    uint64_t clipped_samples;
};

AudioMetrics compute_metrics(const int16_t *data, size_t count) {
    AudioMetrics m{0.0, -120.0, 0};
    if (count == 0) return m;

    double sum_sq = 0.0;
    int16_t max_abs = 0;

    for (size_t i = 0; i < count; ++i) {
        int16_t s = data[i];
        if (s == 32767 || s == -32768) {
            m.clipped_samples++;
        }
        int16_t abs_s = std::abs(s);
        if (abs_s > max_abs) max_abs = abs_s;
        double norm = static_cast<double>(s) / 32768.0;
        sum_sq += norm * norm;
    }

    double rms = std::sqrt(sum_sq / count);
    m.rms_dbfs = 20.0 * std::log10(rms + 1e-12);
    m.peak_dbfs = 20.0 * std::log10((static_cast<double>(max_abs) / 32768.0) + 1e-12);
    return m;
}

struct RunConfig {
    std::string name;
    std::string filename;
    std::string mode; // "bypass", "hpf_only", "full"
    float hpf_cutoff;
    int speex_suppress;
};

struct RunResult {
    RunConfig config;
    double time_us_per_frame;
    double cpu_pct_core;
    AudioMetrics overall;
    AudioMetrics background_04_10;
    AudioMetrics background_17_19;
    AudioMetrics quiet_singing_13_19;
    double bg_attenuation_db;
    double quiet_singing_diff_db;
};

int main(int argc, char **argv) {
    std::string input_path = (argc > 1) ? argv[1] : "scripts/recordings/sample_30s_live_mic.wav";
    std::string out_dir = (argc > 2) ? argv[2] : "build/recordings";

    std::vector<int16_t> original;
    int sample_rate = 48000;
    if (!load_wav(input_path, original, sample_rate)) {
        std::cerr << "Failed to load WAV file: " << input_path << "\n";
        return 1;
    }

    const size_t total_samples = original.size();
    const double duration_s = static_cast<double>(total_samples) / sample_rate;
    const size_t frame_samples = 960; // 20 ms
    const size_t num_frames = total_samples / frame_samples;

    std::cout << "=== SPEEXDSP & HPF TUNING BENCHMARK ===\n";
    std::cout << "Loaded " << input_path << ": " << total_samples << " samples ("
              << duration_s << "s), " << sample_rate << " Hz, " << num_frames << " frames\n\n";

    // Define segments:
    // 1. Candidate low-activity background segment: 04.0s - 10.0s (6.0s)
    const size_t bg1_start = 4 * sample_rate;
    const size_t bg1_len = 6 * sample_rate;

    // 2. Candidate low-activity background segment: 17.0s - 19.0s (2.0s)
    const size_t bg2_start = 17 * sample_rate;
    const size_t bg2_len = 2 * sample_rate;

    // 3. Quiet singing segment: 13.0s - 19.0s (6.0s)
    const size_t sing_start = 13 * sample_rate;
    const size_t sing_len = 6 * sample_rate;

    AudioMetrics orig_overall = compute_metrics(original.data(), total_samples);
    AudioMetrics orig_bg1 = compute_metrics(original.data() + bg1_start, bg1_len);
    AudioMetrics orig_bg2 = compute_metrics(original.data() + bg2_start, bg2_len);
    AudioMetrics orig_sing = compute_metrics(original.data() + sing_start, sing_len);

    std::vector<RunConfig> configs = {
        {"Bypass (Raw Mic)", "bypass_raw.wav", "bypass", 0.0f, 0},
        {"HPF 50 Hz Only", "hpf_only_50hz.wav", "hpf_only", 50.0f, 0},
        {"HPF 80 Hz Only", "hpf_only_80hz.wav", "hpf_only", 80.0f, 0},
        {"Baseline -6 dB (HPF 50Hz)", "baseline_-6db.wav", "full", 50.0f, -6},
        {"Suppression -9 dB (HPF 50Hz)", "suppression_-9db.wav", "full", 50.0f, -9},
        {"Suppression -12 dB (HPF 50Hz)", "suppression_-12db.wav", "full", 50.0f, -12},
        {"Suppression -15 dB (HPF 50Hz)", "suppression_-15db.wav", "full", 50.0f, -15},
        {"Suppression -18 dB (HPF 50Hz)", "suppression_-18db.wav", "full", 50.0f, -18},
        {"Suppression -21 dB (HPF 50Hz)", "suppression_-21db.wav", "full", 50.0f, -21},
        {"Suppression -12 dB (HPF 80Hz Voice)", "suppression_hpf80_voice_-12db.wav", "full", 80.0f, -12}
    };

    std::vector<RunResult> results;

    for (const auto &cfg : configs) {
        std::vector<int16_t> buf = original;
        HighPassFilter2ndOrder hpf(cfg.hpf_cutoff, sample_rate);
        SpeexDenoiseProcessor speex(frame_samples, sample_rate, cfg.speex_suppress);

        auto t0 = std::chrono::high_resolution_clock::now();
        for (size_t f = 0; f < num_frames; ++f) {
            int16_t *frame_ptr = buf.data() + f * frame_samples;
            if (cfg.mode == "full") {
                hpf.process(frame_ptr, frame_samples);
                speex.process(frame_ptr);
            } else if (cfg.mode == "hpf_only") {
                hpf.process(frame_ptr, frame_samples);
            } else {
                // bypass
            }
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double elapsed_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        double us_per_frame = elapsed_us / num_frames;
        double cpu_pct = (us_per_frame / 20000.0) * 100.0;

        RunResult res;
        res.config = cfg;
        res.time_us_per_frame = us_per_frame;
        res.cpu_pct_core = cpu_pct;
        res.overall = compute_metrics(buf.data(), total_samples);
        res.background_04_10 = compute_metrics(buf.data() + bg1_start, bg1_len);
        res.background_17_19 = compute_metrics(buf.data() + bg2_start, bg2_len);
        res.quiet_singing_13_19 = compute_metrics(buf.data() + sing_start, sing_len);
        res.bg_attenuation_db = orig_bg1.rms_dbfs - res.background_04_10.rms_dbfs;
        res.quiet_singing_diff_db = orig_sing.rms_dbfs - res.quiet_singing_13_19.rms_dbfs;

        write_wav(out_dir + "/" + cfg.filename, buf, sample_rate);
        results.push_back(res);

        std::cout << std::left << std::setw(36) << cfg.name
                  << " | " << std::right << std::setw(6) << std::fixed << std::setprecision(1) << us_per_frame << " us/f"
                  << " | CPU: " << std::setw(5) << std::setprecision(2) << cpu_pct << "%"
                  << " | BG RMS: " << std::setw(6) << std::setprecision(2) << res.background_04_10.rms_dbfs << " dB"
                  << " | BG Atten: " << std::setw(5) << std::setprecision(2) << res.bg_attenuation_db << " dB"
                  << " | Sing Diff: " << std::setw(5) << std::setprecision(2) << res.quiet_singing_diff_db << " dB"
                  << " | Clips: " << res.overall.clipped_samples << "\n";
    }

    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    long max_rss_kb = usage.ru_maxrss;
    std::cout << "\nPeak Memory (RSS): " << max_rss_kb << " KB\n";

    // Export JSON
    std::ofstream json_file(out_dir + "/tuning_benchmark_result.json");
    json_file << "{\n"
              << "  \"sample_rate\": " << sample_rate << ",\n"
              << "  \"duration_s\": " << duration_s << ",\n"
              << "  \"total_frames\": " << num_frames << ",\n"
              << "  \"peak_memory_rss_kb\": " << max_rss_kb << ",\n"
              << "  \"original_reference\": {\n"
              << "    \"overall_rms_dbfs\": " << orig_overall.rms_dbfs << ",\n"
              << "    \"background_04_10_rms_dbfs\": " << orig_bg1.rms_dbfs << ",\n"
              << "    \"background_17_19_rms_dbfs\": " << orig_bg2.rms_dbfs << ",\n"
              << "    \"quiet_singing_13_19_rms_dbfs\": " << orig_sing.rms_dbfs << ",\n"
              << "    \"clipped_samples\": " << orig_overall.clipped_samples << "\n"
              << "  },\n"
              << "  \"runs\": [\n";

    for (size_t i = 0; i < results.size(); ++i) {
        const auto &r = results[i];
        json_file << "    {\n"
                  << "      \"name\": \"" << r.config.name << "\",\n"
                  << "      \"filename\": \"" << r.config.filename << "\",\n"
                  << "      \"mode\": \"" << r.config.mode << "\",\n"
                  << "      \"hpf_cutoff_hz\": " << r.config.hpf_cutoff << ",\n"
                  << "      \"speex_suppress_db\": " << r.config.speex_suppress << ",\n"
                  << "      \"us_per_frame\": " << r.time_us_per_frame << ",\n"
                  << "      \"cpu_pct_core\": " << r.cpu_pct_core << ",\n"
                  << "      \"overall_rms_dbfs\": " << r.overall.rms_dbfs << ",\n"
                  << "      \"background_04_10_rms_dbfs\": " << r.background_04_10.rms_dbfs << ",\n"
                  << "      \"background_17_19_rms_dbfs\": " << r.background_17_19.rms_dbfs << ",\n"
                  << "      \"quiet_singing_13_19_rms_dbfs\": " << r.quiet_singing_13_19.rms_dbfs << ",\n"
                  << "      \"bg_attenuation_db\": " << r.bg_attenuation_db << ",\n"
                  << "      \"quiet_singing_diff_db\": " << r.quiet_singing_diff_db << ",\n"
                  << "      \"clipped_samples\": " << r.overall.clipped_samples << "\n"
                  << "    }" << (i + 1 < results.size() ? "," : "") << "\n";
    }
    json_file << "  ]\n}\n";

    std::cout << "\nResults and audio files exported to: " << out_dir << "/\n";
    return 0;
}
