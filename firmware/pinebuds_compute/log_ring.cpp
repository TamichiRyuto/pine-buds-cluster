// SPP log ring buffer (design §13.3.2).
//
// Positions (free_pos/write_pos, and the base handed out by peek) are
// absolute unsigned byte counters that only ever grow; used = write_pos -
// free_pos <= LOG_RING_CAPACITY holds at all times, which keeps plain
// unsigned-difference math safe even across a 2^32 wrap. Physical index
// into buf is pos & (LOG_RING_CAPACITY - 1).

#include "log_ring.h"

#include <stdio.h>

static unsigned log_ring_phys(unsigned pos) {
    return pos & (unsigned)(LOG_RING_CAPACITY - 1);
}

void log_ring_init(struct log_ring *r) {
    r->free_pos = 0;
    r->write_pos = 0;
    r->dropped = 0;
}

int log_ring_push(struct log_ring *r, const char *line, unsigned len) {
    unsigned total = len + 1; /* line bytes + '\n' */
    unsigned i;

    if (total > (unsigned)LOG_RING_CAPACITY) {
        r->dropped++;
        return 0;
    }

    while ((r->write_pos - r->free_pos) + total > (unsigned)LOG_RING_CAPACITY) {
        unsigned used = r->write_pos - r->free_pos;
        unsigned advance = used; /* fallback; overwritten once '\n' is found */
        for (i = 0; i < used; ++i) {
            if (r->buf[log_ring_phys(r->free_pos + i)] == '\n') {
                advance = i + 1;
                break;
            }
        }
        r->free_pos += advance;
        r->dropped++;
    }

    for (i = 0; i < len; ++i) {
        r->buf[log_ring_phys(r->write_pos + i)] = line[i];
    }
    r->buf[log_ring_phys(r->write_pos + len)] = '\n';
    r->write_pos += total;
    return 1;
}

unsigned log_ring_peek(const struct log_ring *r, char *dst, unsigned max,
                       unsigned *base_out) {
    unsigned used = r->write_pos - r->free_pos;
    unsigned n = (used < max) ? used : max;
    unsigned i;

    for (i = 0; i < n; ++i) {
        dst[i] = r->buf[log_ring_phys(r->free_pos + i)];
    }
    *base_out = r->free_pos;
    return n;
}

void log_ring_commit(struct log_ring *r, unsigned base, unsigned n) {
    unsigned target = base + n;
    int ahead = (int)(target - r->free_pos) > 0;
    int not_beyond_write = (int)(r->write_pos - target) >= 0;

    if (ahead && not_beyond_write) {
        r->free_pos = target;
    }
}

unsigned log_ring_used(const struct log_ring *r) {
    return r->write_pos - r->free_pos;
}

unsigned log_ring_take_dropped(struct log_ring *r) {
    unsigned d = r->dropped;
    r->dropped = 0;
    return d;
}

unsigned log_ring_next_chunk(struct log_ring *r, unsigned dropped,
                             unsigned contended, char *dst, unsigned max,
                             unsigned *base_out, int *consumes_ring) {
    if (dropped != 0) {
        int written;

        *consumes_ring = 0;
        if (max == 0) {
            return 0;
        }
        written = snprintf(dst, max, "#- [log] dropped=%u contended=%u\n",
                            dropped, contended);
        if (written < 0) {
            return 0;
        }
        return ((unsigned)written < max) ? (unsigned)written : max - 1;
    }

    {
        unsigned n = log_ring_peek(r, dst, max, base_out);
        *consumes_ring = (n > 0) ? 1 : 0;
        return n;
    }
}
