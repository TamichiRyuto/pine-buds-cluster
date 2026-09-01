// Host-side TDD tests for the SPP log send state machine
// (firmware/pinebuds_compute/spp_tx_fsm). See design doc §13.3.4 / §13.14.
//
// The state machine decides, from the SDK's CONNECTED / DISCONNECTED /
// DATA_SENT events and the log thread's own send attempts, how many ring
// bytes may be committed and whether a new chunk may go out. It never
// touches the ring or the SDK itself.
//
// Test list (t-wada style):
// [ ] F1 init: not ready, nothing to reap
// [ ] F2 connected -> ready; after sending(base 0, 512) not ready and
//        reap returns 0 while the send is in flight
// [ ] F3 data_sent(512) -> reap returns 512 at base 0, ready again, and a
//        second reap returns 0 (a send is reaped once)
// [ ] F4 short write: data_sent(100) after sending 512 -> reap returns 100
// [ ] F5 over-report: data_sent(517) after sending 512 -> reap returns 512
// [ ] F6 marker chunk (ring_len 0): data_sent(40) -> reap returns 0, ready
// [ ] F7 send_failed -> ready again, reap returns 0
// [ ] F8 disconnected -> not ready; connected again -> ready

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
    return testfw::summary();
}
