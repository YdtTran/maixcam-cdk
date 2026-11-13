#pragma once

#include <alsa/asoundlib.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "g4c4_perf.hpp"

/**
 * @brief Dedicated Bluetooth ALSA PCM Playback Worker for MaixCAM.
 *
 * Runs an independent worker thread to decouple WebRTC reception and Opus decoding
 * from potentially blocking Bluetooth/BlueALSA transport writes.
 *
 * Key features:
 * - Bounded playback queue (deterministic overflow drops oldest frame).
 * - Full ALSA partial write, underrun (-EPIPE), and disconnect recovery.
 * - Immediate Talk OFF buffer invalidation and hardware drop (snd_pcm_drop).
 * - Zero use of snd_pcm_drain (which deadlocks on BlueALSA A2DP).
 * - Resilient: Bluetooth failure NEVER crashes video or microphone capture.
 * - Complete monotonic telemetry and instrumentation (PLAN-07).
 */
class G4c4BtPlayback final {
public:
    static constexpr size_t MAX_QUEUE_FRAMES = 25; // 25 * 20ms = 500ms max jitter buffer
    static constexpr size_t FLUSH_KEEP_FRAMES = 2; // Keep newest 1-2 frames (40ms) on periodic flush
    static constexpr int64_t PERIODIC_FLUSH_MS = 5000;
    static constexpr unsigned int SAMPLE_RATE = 48000;
    static constexpr unsigned int CHANNELS = 2;

    struct Stats {
        unsigned sample_rate = 0;
        unsigned channels = 0;
        uint64_t frames_written = 0;
        uint64_t dropped_overflow = 0;
        uint64_t dropped_busy = 0;
        uint64_t alsa_errors = 0;
        uint64_t underruns = 0;
        uint64_t talk_on_events = 0;
        uint64_t talk_off_events = 0;
        uint64_t talk_off_dropped_chunks = 0;
        uint64_t talk_off_pcm_drops = 0;
        size_t current_queue_depth = 0;
        size_t peak_queue_depth = 0;
        uint64_t device_open_count = 0;
        uint64_t device_close_count = 0;
        int64_t last_write_us = 0;
        bool is_device_open = false;
    };

    explicit G4c4BtPlayback(const char* device_name = nullptr)
        : device_name_(device_name ? device_name : default_device())
        , native_sco_(device_name_.find("PROFILE=sco") != std::string::npos)
        , sample_rate_(native_sco_ ? 8000 : SAMPLE_RATE)
        , channels_(native_sco_ ? 1 : CHANNELS)
        , worker_(&G4c4BtPlayback::worker_loop, this) {
    }

    ~G4c4BtPlayback() {
        stop();
    }

