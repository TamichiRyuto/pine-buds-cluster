// Host-side TDD tests for the SPP log send state machine
// (firmware/pinebuds_compute/spp_tx_fsm). See design doc §13.3.4 / §13.14.
//
// The state machine decides, from the SDK's CONNECTED / DISCONNECTED /
// DATA_SENT events and the log thread's own send attempts, how many ring
// bytes may be committed and whether a new chunk may go out. It never
// touches the ring or the SDK itself.
//
// Test list (t-wada style):
// [x] F1 init: not ready, nothing to reap
// [x] F2 connected -> ready; after sending(base 0, 512) not ready and
//        reap returns 0 while the send is in flight
// [x] F3 data_sent(512) -> reap returns 512 at base 0, ready again, and a
//        second reap returns 0 (a send is reaped once)
// [x] F4 short write: data_sent(100) after sending 512 -> reap returns 100
// [x] F5 over-report: data_sent(517) after sending 512 -> reap returns 512
// [x] F6 marker chunk (ring_len 0): data_sent(40) -> reap returns 0, ready
// [x] F7 send_failed -> ready again, reap returns 0
// [x] F8 disconnected -> not ready; connected again -> ready
//
// Device runs 11/13 (design §13.14): the SDK raises REMDEV_CONNECTED twice
// per RFCOMM open (SPPNEW_EVENT_NEW_OPEN, then SPPNEW_EVENT_OPEN a few ms
// later). The second one used to clear `inflight` under a live send, so
// the thread committed min(stale done_len, len) -- 0 on the very first
// chunk -- and re-sent it (Run 11's "!! GAP 11 -> 1").
// [x] F9  duplicate CONNECTED while a chunk is in flight is ignored
//         (on_connected returns 0, dup_connected counts it, send stays in
//         flight until DATA_SENT reaps it)
// [x] F10 duplicate CONNECTED after a completed cycle does not commit the
//         stale done_len of the previous chunk
// [x] F11 DISCONNECTED with a chunk in flight discards it uncommitted
//         (at-least-once: it is re-peeked after the reconnect)
// [x] F12 a send unacknowledged for SPP_TX_FSM_TIMEOUT_MS is given up
//         without commit (ready again, timeouts++) -- Run 13's stale DLC
//         whose DATA_SENT never came must not wedge the channel now that
//         a later CONNECTED no longer resets it
// [x] F13 the timeout compares 32-bit ms with wrap-around

#include <stdio.h>

#include "test_framework.h"

#include "spp_tx_fsm.h"

