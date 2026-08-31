// Host-side TDD tests for the SPP log ring (firmware/pinebuds_compute/
// log_ring). See docs/design-ibrt-transport.md §13.3.2/§13.12.
//
// The ring is the seam between the log producers (compute thread and
// BesbtThread tap, push only) and the single SPP TX thread (peek/commit).
// Absolute byte positions make an in-flight peek survive drop-oldest:
// commit(base, n) only ever moves free_pos forward.
//
// Test list (t-wada style, §13.12):
// [ ] R1  init: used==0, peek==0, take_dropped==0
// [ ] R2  one line roundtrip: push("abc") -> peek "abc\n" (4B) at base 0
// [ ] R3  peek is non-destructive: same bytes, same base, twice
// [ ] R4  commit advances: after commit(base,4) peek==0, used==0
// [ ] R5  partial commit: 8B stored, commit 3 -> next peek is 5B at base+3
// [ ] R6  double commit is a no-op: used never wraps negative
// [ ] R7  stale (shorter) commit is ignored: free_pos never moves back
// [ ] R8  peek honors max: 100B stored, peek(max=16)==16
// [ ] R9  wraparound: bytes out == bytes in across the capacity boundary
// [ ] R10 full ring drop-oldest: one more push evicts exactly the oldest
//         line, take_dropped()==1
// [ ] R11 drops are whole lines: head after R10 is the 2nd-oldest line's
//         first byte
// [ ] R12 one push may drop several lines; take_dropped() counts them
// [ ] R13 take_dropped clears: second call returns 0
// [ ] R14 oversized line rejected: returns 0, ring untouched, dropped+1
// [ ] R15 in-flight overtake: commit with a base already passed by
//         drop-oldest does not rewind free_pos
// [ ] R16 empty push stores just "\n"
// [ ] S1  normal chunk: ring bytes verbatim, consumes_ring==1, base ok
// [ ] S2  drop marker is a chunk of its own; consumes_ring==0
// [ ] S3  the call after the marker returns a normal chunk again
// [ ] S4  no drops + empty ring -> 0
// [ ] S5  chunk length never exceeds max

#include <stdio.h>
#include <string.h>

#include "test_framework.h"

#include "log_ring.h"

// --- helpers ---------------------------------------------------------------

static struct log_ring g_ring;

static void reset_ring() { log_ring_init(&g_ring); }

static int push_str(const char *s) {
    return log_ring_push(&g_ring, s, (unsigned)strlen(s));
}

// A line whose stored size (body + '\n') is exactly 64 bytes, tagged with
// its index in the first 4 chars so eviction order is observable.
enum { TAG_LINE_STORED = 64 };
static void make_tag_line(unsigned idx, char *out /* >= 64 */) {
    snprintf(out, TAG_LINE_STORED, "%04u", idx);
    memset(out + 4, 'x', TAG_LINE_STORED - 1 - 4);
    out[TAG_LINE_STORED - 1] = '\0'; /* 63 chars; ring appends '\n' */
}

// Fill a fresh ring exactly to capacity with 64B tag lines 0..63.
static void fill_with_tag_lines() {
    reset_ring();
    char line[TAG_LINE_STORED];
    for (unsigned i = 0; i < LOG_RING_CAPACITY / TAG_LINE_STORED; ++i) {
        make_tag_line(i, line);
        CHECK(log_ring_push(&g_ring, line, (unsigned)strlen(line)) == 1);
    }
    CHECK(log_ring_used(&g_ring) == LOG_RING_CAPACITY);
}

// --- ring buffer -----------------------------------------------------------

static void test_r1_init_state() {
    reset_ring();
    char dst[8];
    unsigned base = 12345u;
    CHECK(log_ring_used(&g_ring) == 0);
    CHECK(log_ring_peek(&g_ring, dst, sizeof dst, &base) == 0);
    CHECK(log_ring_take_dropped(&g_ring) == 0);
}

static void test_r2_one_line_roundtrip() {
    reset_ring();
    char dst[8];
    unsigned base = 12345u;
    CHECK(push_str("abc") == 1);
    CHECK(log_ring_used(&g_ring) == 4);
    CHECK(log_ring_peek(&g_ring, dst, sizeof dst, &base) == 4);
    CHECK(memcmp(dst, "abc\n", 4) == 0);
    CHECK(base == 0);
}

