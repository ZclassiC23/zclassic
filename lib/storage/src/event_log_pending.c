/* event_log_pending — bounded deferred-record assembly implementation. */
#include "storage/event_log_pending.h"

#include "util/safe_alloc.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#define PENDING_TARGET (8u * 1024u * 1024u)

static bool write_all(int fd, const uint8_t *data, size_t len, off_t offset)
{
    while (len > 0) {
        ssize_t n = pwrite(fd, data, len, offset);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        data += (size_t)n;
        offset += n;
        len -= (size_t)n;
    }
    return true;
}

bool event_log_pending_write(struct event_log_pending *p, int fd,
                             uint64_t *end_offset)
{
    if (!p || !end_offset || p->len == 0)
        return true;
    if (!write_all(fd, p->data, p->len, (off_t)*end_offset))
        return false;
    *end_offset += (uint64_t)p->len;
    p->len = 0;
    return true;
}

bool event_log_pending_prepare(struct event_log_pending *p, size_t need,
                               int fd, uint64_t *end_offset)
{
    if (!p || !end_offset || need > SIZE_MAX - p->len)
        return false;
    size_t required = p->len + need;
    if (p->len > 0 && required > PENDING_TARGET) {
        if (!event_log_pending_write(p, fd, end_offset))
            return false;
        required = need;
    }
    if (required <= p->cap)
        return true;
    size_t cap = p->cap ? p->cap : 4096u;
    while (cap < required) {
        if (cap > SIZE_MAX / 2) {
            cap = required;
            break;
        }
        cap *= 2;
    }
    uint8_t *grown = zcl_realloc(p->data, cap, "event_log/pending");
    if (!grown)
        return false;
    p->data = grown;
    p->cap = cap;
    return true;
}

void event_log_pending_destroy(struct event_log_pending *p)
{
    if (!p)
        return;
    free(p->data);
    *p = (struct event_log_pending){0};
}