namespace {

void test_f1_init_not_ready() {
    spp_tx_fsm f;
    spp_tx_fsm_init(&f);
    unsigned base = 77u;
    CHECK(spp_tx_fsm_ready(&f) == 0);
    CHECK(spp_tx_fsm_reap(&f, &base, 0u) == 0u);
    CHECK(base == 77u);
}

void test_f2_connected_then_sending_is_inflight() {
    spp_tx_fsm f;
    spp_tx_fsm_init(&f);
    CHECK(spp_tx_fsm_on_connected(&f) == 1);
    CHECK(spp_tx_fsm_ready(&f) == 1);
    spp_tx_fsm_sending(&f, 0u, 512u, 0u);
    CHECK(spp_tx_fsm_ready(&f) == 0);
    unsigned base = 77u;
    CHECK(spp_tx_fsm_reap(&f, &base, 100u) == 0u);
    CHECK(base == 77u);
    CHECK(spp_tx_fsm_ready(&f) == 0);
}

void test_f3_data_sent_reaps_once() {
    spp_tx_fsm f;
    spp_tx_fsm_init(&f);
    spp_tx_fsm_on_connected(&f);
    spp_tx_fsm_sending(&f, 0u, 512u, 0u);
    spp_tx_fsm_on_data_sent(&f, 512u);
    unsigned base = 77u;
    CHECK(spp_tx_fsm_reap(&f, &base, 100u) == 512u);
    CHECK(base == 0u);
    CHECK(spp_tx_fsm_ready(&f) == 1);
    CHECK(spp_tx_fsm_reap(&f, &base, 200u) == 0u);
}

void test_f4_short_write_reaps_reported_length() {
    spp_tx_fsm f;
    spp_tx_fsm_init(&f);
    spp_tx_fsm_on_connected(&f);
    spp_tx_fsm_sending(&f, 1024u, 512u, 0u);
    spp_tx_fsm_on_data_sent(&f, 100u);
    unsigned base = 77u;
    CHECK(spp_tx_fsm_reap(&f, &base, 100u) == 100u);
    CHECK(base == 1024u);
}

void test_f5_over_report_is_clamped_to_sent_length() {
    spp_tx_fsm f;
    spp_tx_fsm_init(&f);
    spp_tx_fsm_on_connected(&f);
    spp_tx_fsm_sending(&f, 0u, 512u, 0u);
    spp_tx_fsm_on_data_sent(&f, 517u);
    unsigned base = 77u;
    CHECK(spp_tx_fsm_reap(&f, &base, 100u) == 512u);
}

void test_f6_marker_chunk_commits_nothing() {
    spp_tx_fsm f;
    spp_tx_fsm_init(&f);
    spp_tx_fsm_on_connected(&f);
    spp_tx_fsm_sending(&f, 7u, 0u, 0u); /* ring_len 0 = drop marker */
    CHECK(spp_tx_fsm_ready(&f) == 0);
    spp_tx_fsm_on_data_sent(&f, 40u);
    unsigned base = 77u;
    CHECK(spp_tx_fsm_reap(&f, &base, 100u) == 0u);
    CHECK(spp_tx_fsm_ready(&f) == 1);
}

void test_f7_send_failed_is_ready_again() {
    spp_tx_fsm f;
    spp_tx_fsm_init(&f);
    spp_tx_fsm_on_connected(&f);
    spp_tx_fsm_sending(&f, 0u, 512u, 0u);
    spp_tx_fsm_send_failed(&f);
    CHECK(spp_tx_fsm_ready(&f) == 1);
    unsigned base = 77u;
    CHECK(spp_tx_fsm_reap(&f, &base, 100u) == 0u);
}

void test_f8_disconnect_then_reconnect() {
    spp_tx_fsm f;
    spp_tx_fsm_init(&f);
    spp_tx_fsm_on_connected(&f);
    spp_tx_fsm_on_disconnected(&f);
    CHECK(spp_tx_fsm_ready(&f) == 0);
    CHECK(spp_tx_fsm_on_connected(&f) == 1);
    CHECK(spp_tx_fsm_ready(&f) == 1);
}

void test_f9_duplicate_connected_while_inflight_is_ignored() {
    spp_tx_fsm f;
    spp_tx_fsm_init(&f);
    CHECK(spp_tx_fsm_on_connected(&f) == 1);
    spp_tx_fsm_sending(&f, 0u, 512u, 0u);
    CHECK(spp_tx_fsm_on_connected(&f) == 0); /* SDK: NEW_OPEN then OPEN */
    CHECK(f.dup_connected == 1u);
    CHECK(spp_tx_fsm_ready(&f) == 0);
    unsigned base = 77u;
    CHECK(spp_tx_fsm_reap(&f, &base, 10u) == 0u);
    spp_tx_fsm_on_data_sent(&f, 512u);
    CHECK(spp_tx_fsm_reap(&f, &base, 20u) == 512u);
    CHECK(base == 0u);
}

void test_f10_duplicate_connected_does_not_commit_stale_done_len() {
    spp_tx_fsm f;
    spp_tx_fsm_init(&f);
    spp_tx_fsm_on_connected(&f);
    spp_tx_fsm_sending(&f, 0u, 512u, 0u);
    spp_tx_fsm_on_data_sent(&f, 512u);
    unsigned base = 77u;
    CHECK(spp_tx_fsm_reap(&f, &base, 10u) == 512u);
    spp_tx_fsm_sending(&f, 512u, 300u, 10u);
    CHECK(spp_tx_fsm_on_connected(&f) == 0);
    CHECK(spp_tx_fsm_reap(&f, &base, 20u) == 0u); /* still in flight */
    CHECK(spp_tx_fsm_ready(&f) == 0);
}

void test_f11_disconnect_discards_inflight_chunk() {
    spp_tx_fsm f;
    spp_tx_fsm_init(&f);
    spp_tx_fsm_on_connected(&f);
    spp_tx_fsm_sending(&f, 0u, 512u, 0u);
    spp_tx_fsm_on_data_sent(&f, 512u);
    unsigned base = 77u;
    CHECK(spp_tx_fsm_reap(&f, &base, 10u) == 512u);
    spp_tx_fsm_sending(&f, 512u, 300u, 10u);
    spp_tx_fsm_on_disconnected(&f);
    CHECK(spp_tx_fsm_reap(&f, &base, 20u) == 0u); /* never confirmed: resend later */
    CHECK(spp_tx_fsm_ready(&f) == 0);
    CHECK(spp_tx_fsm_on_connected(&f) == 1);
    CHECK(spp_tx_fsm_ready(&f) == 1);
}

void test_f12_inflight_timeout_resends_without_commit() {
    spp_tx_fsm f;
    spp_tx_fsm_init(&f);
    spp_tx_fsm_on_connected(&f);
    spp_tx_fsm_sending(&f, 0u, 512u, 1000u);
    unsigned base = 77u;
    CHECK(spp_tx_fsm_reap(&f, &base, 1000u + SPP_TX_FSM_TIMEOUT_MS - 1u) == 0u);
    CHECK(spp_tx_fsm_ready(&f) == 0);
    CHECK(spp_tx_fsm_reap(&f, &base, 1000u + SPP_TX_FSM_TIMEOUT_MS) == 0u);
    CHECK(spp_tx_fsm_ready(&f) == 1);
    CHECK(f.timeouts == 1u);
    CHECK(base == 77u);
}

void test_f13_timeout_wraps_around_32bit_ms() {
    spp_tx_fsm f;
    spp_tx_fsm_init(&f);
    spp_tx_fsm_on_connected(&f);
    spp_tx_fsm_sending(&f, 0u, 512u, 0xFFFFFF00u);
    unsigned base = 77u;
    CHECK(spp_tx_fsm_reap(&f, &base, 0x00000010u) == 0u); /* 272 ms elapsed */
    CHECK(spp_tx_fsm_ready(&f) == 0);
}

}  // namespace

int main() {
    RUN_TEST(test_f1_init_not_ready);
    RUN_TEST(test_f2_connected_then_sending_is_inflight);
    RUN_TEST(test_f3_data_sent_reaps_once);
    RUN_TEST(test_f4_short_write_reaps_reported_length);
    RUN_TEST(test_f5_over_report_is_clamped_to_sent_length);
    RUN_TEST(test_f6_marker_chunk_commits_nothing);
    RUN_TEST(test_f7_send_failed_is_ready_again);
    RUN_TEST(test_f8_disconnect_then_reconnect);
    RUN_TEST(test_f9_duplicate_connected_while_inflight_is_ignored);
    RUN_TEST(test_f10_duplicate_connected_does_not_commit_stale_done_len);
    RUN_TEST(test_f11_disconnect_discards_inflight_chunk);
    RUN_TEST(test_f12_inflight_timeout_resends_without_commit);
    RUN_TEST(test_f13_timeout_wraps_around_32bit_ms);
    return testfw::summary();
}
