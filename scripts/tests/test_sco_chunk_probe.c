#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

static int sco_fd;

int getsockopt(int fd, int level, int option, void *value, socklen_t *length) {
    if (fd != sco_fd) return syscall(SYS_getsockopt, fd, level, option, value, length);
    if (level == SOL_SOCKET && (option == SO_DOMAIN || option == SO_PROTOCOL)) {
        *(int *)value = option == SO_DOMAIN ? AF_BLUETOOTH : 2;
        *length = sizeof(int);
        return 0;
    }
    if (level == 274 && option == 11) {
        *(unsigned short *)value = 0x0060;
        *length = sizeof(unsigned short);
        return 0;
    }
    errno = ENOPROTOOPT;
    return -1;
}

int main(void) {
    int pair[2];
    unsigned char input[255], output[254];
    for (size_t i = 0; i < sizeof(input); ++i) input[i] = (unsigned char)i;
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
    sco_fd = pair[0];
    /* A same-session incoming SCO read must establish the probe size. */
    assert(write(sco_fd, input, sizeof(input)) == -1 && errno == EIO);
    assert(write(pair[1], input, 120) == 120);
    assert(read(sco_fd, output, sizeof(output)) == 120);
    assert(write(sco_fd, input, sizeof(input)) == 254);
    assert(recv(pair[1], output, 120, 0) == 120);
    assert(recv(pair[1], output + 120, 120, 0) == 120);
    assert(recv(pair[1], output + 240, 14, 0) == 14);
    assert(memcmp(input, output, sizeof(output)) == 0);
    sco_fd = -1;
    assert(write(pair[0], input, sizeof(input)) == 255);
    assert(recv(pair[1], input, sizeof(input), 0) == 255);
    close(pair[0]); close(pair[1]);
    return 0;
}
