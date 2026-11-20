#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "sco_chunk.h"

/* Temporary preload, placed before the existing alignment shim. */
static pthread_once_t once = PTHREAD_ONCE_INIT;
static ssize_t (*next_write)(int, const void *, size_t);
static ssize_t (*next_read)(int, void *, size_t);
static _Atomic size_t observed_read_size;

static void resolve(void) {
    next_write = (ssize_t (*)(int, const void *, size_t))dlsym(RTLD_NEXT, "write");
    next_read = (ssize_t (*)(int, void *, size_t))dlsym(RTLD_NEXT, "read");
}

static int is_cvsd_sco(int fd) {
    int domain, protocol;
    unsigned short voice;
    socklen_t len = sizeof(domain);
    if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &len) || domain != AF_BLUETOOTH)
        return 0;
    len = sizeof(protocol);
    if (getsockopt(fd, SOL_SOCKET, SO_PROTOCOL, &protocol, &len) || protocol != 2)
        return 0;
    len = sizeof(voice);
    return getsockopt(fd, 274, 11, &voice, &len) == 0 && voice == 0x0060;
}

ssize_t read(int fd, void *buffer, size_t count) {
    pthread_once(&once, resolve);
    if (!next_read) { errno = ENOSYS; return -1; }
    ssize_t result = next_read(fd, buffer, count);
    int saved_errno = errno;
    if (result > 0 && is_cvsd_sco(fd)) atomic_store(&observed_read_size, (size_t)result);
    errno = saved_errno;
    return result;
}

struct state {
    int fd;
    size_t elapsed_bytes;
    struct timespec start;
};

static ssize_t send_part(void *opaque, const void *data, size_t count) {
    struct state *state = opaque;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    ssize_t result = next_write(state->fd, data, count);
    int saved_errno = errno;
    const char *log = getenv("SCO_DIAG_LOG");
    if (log && *log) {
        int fd = open(log, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
        if (fd >= 0) {
            char line[160];
            int n = snprintf(line, sizeof(line), "%lld.%09ld fd=%d request=%zu result=%zd errno=%d\n",
                             (long long)now.tv_sec, now.tv_nsec, state->fd, count,
                             result, result < 0 ? saved_errno : 0);
            if (n > 0 && (size_t)n < sizeof(line)) syscall(SYS_write, fd, line, (size_t)n);
            close(fd);
        }
    }
    errno = saved_errno;
    return result;
}

static int pace(void *opaque, size_t bytes) {
    struct state *state = opaque;
    state->elapsed_bytes += bytes;
    long long ns = (long long)state->start.tv_nsec + (long long)state->elapsed_bytes * 62500;
    struct timespec until = { state->start.tv_sec + ns / 1000000000, ns % 1000000000 };
    int error;
    do { error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL); }
    while (error == EINTR);
    if (error) { errno = error; return -1; }
    return 0;
}

ssize_t write(int fd, const void *buffer, size_t count) {
    pthread_once(&once, resolve);
    if (!next_write) { errno = ENOSYS; return -1; }
    const char *setting = getenv("SCO_DIAG_CHUNK");
    if (!setting || !*setting || count < 2) return next_write(fd, buffer, count);
    char *end;
    unsigned long chunk = strtoul(setting, &end, 10);
    if (*end || chunk < 2 || chunk > 254 || (chunk & 1)) {
        errno = EINVAL;
        return -1;
    }
    if (!is_cvsd_sco(fd)) return next_write(fd, buffer, count);
    /* Fail closed if this connection has not produced the measured read size. */
    for (int i = 0; i < 200 && atomic_load(&observed_read_size) == 0; ++i) {
        struct timespec wait = { 0, 10000000 };
        nanosleep(&wait, NULL);
    }
    if (atomic_load(&observed_read_size) != chunk) { errno = EIO; return -1; }
    struct state state = { .fd = fd };
    if (clock_gettime(CLOCK_MONOTONIC, &state.start)) return -1;
    return sco_chunk_write(&state, buffer, count, chunk, send_part, pace);
}