static void test_r3_peek_is_nondestructive() {
    reset_ring();
    char a[8], b[8];
    unsigned base_a = 1u, base_b = 2u;
    CHECK(push_str("abc") == 1);
    CHECK(log_ring_peek(&g_ring, a, sizeof a, &base_a) == 4);
    CHECK(log_ring_peek(&g_ring, b, sizeof b, &base_b) == 4);
    CHECK(memcmp(a, b, 4) == 0);
    CHECK(base_a == base_b);
    CHECK(log_ring_used(&g_ring) == 4);
}

static void test_r4_commit_advances() {
    reset_ring();
    char dst[8];
    unsigned base = 0;
    CHECK(push_str("abc") == 1);
    CHECK(log_ring_peek(&g_ring, dst, sizeof dst, &base) == 4);
    log_ring_commit(&g_ring, base, 4);
    CHECK(log_ring_used(&g_ring) == 0);
    CHECK(log_ring_peek(&g_ring, dst, sizeof dst, &base) == 0);
}

static void test_r5_partial_commit() {
    reset_ring();
    char dst[16];
    unsigned base = 0;
    CHECK(push_str("abcdefg") == 1); /* stored: "abcdefg\n" = 8 bytes */
    CHECK(log_ring_peek(&g_ring, dst, sizeof dst, &base) == 8);
    log_ring_commit(&g_ring, base, 3);
    unsigned base2 = 0;
    CHECK(log_ring_peek(&g_ring, dst, sizeof dst, &base2) == 5);
    CHECK(memcmp(dst, "defg\n", 5) == 0);
    CHECK(base2 == base + 3);
}

static void test_r6_double_commit_noop() {
    reset_ring();
    char dst[8];
    unsigned base = 0;
    CHECK(push_str("abc") == 1);
    CHECK(log_ring_peek(&g_ring, dst, sizeof dst, &base) == 4);
    log_ring_commit(&g_ring, base, 4);
    log_ring_commit(&g_ring, base, 4);
    CHECK(log_ring_used(&g_ring) == 0);
    CHECK(push_str("de") == 1); /* ring still behaves after double commit */
    CHECK(log_ring_used(&g_ring) == 3);
}

static void test_r7_stale_commit_ignored() {
    reset_ring();
    char dst[8];
    unsigned base = 0;
    CHECK(push_str("abc") == 1);
    CHECK(log_ring_peek(&g_ring, dst, sizeof dst, &base) == 4);
    log_ring_commit(&g_ring, base, 4);
    log_ring_commit(&g_ring, base, 2); /* older, shorter: must not rewind */
    CHECK(log_ring_used(&g_ring) == 0);
    CHECK(log_ring_peek(&g_ring, dst, sizeof dst, &base) == 0);
}

static void test_r8_peek_honors_max() {
    reset_ring();
    char line[32];
    memset(line, 'y', 24);
    line[24] = '\0';
    for (int i = 0; i < 4; ++i) { /* 4 x 25 stored bytes = 100 */
        CHECK(log_ring_push(&g_ring, line, 24) == 1);
    }
    CHECK(log_ring_used(&g_ring) == 100);
    char dst[64];
    unsigned base = 0;
    CHECK(log_ring_peek(&g_ring, dst, 16, &base) == 16);
}

static void test_r9_wraparound_stream_intact() {
    reset_ring();
    enum { LINES = 500, LINE_STORED = 32 };
    static char expect[LINES * LINE_STORED];
    static char actual[LINES * LINE_STORED];
    unsigned expect_len = 0, actual_len = 0;
    char line[LINE_STORED];
    char dst[64];

    for (unsigned i = 0; i < LINES; ++i) {
        snprintf(line, sizeof line, "%05u", i);
        memset(line + 5, 'z', LINE_STORED - 1 - 5);
        line[LINE_STORED - 1] = '\0'; /* 31 chars + '\n' = 32 stored */
        CHECK(log_ring_push(&g_ring, line, (unsigned)strlen(line)) == 1);
        memcpy(expect + expect_len, line, LINE_STORED - 1);
        expect[expect_len + LINE_STORED - 1] = '\n';
        expect_len += LINE_STORED;

        // Drain fully each iteration so nothing is ever dropped.
        for (;;) {
            unsigned base = 0;
            unsigned n = log_ring_peek(&g_ring, dst, sizeof dst, &base);
            if (n == 0) break;
            memcpy(actual + actual_len, dst, n);
            actual_len += n;
            log_ring_commit(&g_ring, base, n);
        }
    }
    CHECK(log_ring_take_dropped(&g_ring) == 0);
    CHECK(actual_len == expect_len);
    CHECK(memcmp(actual, expect, expect_len) == 0);
}

