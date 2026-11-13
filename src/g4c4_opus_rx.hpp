#pragma once

#include <opus.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "g4c4_resampler.hpp"
#include "g4c4_bt_playback.hpp"
#include "g4c4_perf.hpp"
#include <cmath>

/**
 * @brief Dedicated Opus Downlink Receiver and Decoder Worker for MaixCAM.
 *
 * Runs an independent worker thread to decode incoming WebRTC Opus audio (8 kHz mono),
 * resample it via cubic interpolation to 48 kHz stereo, and deliver it to G4c4BtPlayback.
 *
 * Integrated with monotonic telemetry and instrumentation (PLAN-07).
 */
class G4c4OpusRx final {
public:
    struct RxStats {
        uint64_t packets_received = 0;
        uint64_t packets_enqueued = 0;
        uint64_t dropped_busy = 0;
        uint64_t dropped_overflow = 0;
        uint64_t dropped_inactive = 0;
        uint64_t packets_decoded = 0;
        uint64_t decode_errors = 0;
        uint64_t samples_decoded = 0;
        uint64_t decoder_resets = 0;
        uint64_t talk_on_events = 0;
        uint64_t talk_off_events = 0;
        uint64_t talk_off_dropped_packets = 0;
        size_t current_queue_depth = 0;
        size_t peak_queue_depth = 0;
        G4c4Resampler::Stats resampler{};
        G4c4BtPlayback::Stats playback{};
    };

    explicit G4c4OpusRx(std::shared_ptr<G4c4BtPlayback> playback = nullptr)
        : playback_(playback ? playback : std::make_shared<G4c4BtPlayback>())
        , worker_(&G4c4OpusRx::run, this) {}

