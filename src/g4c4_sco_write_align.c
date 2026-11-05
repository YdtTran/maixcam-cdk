#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <sys/socket.h>
#include <unistd.h>

/* Linux Bluetooth ABI, avoiding a build dependency on libbluetooth headers. */
#define G4C4_BTPROTO_SCO 2
#define G4C4_SOL_BLUETOOTH 274
#define G4C4_BT_VOICE 11
#define G4C4_BT_VOICE_CVSD_16BIT 0x0060

static pthread_once_t resolve_once = PTHREAD_ONCE_INIT;
static ssize_t (*next_write)(int, const void *, size_t);

static void resolve_write(void) {
    next_write = (ssize_t (*)(int, const void *, size_t))dlsym(RTLD_NEXT, "write");
}

/* BlueALSA 4.1.1 CVSD advances its PCM pointer by floor(MTU / 2) samples.
 * An odd MTU therefore sends one byte twice. Return a genuine short write of
 * whole S16 samples, matching that advancement. This shim is loaded only into
 * BlueALSA; capture reads, controller configuration and other writes are intact.
 */
ssize_t write(int fd, const void *buffer, size_t count) {
    pthread_once(&resolve_once, resolve_write);
    if (!next_write) {
        errno = ENOSYS;
        return -1;
    }
    const int saved_errno = errno;
    if (count > 1 && (count & 1)) {
        int domain = 0, protocol = 0;
        unsigned short voice = 0;
        socklen_t size = sizeof(domain);
        if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &size) == 0 &&
            domain == AF_BLUETOOTH) {
            size = sizeof(protocol);
            if (getsockopt(fd, SOL_SOCKET, SO_PROTOCOL, &protocol, &size) == 0 &&
                protocol == G4C4_BTPROTO_SCO) {
                size = sizeof(voice);
                if (getsockopt(fd, G4C4_SOL_BLUETOOTH, G4C4_BT_VOICE, &voice, &size) == 0 &&
                    voice == G4C4_BT_VOICE_CVSD_16BIT)
                    --count;
            }
        }
    }
    errno = saved_errno;
    return next_write(fd, buffer, count);
}