static void test_r10_full_ring_drops_oldest() {
    fill_with_tag_lines();
    char line[TAG_LINE_STORED];
    make_tag_line(9999, line);
    CHECK(log_ring_push(&g_ring, line, (unsigned)strlen(line)) == 1);
    CHECK(log_ring_take_dropped(&g_ring) == 1);
    CHECK(log_ring_used(&g_ring) == LOG_RING_CAPACITY);
}

static void test_r11_drop_is_whole_line() {
    fill_with_tag_lines();
    char line[TAG_LINE_STORED];
    make_tag_line(9999, line);
    CHECK(log_ring_push(&g_ring, line, (unsigned)strlen(line)) == 1);
    char dst[8];
    unsigned base = 0;
    CHECK(log_ring_peek(&g_ring, dst, 4, &base) == 4);
    CHECK(memcmp(dst, "0001", 4) == 0); /* line 0000 is gone, cleanly */
}

static void test_r12_one_push_drops_several_lines() {
    fill_with_tag_lines();
    char big[121];
    memset(big, 'b', 120);
    big[120] = '\0'; /* 121 stored bytes: needs two 64B lines evicted */
    CHECK(log_ring_push(&g_ring, big, 120) == 1);
    CHECK(log_ring_take_dropped(&g_ring) == 2);
    char dst[8];
    unsigned base = 0;
    CHECK(log_ring_peek(&g_ring, dst, 4, &base) == 4);
    CHECK(memcmp(dst, "0002", 4) == 0);
}

static void test_r13_take_dropped_clears() {
    fill_with_tag_lines();
    char line[TAG_LINE_STORED];
    make_tag_line(9999, line);
    CHECK(log_ring_push(&g_ring, line, (unsigned)strlen(line)) == 1);
    CHECK(log_ring_take_dropped(&g_ring) == 1);
    CHECK(log_ring_take_dropped(&g_ring) == 0);
}

static void test_r14_oversized_line_rejected() {
    reset_ring();
    CHECK(push_str("abc") == 1);
    char before[8];
    unsigned base_before = 0;
    CHECK(log_ring_peek(&g_ring, before, sizeof before, &base_before) == 4);

    static char huge[LOG_RING_CAPACITY + 1];
    memset(huge, 'h', LOG_RING_CAPACITY);
    huge[LOG_RING_CAPACITY] = '\0'; /* len+1 > capacity: must be refused */
    CHECK(log_ring_push(&g_ring, huge, LOG_RING_CAPACITY) == 0);

    CHECK(log_ring_take_dropped(&g_ring) == 1);
    CHECK(log_ring_used(&g_ring) == 4);
    char after[8];
    unsigned base_after = 0;
    CHECK(log_ring_peek(&g_ring, after, sizeof after, &base_after) == 4);
    CHECK(memcmp(before, after, 4) == 0);
    CHECK(base_before == base_after);
}

static void test_r15_overtaken_commit_does_not_rewind() {
    fill_with_tag_lines();
    char dst[TAG_LINE_STORED];
    unsigned base = 0;
    unsigned n = log_ring_peek(&g_ring, dst, TAG_LINE_STORED, &base);
    CHECK(n == TAG_LINE_STORED);

    // Overflow by two lines: free_pos overtakes base + n.
    char line[TAG_LINE_STORED];
    make_tag_line(9998, line);
    CHECK(log_ring_push(&g_ring, line, (unsigned)strlen(line)) == 1);
    make_tag_line(9999, line);
    CHECK(log_ring_push(&g_ring, line, (unsigned)strlen(line)) == 1);
    CHECK(log_ring_take_dropped(&g_ring) == 2);

    unsigned used_before = log_ring_used(&g_ring);
    log_ring_commit(&g_ring, base, n); /* stale in-flight ack */
    CHECK(log_ring_used(&g_ring) == used_before);
    char head[8];
    unsigned head_base = 0;
    CHECK(log_ring_peek(&g_ring, head, 4, &head_base) == 4);
    CHECK(memcmp(head, "0002", 4) == 0);
}

static void test_r16_empty_push_stores_newline() {
    reset_ring();
    CHECK(log_ring_push(&g_ring, "", 0) == 1);
    CHECK(log_ring_used(&g_ring) == 1);
    char dst[4];
    unsigned base = 0;
    CHECK(log_ring_peek(&g_ring, dst, sizeof dst, &base) == 1);
    CHECK(dst[0] == '\n');
}