    G4c4BtPlayback(const G4c4BtPlayback&) = delete;
    G4c4BtPlayback& operator=(const G4c4BtPlayback&) = delete;

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return;
            stopping_ = true;
            queue_.clear();
            current_queue_depth_.store(0, std::memory_order_relaxed);
        }
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    /**
     * @brief Set Talk gating state.
     * When false (Talk OFF), immediately flushes pending queue and drops hardware buffer.
     */
    void set_talk_active(bool active) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (talk_active_ == active) return;
        talk_active_ = active;

        if (active) {
            talk_on_events_.fetch_add(1, std::memory_order_relaxed);
            is_prebuffering_ = true;
        } else {
            talk_off_events_.fetch_add(1, std::memory_order_relaxed);
            // Talk OFF: invalidate all pending buffers immediately
            talk_off_dropped_chunks_.fetch_add(queue_.size(), std::memory_order_relaxed);
            queue_.clear();
            current_queue_depth_.store(0, std::memory_order_relaxed);
            need_pcm_drop_ = true;
            is_prebuffering_ = true;
            last_periodic_flush_ = std::chrono::steady_clock::time_point{};
        }
        cv_.notify_one();
    }

    bool is_talk_active() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return talk_active_;
    }

    bool uses_native_sco() const { return native_sco_; }

    /**
     * @brief Enqueue native S16_LE PCM (8 kHz mono SCO or 48 kHz stereo A2DP).
     *
     * @param pcm Interleaved S16 samples (num_frames * configured channels).
     * @param num_frames Number of frames per channel.
     */
    void enqueue_pcm(const int16_t* pcm, size_t num_frames) {
        if (!pcm || num_frames == 0) return;

        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || !talk_active_) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (last_periodic_flush_.time_since_epoch().count() == 0) {
            last_periodic_flush_ = now;
        } else if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_periodic_flush_).count() >= PERIODIC_FLUSH_MS) {
            last_periodic_flush_ = now;
            if (queue_.size() >= FLUSH_KEEP_FRAMES) {
                const size_t drop = queue_.size() - (FLUSH_KEEP_FRAMES - 1);
                for (size_t i = 0; i < drop; ++i) queue_.pop_front();
                dropped_overflow_.fetch_add(drop, std::memory_order_relaxed);
            }
        }

        // Deterministic overflow policy: drop oldest frame to prevent latency buildup
        if (queue_.size() >= MAX_QUEUE_FRAMES) {
            queue_.pop_front();
            dropped_overflow_.fetch_add(1, std::memory_order_relaxed);
        }

        Chunk chunk;
        chunk.samples.assign(pcm, pcm + (num_frames * channels_));
        chunk.num_frames = num_frames;

        queue_.push_back(std::move(chunk));
        const size_t qsize = queue_.size();
        current_queue_depth_.store(qsize, std::memory_order_relaxed);

        size_t cur_peak = peak_queue_depth_.load(std::memory_order_relaxed);
        while (qsize > cur_peak && !peak_queue_depth_.compare_exchange_weak(cur_peak, qsize, std::memory_order_relaxed)) {}

        last_enqueue_ = now;
        cv_.notify_one();
    }

    /**
     * @brief Periodic idle notification (called when no Opus packets arrive).
     */
    void notify_stream_idle() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (last_enqueue_.time_since_epoch().count() > 0) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_enqueue_).count() > 500) {
                if (!queue_.empty()) {
                    talk_off_dropped_chunks_.fetch_add(queue_.size(), std::memory_order_relaxed);
                    queue_.clear();
                    current_queue_depth_.store(0, std::memory_order_relaxed);
                }
                need_pcm_drop_ = true;
                last_enqueue_ = std::chrono::steady_clock::time_point{};
                last_periodic_flush_ = std::chrono::steady_clock::time_point{};
                cv_.notify_one();
            }
        }
    }

    Stats get_stats() const {
        Stats s;
        s.sample_rate = sample_rate_;
        s.channels = channels_;
        s.frames_written = frames_written_.load(std::memory_order_relaxed);
        s.dropped_overflow = dropped_overflow_.load(std::memory_order_relaxed);
        s.dropped_busy = dropped_busy_.load(std::memory_order_relaxed);
        s.alsa_errors = alsa_errors_.load(std::memory_order_relaxed);
        s.underruns = underruns_.load(std::memory_order_relaxed);
        s.talk_on_events = talk_on_events_.load(std::memory_order_relaxed);
        s.talk_off_events = talk_off_events_.load(std::memory_order_relaxed);
        s.talk_off_dropped_chunks = talk_off_dropped_chunks_.load(std::memory_order_relaxed);
        s.talk_off_pcm_drops = talk_off_pcm_drops_.load(std::memory_order_relaxed);
        s.current_queue_depth = current_queue_depth_.load(std::memory_order_relaxed);
        s.peak_queue_depth = peak_queue_depth_.load(std::memory_order_relaxed);
        s.device_open_count = device_open_count_.load(std::memory_order_relaxed);
        s.device_close_count = device_close_count_.load(std::memory_order_relaxed);
        s.last_write_us = last_write_us_.load(std::memory_order_relaxed);
        s.is_device_open = is_device_open_.load(std::memory_order_relaxed);
        return s;
    }

