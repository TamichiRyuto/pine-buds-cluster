// Byte ring buffer for the SPP log channel (design §13.3.2).
//
// Target-independent pure logic: no SDK headers, no heap, gnu++98. The
// producers (compute thread, BesbtThread tap) push whole lines; the single
// consumer (SPP log thread) peeks bytes without consuming and commits them
// only after the stack reports DATA_SENT. Positions are absolute byte
// counters so a drop-oldest overtaking an in-flight peek stays consistent:
// commit only ever moves free_pos forward.

#ifndef PINEBUDS_COMPUTE_LOG_RING_H
#define PINEBUDS_COMPUTE_LOG_RING_H

enum { LOG_RING_CAPACITY = 4096 }; /* power of two (& (CAP-1) indexing) */

struct log_ring {
    char buf[LOG_RING_CAPACITY];
    unsigned free_pos;  /* absolute; everything before it is discarded */
    unsigned write_pos; /* absolute; next byte goes here */
    unsigned dropped;   /* lines discarded so far (cumulative) */
};

void log_ring_init(struct log_ring *r);

/* Append one line; the ring adds the trailing '\n' itself. If space is
   short, whole oldest lines are discarded until it fits (dropped++ per
   line). Returns 1 on success. A line with len+1 > LOG_RING_CAPACITY is
   rejected: returns 0, nothing is discarded, dropped++. */
int log_ring_push(struct log_ring *r, const char *line, unsigned len);

/* Copy up to max unsent bytes into dst without consuming them; *base_out
   receives the absolute position of dst[0]. May split mid-line (SPP is a
   byte stream; the receiver recovers line boundaries from '\n'). */
unsigned log_ring_peek(const struct log_ring *r, char *dst, unsigned max,
                       unsigned *base_out);

/* Acknowledge that n bytes starting at base were sent:
   free_pos = max(free_pos, base + n) (unsigned-difference compare).
   A stale base (already overtaken by drop-oldest) is a no-op. */
void log_ring_commit(struct log_ring *r, unsigned base, unsigned n);

unsigned log_ring_used(const struct log_ring *r);
unsigned log_ring_take_dropped(struct log_ring *r); /* read and clear */

/* Pure chunk builder for the TX thread (design §13.12): with dropped != 0
   the chunk is only the marker line "#- [log] dropped=<d> contended=<c>\n"
   and *consumes_ring is 0; otherwise it peeks the ring (*consumes_ring is
   1 when bytes were taken, *base_out as per log_ring_peek). Returns the
   chunk length, 0 when there is nothing to send. */
unsigned log_ring_next_chunk(struct log_ring *r, unsigned dropped,
                             unsigned contended, char *dst, unsigned max,
                             unsigned *base_out, int *consumes_ring);

#endif /* PINEBUDS_COMPUTE_LOG_RING_H */