    ~G4c4OpusRx() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            queue_.clear();
            current_queue_depth_.store(0, std::memory_order_relaxed);
        }
        cv_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

    G4c4OpusRx(const G4c4OpusRx&) = delete;
    G4c4OpusRx& operator=(const G4c4OpusRx&) = delete;

    void set_talk_state(bool active) {
        const bool prev = talk_active_.exchange(active, std::memory_order_seq_cst);
        if (prev != active) {
            if (active) {
                talk_on_events_.fetch_add(1, std::memory_order_relaxed);
            } else {
                talk_off_events_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!active) {
                // Immediate invalidation: drop pending Opus packets
                talk_off_dropped_packets_.fetch_add(queue_.size(), std::memory_order_relaxed);
                queue_.clear();
                current_queue_depth_.store(0, std::memory_order_relaxed);
                need_decoder_reset_ = true;
            }
        }
        if (playback_) {
            playback_->set_talk_active(active);
        }
    }

    bool is_talk_active() const {
        return talk_active_.load(std::memory_order_relaxed);
    }

    void enqueue(
        const void* data,
        size_t size,
        uint32_t timestamp,
        uint8_t payload_type
    ) {
        packets_received_.fetch_add(1, std::memory_order_relaxed);
        if (diag_disable_rx_) {
            dropped_inactive_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        if (!data || size == 0 || size > 512) {
            dropped_busy_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        Packet packet;
        packet.size = static_cast<uint16_t>(size);
        std::memcpy(packet.bytes.data(), data, size);
        packet.timestamp = timestamp;
        packet.payload_type = payload_type;

        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }

        if (!talk_active_.load(std::memory_order_relaxed)) {
            dropped_inactive_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        if (queue_.size() >= 25) {
            queue_.pop_front();
            dropped_overflow_.fetch_add(1, std::memory_order_relaxed);
        }

        queue_.push_back(std::move(packet));
        const size_t qsize = queue_.size();
        current_queue_depth_.store(qsize, std::memory_order_relaxed);

        size_t cur_peak = peak_queue_depth_.load(std::memory_order_relaxed);
        while (qsize > cur_peak && !peak_queue_depth_.compare_exchange_weak(cur_peak, qsize, std::memory_order_relaxed)) {}

        packets_enqueued_.fetch_add(1, std::memory_order_relaxed);
        cv_.notify_one();
    }

    RxStats get_stats() const {
        RxStats s;
        s.packets_received = packets_received_.load(std::memory_order_relaxed);
        s.packets_enqueued = packets_enqueued_.load(std::memory_order_relaxed);
        s.dropped_busy = dropped_busy_.load(std::memory_order_relaxed);
        s.dropped_overflow = dropped_overflow_.load(std::memory_order_relaxed);
        s.dropped_inactive = dropped_inactive_.load(std::memory_order_relaxed);
        s.packets_decoded = packets_decoded_.load(std::memory_order_relaxed);
        s.decode_errors = decode_errors_.load(std::memory_order_relaxed);
        s.samples_decoded = samples_decoded_.load(std::memory_order_relaxed);
        s.decoder_resets = decoder_resets_.load(std::memory_order_relaxed);
        s.talk_on_events = talk_on_events_.load(std::memory_order_relaxed);
        s.talk_off_events = talk_off_events_.load(std::memory_order_relaxed);
        s.talk_off_dropped_packets = talk_off_dropped_packets_.load(std::memory_order_relaxed);
        s.current_queue_depth = current_queue_depth_.load(std::memory_order_relaxed);
        s.peak_queue_depth = peak_queue_depth_.load(std::memory_order_relaxed);
        s.resampler = resampler_.get_stats();
        if (playback_) {
            s.playback = playback_->get_stats();
        }
        return s;
    }

private:
    struct Packet {
        std::array<unsigned char, 512> bytes{};
        uint16_t size = 0;
        uint32_t timestamp = 0;
        uint8_t payload_type = 0;
    };

    void run() {
        g4c4_perf::set_thread_name("opus_rx");
        if (g4c4_perf::env_flag("MAIX_DIAG_LOCAL_PCM_PLAYBACK")) {
            std::array<int16_t, 160> tone{};
            double phase = 0.0;
            const double step = 6.28318530717958647692 * 440.0 / 8000.0;
            auto next = std::chrono::steady_clock::now();
            while (!stopping_) {
                if (playback_ && talk_active_.load(std::memory_order_relaxed)) {
                    for (size_t i = 0; i < tone.size(); ++i) {
                        tone[i] = static_cast<int16_t>(std::sin(phase) * 8000.0);
                        phase += step;
                        if (phase >= 6.28318530717958647692) phase -= 6.28318530717958647692;
                    }
                    playback_->enqueue_pcm(tone.data(), tone.size());
                }
                next += std::chrono::milliseconds(20);
                std::this_thread::sleep_until(next);
            }
            return;
        }
        int error = OPUS_OK;
        OpusDecoder* decoder = opus_decoder_create(
            8000, 1, &error
        );

        if (!decoder || error != OPUS_OK) {
            std::fprintf(stderr,
                "[G4c4 RX] decoder init failed: %d\n", error);
            if (decoder) opus_decoder_destroy(decoder);
            return;
        }

        // Maximum Opus packet duration is 120 ms.
        // 8000 Hz * 0.120 s = 960 mono samples.
        std::array<opus_int16, 960> pcm{};
        std::vector<int16_t> stereo_resampled;
        auto& rperf = g4c4_perf::stats().rx;
        uint64_t cpu_prev = g4c4_perf::thread_cpu_ns();

        for (;;) {
            Packet packet;
            bool do_reset = false;

            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(250), [this] {
                    return stopping_ || !queue_.empty() || need_decoder_reset_;
                });

                if (stopping_) break;

                if (need_decoder_reset_) {
                    do_reset = true;
                    need_decoder_reset_ = false;
                }

                if (!queue_.empty()) {
                    packet = std::move(queue_.front());
                    queue_.pop_front();
                    current_queue_depth_.store(queue_.size(), std::memory_order_relaxed);
                }
            }

            if (do_reset) {
                decoder_resets_.fetch_add(1, std::memory_order_relaxed);
                opus_decoder_ctl(decoder, OPUS_RESET_STATE);
                resampler_.reset();
            }

            if (packet.size == 0) {
                // Stream idle: notify playback worker to flush any stale tail
                if (playback_ && !diag_disable_pb_) {
                    playback_->notify_stream_idle();
                }
                uint64_t cpu_now = g4c4_perf::thread_cpu_ns();
                rperf.thread_cpu_ns.fetch_add(cpu_now - cpu_prev, std::memory_order_relaxed);
                cpu_prev = cpu_now;
                continue;
            }

            uint64_t td0 = g4c4_perf::mono_ns();
            int samples = opus_decode(
                decoder,
                packet.bytes.data(),
                static_cast<opus_int32>(packet.size),
                pcm.data(),
                static_cast<int>(pcm.size()),
                0
            );
            rperf.opus_decode_ns.fetch_add(g4c4_perf::mono_ns() - td0, std::memory_order_relaxed);
            rperf.packets_decoded.fetch_add(1, std::memory_order_relaxed);

            if (samples < 0) {
                const uint64_t failures = decode_errors_.fetch_add(1, std::memory_order_relaxed) + 1;
                if (failures <= 5 || failures % 100 == 0) {
                    std::fprintf(stderr,
                        "[G4c4 RX] decode error=%d failures=%llu\n",
                        samples,
                        static_cast<unsigned long long>(failures));
                }
                uint64_t cpu_now = g4c4_perf::thread_cpu_ns();
                rperf.thread_cpu_ns.fetch_add(cpu_now - cpu_prev, std::memory_order_relaxed);
                cpu_prev = cpu_now;
                continue;
            }

            const uint64_t decoded_total = packets_decoded_.fetch_add(1, std::memory_order_relaxed) + 1;
            samples_decoded_.fetch_add(static_cast<uint64_t>(samples), std::memory_order_relaxed);

            if (!diag_disable_pb_) {
                if (playback_ && playback_->uses_native_sco()) {
                    // CVSD already uses the decoder's 8 kHz mono S16 format.
                    if (talk_active_.load(std::memory_order_relaxed))
                        playback_->enqueue_pcm(pcm.data(), static_cast<size_t>(samples));
                } else {
                    // Retain conversion only for explicitly selected A2DP playback.
                    resampler_.process(pcm.data(), static_cast<size_t>(samples), stereo_resampled);
                    if (playback_ && talk_active_.load(std::memory_order_relaxed)) {
                        playback_->enqueue_pcm(stereo_resampled.data(),
                            static_cast<size_t>(samples * G4c4Resampler::RATIO));
                    }
                }
            }

            uint64_t cpu_now = g4c4_perf::thread_cpu_ns();
            rperf.thread_cpu_ns.fetch_add(cpu_now - cpu_prev, std::memory_order_relaxed);
            cpu_prev = cpu_now;

            if (decoded_total == 1 || decoded_total % 250 == 0) {
                auto pb_stats = playback_ ? playback_->get_stats() : G4c4BtPlayback::Stats{};
                auto rs_stats = resampler_.get_stats();
                std::fprintf(stderr,
                    "[G4c4 RX] decoded=%llu samples=%llu last_frame=%d "
                    "resamp_in=%llu resamp_out=%llu clips=%llu "
                    "bt_written=%llu bt_underruns=%llu bt_errs=%llu bt_dropped=%llu\n",
                    static_cast<unsigned long long>(decoded_total),
                    static_cast<unsigned long long>(samples_decoded_.load(std::memory_order_relaxed)),
                    samples,
                    static_cast<unsigned long long>(rs_stats.frames_in),
                    static_cast<unsigned long long>(rs_stats.frames_out),
                    static_cast<unsigned long long>(rs_stats.clips),
                    static_cast<unsigned long long>(pb_stats.frames_written),
                    static_cast<unsigned long long>(pb_stats.underruns),
                    static_cast<unsigned long long>(pb_stats.alsa_errors),
                    static_cast<unsigned long long>(pb_stats.dropped_overflow));
            }
        }

        opus_decoder_destroy(decoder);
    }

    std::shared_ptr<G4c4BtPlayback> playback_;
    mutable G4c4Resampler resampler_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Packet> queue_;
    bool stopping_ = false;
    bool need_decoder_reset_ = false;
    const bool diag_disable_rx_ = g4c4_perf::env_flag("MAIX_DIAG_DISABLE_OPUS_RX");
    const bool diag_disable_pb_ = g4c4_perf::env_flag("MAIX_DIAG_DISABLE_BT_PLAYBACK");
    std::atomic<bool> talk_active_{true};

    // Monotonic metrics tracking (PLAN-07)
    std::atomic<uint64_t> packets_received_{0};
    std::atomic<uint64_t> packets_enqueued_{0};
    std::atomic<uint64_t> dropped_busy_{0};
    std::atomic<uint64_t> dropped_overflow_{0};
    std::atomic<uint64_t> dropped_inactive_{0};
    std::atomic<uint64_t> packets_decoded_{0};
    std::atomic<uint64_t> decode_errors_{0};
    std::atomic<uint64_t> samples_decoded_{0};
    std::atomic<uint64_t> decoder_resets_{0};
    std::atomic<uint64_t> talk_on_events_{0};
    std::atomic<uint64_t> talk_off_events_{0};
    std::atomic<uint64_t> talk_off_dropped_packets_{0};
    std::atomic<size_t> current_queue_depth_{0};
    std::atomic<size_t> peak_queue_depth_{0};

    std::thread worker_;
};
