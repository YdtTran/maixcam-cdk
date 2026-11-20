#pragma once

#include <errno.h>
#include <stddef.h>
#include <sys/types.h>

/* Diagnostic only: BlueALSA consumes floor(requested / 2) S16 samples. */
static ssize_t sco_chunk_write(void *context, const void *buffer, size_t requested,
                               size_t chunk, ssize_t (*send_part)(void *, const void *, size_t),
                               int (*pace)(void *, size_t)) {
    size_t length = requested & ~(size_t)1;
    size_t offset = 0;
    if (length == 0 || chunk < 2 || (chunk & 1)) {
        errno = EINVAL;
        return -1;
    }
    while (offset < length) {
        size_t part = length - offset < chunk ? length - offset : chunk;
        ssize_t written = send_part(context, (const char *)buffer + offset, part);
        if (written < 0 && errno == EINTR) continue;
        if (written < 0) {
            /* BlueALSA retries EAGAIN for the whole buffer; never replay a prefix. */
            if (offset && errno == EAGAIN) errno = EIO;
            return -1;
        }
        if (written == 0 || (written & 1) || (size_t)written > part) {
            errno = EIO;
            return -1;
        }
        offset += (size_t)written;
        if (offset < length && pace(context, (size_t)written) < 0) return -1;
    }
    return (ssize_t)offset;
}