private:
    struct Chunk {
        std::vector<int16_t> samples;
        size_t num_frames = 0;
    };

    static std::string default_device() {
        const char* env = std::getenv("MAIX_WEBRTC_BT_DEVICE");
        if (env && env[0] != '\0') {
            return std::string(env);
        }
        return "bluealsa:DEV=6C:16:29:7B:00:D4,PROFILE=sco";
    }

    bool open_device() {
        if (pcm_handle_) return true;

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_open_error_).count() < 500) {
            return false;
        }

        int err = snd_pcm_open(&pcm_handle_, device_name_.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
        if (err < 0) {
            last_open_error_ = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(last_open_error_ - last_log_error_).count() >= 3) {
                std::fprintf(stderr, "[G4c4 BT Playback] snd_pcm_open failed (%s): %s\n",
                             device_name_.c_str(), snd_strerror(err));
                last_log_error_ = last_open_error_;
            }
            pcm_handle_ = nullptr;
            is_device_open_.store(false, std::memory_order_relaxed);
            return false;
        }

        // BlueALSA needs time to negotiate D-Bus state
        for (int retry = 0; retry < 6; ++retry) {
            if (native_sco_) {
                snd_pcm_hw_params_t* hw = nullptr;
                snd_pcm_hw_params_alloca(&hw);
                unsigned int rate = sample_rate_;
                unsigned int period_us = 20000;
                unsigned int buffer_us = 80000;
                if ((err = snd_pcm_hw_params_any(pcm_handle_, hw)) >= 0 &&
                    (err = snd_pcm_hw_params_set_rate_resample(pcm_handle_, hw, 1)) >= 0 &&
                    (err = snd_pcm_hw_params_set_access(pcm_handle_, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) >= 0 &&
                    (err = snd_pcm_hw_params_set_format(pcm_handle_, hw, SND_PCM_FORMAT_S16_LE)) >= 0 &&
                    (err = snd_pcm_hw_params_set_channels(pcm_handle_, hw, channels_)) >= 0 &&
                    (err = snd_pcm_hw_params_set_rate_near(pcm_handle_, hw, &rate, nullptr)) >= 0 &&
                    (err = snd_pcm_hw_params_set_period_time_near(pcm_handle_, hw, &period_us, nullptr)) >= 0 &&
                    (err = snd_pcm_hw_params_set_buffer_time_near(pcm_handle_, hw, &buffer_us, nullptr)) >= 0) {
                    err = snd_pcm_hw_params(pcm_handle_, hw);
                }
                if (err < 0 && err != -EBUSY && err != -EAGAIN) {
                    err = snd_pcm_set_params(pcm_handle_, SND_PCM_FORMAT_S16_LE,
                                             SND_PCM_ACCESS_RW_INTERLEAVED, channels_,
                                             sample_rate_, 1, 80000);
                }
            } else {
                err = snd_pcm_set_params(pcm_handle_,
                                         SND_PCM_FORMAT_S16_LE,
                                         SND_PCM_ACCESS_RW_INTERLEAVED,
                                         channels_,
                                         sample_rate_,
                                         1,      // soft_resample allowed
                                         120000); // 120ms latency buffer
            }
            if (err == -EBUSY || err == -EAGAIN) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            break;
        }
        if (err < 0) {
            last_open_error_ = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(last_open_error_ - last_log_error_).count() >= 3) {
                std::fprintf(stderr, "[G4c4 BT Playback] snd_pcm_set_params failed: %s\n", snd_strerror(err));
                last_log_error_ = last_open_error_;
            }
            snd_pcm_close(pcm_handle_);
            pcm_handle_ = nullptr;
            is_device_open_.store(false, std::memory_order_relaxed);
            return false;
        }

        device_open_count_.fetch_add(1, std::memory_order_relaxed);
        is_device_open_.store(true, std::memory_order_relaxed);

        std::fprintf(stderr, "[G4c4 BT Playback] ALSA playback opened (blocking): %s (%u Hz / %u channels S16_LE)\n",
                     device_name_.c_str(), sample_rate_, channels_);
        return true;
    }

    void close_device() {
        if (pcm_handle_) {
            device_close_count_.fetch_add(1, std::memory_order_relaxed);
            is_device_open_.store(false, std::memory_order_relaxed);
            snd_pcm_drop(pcm_handle_);
            snd_pcm_close(pcm_handle_);
            pcm_handle_ = nullptr;
        }
    }

    void worker_loop() {
        g4c4_perf::set_thread_name("bt_playback");
        auto& pperf = g4c4_perf::stats().playback;
        uint64_t cpu_prev = g4c4_perf::thread_cpu_ns();
        while (!stopping_) {
            uint64_t cpu_now = g4c4_perf::thread_cpu_ns();
            pperf.thread_cpu_ns.fetch_add(cpu_now - cpu_prev, std::memory_order_relaxed);
            cpu_prev = cpu_now;
            Chunk chunk;
            bool do_drop = false;
            bool from_queue = false;

            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (!talk_active_ && !pcm_handle_ && !need_pcm_drop_) {
                    cv_.wait(lock, [this] {
                        return stopping_ || talk_active_ || need_pcm_drop_ || !queue_.empty();
                    });
                } else {
                    cv_.wait_for(lock, std::chrono::milliseconds(20), [this] {
                        return stopping_ || !queue_.empty() || need_pcm_drop_;
                    });
                }

                if (stopping_) break;

                if (need_pcm_drop_) {
                    do_drop = true;
                    need_pcm_drop_ = false;
                    is_prebuffering_ = true;
                }

                if (do_drop) {
                    // Close idle/stopped SCO playback handle outside mutex so BlueALSA
                    // releases its playback reference when Talk is OFF or stream is idle.
                } else if (is_prebuffering_ && queue_.size() < 2 && talk_active_) {
                    continue;
                } else {
                    is_prebuffering_ = false;
                }

                if (!do_drop && (queue_.empty() || !talk_active_)) {
                    if (native_sco_ && pcm_handle_ && talk_active_ && !is_prebuffering_ &&
                        last_enqueue_.time_since_epoch().count() > 0 &&
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - last_enqueue_).count() <= 500) {
                        // Active SCO Talk session packet gap: feed 20ms silence (160 samples)
                        // to keep the 80ms buffer clocked without underrunning.
                        chunk.samples.assign(sample_rate_ / 50 * channels_, 0);
                        chunk.num_frames = sample_rate_ / 50;
                    } else {
                        continue;
                    }
                } else if (!queue_.empty() && talk_active_) {
                    // Non-destructive peek: do NOT discard chunk if device open is delayed
                    chunk = queue_.front();
                    from_queue = true;
                }
            }

            if (do_drop && pcm_handle_) {
                talk_off_pcm_drops_.fetch_add(1, std::memory_order_relaxed);
                close_device();
            }

            if (chunk.num_frames == 0) {
                continue;
            }

            if (!open_device()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            // Successfully opened/verified device: now safely pop real chunk from queue
            if (from_queue) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!queue_.empty()) {
                    queue_.pop_front();
                    current_queue_depth_.store(queue_.size(), std::memory_order_relaxed);
                }
            }

            // Write frames to ALSA device, handling blocking writes and errors
            const int16_t* ptr = chunk.samples.data();
            size_t remaining = chunk.num_frames;
            int eagain_retries = 0;

            while (remaining > 0 && !stopping_) {
                auto t0 = std::chrono::steady_clock::now();
                snd_pcm_sframes_t written = snd_pcm_writei(pcm_handle_, ptr, remaining);
                auto t1 = std::chrono::steady_clock::now();
                const int64_t wr_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
                last_write_us_.store(wr_us, std::memory_order_relaxed);
                pperf.write_calls.fetch_add(1, std::memory_order_relaxed);
                pperf.alsa_write_ns.fetch_add(static_cast<uint64_t>(wr_us) * 1000ULL, std::memory_order_relaxed);

                if (written > 0) {
                    if (static_cast<size_t>(written) < remaining) {
                        pperf.partial_count.fetch_add(1, std::memory_order_relaxed);
                    }
                    frames_written_.fetch_add(written, std::memory_order_relaxed);
                    ptr += (written * channels_);
                    remaining -= written;
                    eagain_retries = 0;
                } else if (written == -EAGAIN || written == -EINTR) {
                    pperf.eagain_count.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    if (++eagain_retries > 30) {
                        dropped_overflow_.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                    continue;
                } else if (written == -EPIPE) {
                    // Underrun: recover and re-prepare
                    underruns_.fetch_add(1, std::memory_order_relaxed);
                    int r = snd_pcm_prepare(pcm_handle_);
                    if (r < 0) {
                        alsa_errors_.fetch_add(1, std::memory_order_relaxed);
                        close_device();
                        break;
                    }
                    is_prebuffering_ = true;
                    eagain_retries = 0;
                } else if (written == -ESTRPIPE) {
                    int r = snd_pcm_resume(pcm_handle_);
                    if (r < 0) r = snd_pcm_prepare(pcm_handle_);
                    if (r < 0) {
                        alsa_errors_.fetch_add(1, std::memory_order_relaxed);
                        close_device();
                        break;
                    }
                    eagain_retries = 0;
                } else if (written == 0) {
                    pperf.zero_count.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                } else {
                    int rec = snd_pcm_recover(pcm_handle_, written, 1);
                    if (rec >= 0) {
                        is_prebuffering_ = true;
                        eagain_retries = 0;
                        continue;
                    }
                    alsa_errors_.fetch_add(1, std::memory_order_relaxed);
                    close_device();
                    break;
                }
            }
        }

        close_device();
    }

    std::string device_name_;
    const bool native_sco_;
    const unsigned sample_rate_;
    const unsigned channels_;
    snd_pcm_t* pcm_handle_ = nullptr;
    std::chrono::steady_clock::time_point last_open_error_{};
    std::chrono::steady_clock::time_point last_log_error_{};
    std::chrono::steady_clock::time_point last_enqueue_{};
    std::chrono::steady_clock::time_point last_periodic_flush_{};

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Chunk> queue_;
    bool talk_active_ = true; // Default ON; can be gated by Talk toggle
    bool need_pcm_drop_ = false;
    bool is_prebuffering_ = true;
    std::atomic<bool> stopping_{false};

    // Monotonic metrics tracking (PLAN-07)
    std::atomic<uint64_t> frames_written_{0};
    std::atomic<uint64_t> dropped_overflow_{0};
    std::atomic<uint64_t> dropped_busy_{0};
    std::atomic<uint64_t> alsa_errors_{0};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint64_t> talk_on_events_{0};
    std::atomic<uint64_t> talk_off_events_{0};
    std::atomic<uint64_t> talk_off_dropped_chunks_{0};
    std::atomic<uint64_t> talk_off_pcm_drops_{0};
    std::atomic<size_t> current_queue_depth_{0};
    std::atomic<size_t> peak_queue_depth_{0};
    std::atomic<uint64_t> device_open_count_{0};
    std::atomic<uint64_t> device_close_count_{0};
    std::atomic<int64_t> last_write_us_{0};
    std::atomic<bool> is_device_open_{false};

    std::thread worker_;
};
