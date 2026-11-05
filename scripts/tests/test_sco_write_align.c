#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

static int sco_fd = -1;
static unsigned short voice = 0x0060;

/* Emulate SCO metadata while using a real packet socket for byte verification. */
int getsockopt(int fd, int level, int option, void *value, socklen_t *length) {
    if (fd != sco_fd) return syscall(SYS_getsockopt, fd, level, option, value, length);
    if (level == SOL_SOCKET && (option == SO_DOMAIN || option == SO_PROTOCOL)) {
        *(int *)value = option == SO_DOMAIN ? AF_BLUETOOTH : 2;
        *length = sizeof(int);
        return 0;
    }
    if (level == 274 && option == 11) {
        *(unsigned short *)value = voice;
        *length = sizeof(voice);
        return 0;
    }
    errno = ENOPROTOOPT;
    return -1;
}

int main(void) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
    sco_fd = pair[0];
    unsigned char source[1024], received[1024];
    for (size_t i = 0; i < sizeof(source); ++i) source[i] = (i * 37 + i / 256) & 255;
    size_t total = 0;
    // Actual BlueALSA loop: MTU=255, PCM pointer advances 127 S16 samples.
    for (size_t offset = 0; offset + 255 <= sizeof(source); offset += 254) {
        ssize_t written = write(sco_fd, source + offset, 255);
        ssize_t length = recv(pair[1], received + total, sizeof(received) - total, 0);
        if (written != 254 || length != 254) {
            fprintf(stderr, "FAIL: sent %zd bytes while consuming 254 PCM bytes\n", written);
            return 1;
        }
        total += length;
    }
    assert(memcmp(source, received, total) == 0);
    assert(write(sco_fd, source, 120) == 120);
    assert(recv(pair[1], received, sizeof(received), 0) == 120);
    // Transparent codec frames must remain unchanged.
    voice = 0x0003;
    assert(write(sco_fd, source, 255) == 255);
    assert(recv(pair[1], received, sizeof(received), 0) == 255);
    // Non-Bluetooth writes must remain unchanged, including odd lengths.
    sco_fd = -1;
    assert(write(pair[0], source, 255) == 255);
    assert(recv(pair[1], received, sizeof(received), 0) == 255);
    close(pair[0]);
    close(pair[1]);
    puts("CVSD sample alignment and other socket writes: PASS");
}
