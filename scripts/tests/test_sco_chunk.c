#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>

#include "../sco_chunk.h"

static unsigned char output[256];
static size_t used;
static size_t calls;
static int fail_after;

static ssize_t send_part(void *context, const void *data, size_t count) {
    (void)context;
    if (fail_after && calls == (size_t)fail_after) {
        errno = EIO;
        return -1;
    }
    ++calls;
    if (calls == 1 && count > 4) count -= 4;
    memcpy(output + used, data, count);
    used += count;
    return count;
}

static int pace(void *context, size_t bytes) {
    (void)context;
    return bytes % 2 ? -1 : 0;
}

int main(void) {
    unsigned char input[255];
    for (size_t i = 0; i < sizeof(input); ++i) input[i] = (unsigned char)i;
    assert(sco_chunk_write(NULL, input, sizeof(input), 120, send_part, pace) == 254);
    assert(used == 254 && memcmp(input, output, used) == 0);
    used = calls = 0;
    fail_after = 1;
    assert(sco_chunk_write(NULL, input, sizeof(input), 120, send_part, pace) == -1);
    assert(errno == EIO && used == 116);
    assert(sco_chunk_write(NULL, input, sizeof(input), 119, send_part, pace) == -1);
    assert(errno == EINVAL);
    return 0;
}