// --- TX chunk builder ------------------------------------------------------

static void test_s1_normal_chunk() {
    reset_ring();
    CHECK(push_str("hello") == 1);
    char dst[64];
    unsigned base = 77u;
    int consumes = -1;
    unsigned d = log_ring_take_dropped(&g_ring);
    unsigned n = log_ring_next_chunk(&g_ring, d, 0, dst, sizeof dst, &base,
                                     &consumes);
    CHECK(n == 6);
    CHECK(memcmp(dst, "hello\n", 6) == 0);
    CHECK(consumes == 1);
    CHECK(base == 0);
}

static void test_s2_drop_marker_is_its_own_chunk() {
    fill_with_tag_lines();
    char line[TAG_LINE_STORED];
    make_tag_line(9999, line);
    CHECK(log_ring_push(&g_ring, line, (unsigned)strlen(line)) == 1);

    char dst[128];
    unsigned base = 0;
    int consumes = -1;
    unsigned d = log_ring_take_dropped(&g_ring);
    CHECK(d == 1);
    unsigned n = log_ring_next_chunk(&g_ring, d, 7, dst, sizeof dst, &base,
                                     &consumes);
    const char *want = "#- [log] dropped=1 contended=7\n";
    CHECK(n == (unsigned)strlen(want));
    CHECK(memcmp(dst, want, n) == 0);
    CHECK(consumes == 0);
    CHECK(log_ring_used(&g_ring) == LOG_RING_CAPACITY); /* ring untouched */
}

static void test_s3_after_marker_back_to_normal() {
    fill_with_tag_lines();
    char line[TAG_LINE_STORED];
    make_tag_line(9999, line);
    CHECK(log_ring_push(&g_ring, line, (unsigned)strlen(line)) == 1);

    char dst[600];
    unsigned base = 0;
    int consumes = -1;
    unsigned d = log_ring_take_dropped(&g_ring);
    (void)log_ring_next_chunk(&g_ring, d, 0, dst, sizeof dst, &base, &consumes);

    d = log_ring_take_dropped(&g_ring); /* now 0: marker consumed */
    CHECK(d == 0);
    unsigned n = log_ring_next_chunk(&g_ring, d, 0, dst, sizeof dst, &base,
                                     &consumes);
    CHECK(n > 0);
    CHECK(consumes == 1);
    CHECK(memcmp(dst, "0001", 4) == 0);
}

static void test_s4_empty_ring_no_chunk() {
    reset_ring();
    char dst[64];
    unsigned base = 0;
    int consumes = -1;
    CHECK(log_ring_next_chunk(&g_ring, 0, 0, dst, sizeof dst, &base,
                              &consumes) == 0);
}

static void test_s5_chunk_capped_at_max() {
    fill_with_tag_lines(); /* 4096 bytes available */
    char dst[100];
    unsigned base = 0;
    int consumes = -1;
    unsigned n = log_ring_next_chunk(&g_ring, 0, 0, dst, 100, &base,
                                     &consumes);
    CHECK(n == 100);
}

int main() {
    RUN_TEST(test_r1_init_state);
    RUN_TEST(test_r2_one_line_roundtrip);
    RUN_TEST(test_r3_peek_is_nondestructive);
    RUN_TEST(test_r4_commit_advances);
    RUN_TEST(test_r5_partial_commit);
    RUN_TEST(test_r6_double_commit_noop);
    RUN_TEST(test_r7_stale_commit_ignored);
    RUN_TEST(test_r8_peek_honors_max);
    RUN_TEST(test_r9_wraparound_stream_intact);
    RUN_TEST(test_r10_full_ring_drops_oldest);
    RUN_TEST(test_r11_drop_is_whole_line);
    RUN_TEST(test_r12_one_push_drops_several_lines);
    RUN_TEST(test_r13_take_dropped_clears);
    RUN_TEST(test_r14_oversized_line_rejected);
    RUN_TEST(test_r15_overtaken_commit_does_not_rewind);
    RUN_TEST(test_r16_empty_push_stores_newline);
    RUN_TEST(test_s1_normal_chunk);
    RUN_TEST(test_s2_drop_marker_is_its_own_chunk);
    RUN_TEST(test_s3_after_marker_back_to_normal);
    RUN_TEST(test_s4_empty_ring_no_chunk);
    RUN_TEST(test_s5_chunk_capped_at_max);
    return testfw::summary();
}
