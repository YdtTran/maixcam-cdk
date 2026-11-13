#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <pthread.h>
#include <time.h>

namespace g4c4_perf {

inline uint64_t mono_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

inline uint64_t thread_cpu_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

inline void set_thread_name(const char* name) {
    pthread_setname_np(pthread_self(), name);
}

inline bool env_flag(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '1' && v[1] == '\0';
}

inline int env_int(const char* name, int def) {
    const char* v = std::getenv(name);
    if (!v || !*v) return def;
    return std::atoi(v);
}

struct VideoStats {
    std::atomic<uint64_t> loop_iters{0};
    std::atomic<uint64_t> vi_pops{0};
    std::atomic<uint64_t> vi_empty{0};
    std::atomic<uint64_t> venc_pops{0};
    std::atomic<uint64_t> venc_empty{0};
    std::atomic<uint64_t> frames_sent{0};
    std::atomic<uint64_t> vi_pop_ns{0};
    std::atomic<uint64_t> venc_pop_ns{0};
    std::atomic<uint64_t> copy_ns{0};
    std::atomic<uint64_t> webrtc_push_ns{0};
    std::atomic<uint64_t> venc_push_ns{0};
    std::atomic<uint64_t> thread_cpu_ns{0};
};

struct MicStats {
    std::atomic<uint64_t> read_calls{0};
    std::atomic<uint64_t> empty_reads{0};
    std::atomic<uint64_t> frames_encoded{0};
    std::atomic<uint64_t> read_ns{0};
    std::atomic<uint64_t> opus_encode_ns{0};
    std::atomic<uint64_t> webrtc_push_ns{0};
    std::atomic<uint64_t> thread_cpu_ns{0};
};

struct RxPerfStats {
    std::atomic<uint64_t> packets_decoded{0};
    std::atomic<uint64_t> opus_decode_ns{0};
    std::atomic<uint64_t> thread_cpu_ns{0};
};

struct PlaybackPerfStats {
    std::atomic<uint64_t> write_calls{0};
    std::atomic<uint64_t> eagain_count{0};
    std::atomic<uint64_t> partial_count{0};
    std::atomic<uint64_t> zero_count{0};
    std::atomic<uint64_t> alsa_write_ns{0};
    std::atomic<uint64_t> thread_cpu_ns{0};
};

struct MutexStats {
    std::atomic<uint64_t> media_locks{0};
    std::atomic<uint64_t> media_wait_ns{0};
    std::atomic<uint64_t> media_hold_ns{0};
};

struct Registry {
    VideoStats video;
    MicStats mic;
    RxPerfStats rx;
    PlaybackPerfStats playback;
    MutexStats mutex;
};

inline Registry& stats() {
    static Registry r;
    return r;
}

template <typename Mutex>
class TimedLockGuard {
public:
    explicit TimedLockGuard(Mutex& m) : m_(m) {
        const uint64_t t0 = mono_ns();
        m_.lock();
        acquired_ns_ = mono_ns();
        auto& ms = stats().mutex;
        ms.media_locks.fetch_add(1, std::memory_order_relaxed);
        ms.media_wait_ns.fetch_add(acquired_ns_ - t0, std::memory_order_relaxed);
    }
    ~TimedLockGuard() {
        const uint64_t t1 = mono_ns();
        stats().mutex.media_hold_ns.fetch_add(t1 - acquired_ns_, std::memory_order_relaxed);
        m_.unlock();
    }
    TimedLockGuard(const TimedLockGuard&) = delete;
    TimedLockGuard& operator=(const TimedLockGuard&) = delete;
private:
    Mutex& m_;
    uint64_t acquired_ns_;
};

} // namespace g4c4_perf
