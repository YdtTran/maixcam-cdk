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
    static constexpr unsigned int SAMPLE_RATE = 48000;
    static constexpr unsigned int CHANNELS = 2;

    struct Stats {
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
        }
        cv_.notify_one();
    }

    bool is_talk_active() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return talk_active_;
    }

    /**
     * @brief Enqueue 48 kHz stereo interleaved S16_LE PCM frames for playback.
     *
     * @param pcm Interleaved S16 samples (num_frames * 2 elements).
     * @param num_frames Number of stereo frames (pairs of samples).
     */
    void enqueue_stereo_pcm(const int16_t* pcm, size_t num_frames) {
        if (!pcm || num_frames == 0) return;

        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || !talk_active_) {
            return;
        }

        // Deterministic overflow policy: drop oldest frame to prevent latency buildup
        if (queue_.size() >= MAX_QUEUE_FRAMES) {
            queue_.pop_front();
            dropped_overflow_.fetch_add(1, std::memory_order_relaxed);
        }

        Chunk chunk;
        chunk.samples.assign(pcm, pcm + (num_frames * CHANNELS));
        chunk.num_frames = num_frames;

        queue_.push_back(std::move(chunk));
        const size_t qsize = queue_.size();
        current_queue_depth_.store(qsize, std::memory_order_relaxed);

        size_t cur_peak = peak_queue_depth_.load(std::memory_order_relaxed);
        while (qsize > cur_peak && !peak_queue_depth_.compare_exchange_weak(cur_peak, qsize, std::memory_order_relaxed)) {}

        last_enqueue_ = std::chrono::steady_clock::now();
        cv_.notify_one();
    }

    /**
     * @brief Periodic idle notification (called when no Opus packets arrive).
     */
    void notify_stream_idle() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!queue_.empty()) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_enqueue_).count() > 300) {
                talk_off_dropped_chunks_.fetch_add(queue_.size(), std::memory_order_relaxed);
                queue_.clear();
                current_queue_depth_.store(0, std::memory_order_relaxed);
                need_pcm_drop_ = true;
                cv_.notify_one();
            }
        }
    }

    Stats get_stats() const {
        Stats s;
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
        return "bluealsa:DEV=6C:16:29:7B:00:D4,PROFILE=a2dp";
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
            err = snd_pcm_set_params(pcm_handle_,
                                     SND_PCM_FORMAT_S16_LE,
                                     SND_PCM_ACCESS_RW_INTERLEAVED,
                                     CHANNELS,
                                     SAMPLE_RATE,
                                     1,      // soft_resample allowed
                                     120000); // 120ms latency buffer (resilient to BT/Wi-Fi burst jitter)
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

        std::fprintf(stderr, "[G4c4 BT Playback] ALSA playback opened (blocking): %s (48 kHz stereo S16_LE)\n",
                     device_name_.c_str());
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
        // Pre-warm BlueALSA device on worker launch so initial stream has zero setup latency
        open_device();

        while (!stopping_) {
            Chunk chunk;
            bool do_drop = false;

            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
                    return stopping_ || !queue_.empty() || need_pcm_drop_;
                });

                if (stopping_) break;

                if (need_pcm_drop_) {
                    do_drop = true;
                    need_pcm_drop_ = false;
                    is_prebuffering_ = true;
                }

                if (do_drop) {
                    // PCM drop will be performed below outside mutex
                } else if (is_prebuffering_ && queue_.size() < 2 && talk_active_) {
                    continue;
                } else {
                    is_prebuffering_ = false;
                }

                if (!do_drop && (queue_.empty() || !talk_active_)) {
                    // While idle, ensure device is pre-warmed if not already open
                    if (!pcm_handle_ && talk_active_) {
                        lock.unlock();
                        open_device();
                    }
                    continue;
                }

                if (!queue_.empty() && talk_active_) {
                    // Non-destructive peek: do NOT discard chunk if device open is delayed
                    chunk = queue_.front();
                }
            }

            if (do_drop && pcm_handle_) {
                talk_off_pcm_drops_.fetch_add(1, std::memory_order_relaxed);
                snd_pcm_drop(pcm_handle_);
                snd_pcm_prepare(pcm_handle_);
            }

            if (chunk.num_frames == 0) {
                continue;
            }

            if (!open_device()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            // Successfully opened/verified device: now safely pop chunk from queue
            {
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
                last_write_us_.store(
                    std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count(),
                    std::memory_order_relaxed
                );

                if (written > 0) {
                    frames_written_.fetch_add(written, std::memory_order_relaxed);
                    ptr += (written * CHANNELS);
                    remaining -= written;
                    eagain_retries = 0;
                } else if (written == -EAGAIN || written == -EINTR) {
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
    snd_pcm_t* pcm_handle_ = nullptr;
    std::chrono::steady_clock::time_point last_open_error_{};
    std::chrono::steady_clock::time_point last_log_error_{};
    std::chrono::steady_clock::time_point last_enqueue_{};

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
