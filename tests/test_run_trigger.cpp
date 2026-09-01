// Host-side TDD tests for the tap-triggered run arbitration
// (firmware/pinebuds_compute/run_trigger). See design doc §14.
//
// Test list (t-wada style):
// [ ] T1 init: idle, seq 0
// [ ] T2 local tap when idle -> START_NOTIFY, seq 1, running
// [ ] T3 local tap while running -> NONE, seq unchanged
// [ ] T4 run_done -> idle; the next local tap gets seq 2
// [ ] T5 peer START(7) when idle -> START, running, seq 7
// [ ] T6 peer START while running -> NONE
// [ ] T7 peer START with an older seq (3 after 7) when idle -> START,
//        seq stays 7 (monotonic)
// [ ] T8 run_done when idle is a no-op

#include <stdio.h>

#include "test_framework.h"

#include "run_trigger.h"

namespace {

void test_t1_init_idle() {
    run_trigger t;
    run_trigger_init(&t);
    CHECK(run_trigger_is_running(&t) == 0);
    CHECK(t.seq == 0u);
}

void test_t2_local_tap_starts_and_notifies() {
    run_trigger t;
    run_trigger_init(&t);
    unsigned seq = 99u;
    CHECK(run_trigger_on_local_tap(&t, &seq) == RUN_TRIGGER_START_NOTIFY);
    CHECK(seq == 1u);
    CHECK(t.seq == 1u);
    CHECK(run_trigger_is_running(&t) == 1);
}

void test_t3_local_tap_while_running_ignored() {
    run_trigger t;
    run_trigger_init(&t);
    unsigned seq = 0u;
    run_trigger_on_local_tap(&t, &seq);
    seq = 99u;
    CHECK(run_trigger_on_local_tap(&t, &seq) == RUN_TRIGGER_NONE);
    CHECK(seq == 99u);
    CHECK(t.seq == 1u);
    CHECK(run_trigger_is_running(&t) == 1);
}

void test_t4_run_done_then_next_tap_is_seq_2() {
    run_trigger t;
    run_trigger_init(&t);
    unsigned seq = 0u;
    run_trigger_on_local_tap(&t, &seq);
    run_trigger_on_run_done(&t);
    CHECK(run_trigger_is_running(&t) == 0);
    CHECK(run_trigger_on_local_tap(&t, &seq) == RUN_TRIGGER_START_NOTIFY);
    CHECK(seq == 2u);
}

void test_t5_peer_start_when_idle() {
    run_trigger t;
    run_trigger_init(&t);
    CHECK(run_trigger_on_peer_start(&t, 7u) == RUN_TRIGGER_START);
    CHECK(run_trigger_is_running(&t) == 1);
    CHECK(t.seq == 7u);
}

void test_t6_peer_start_while_running_ignored() {
    run_trigger t;
    run_trigger_init(&t);
    unsigned seq = 0u;
    run_trigger_on_local_tap(&t, &seq);
    CHECK(run_trigger_on_peer_start(&t, 1u) == RUN_TRIGGER_NONE);
    CHECK(t.seq == 1u);
    CHECK(run_trigger_is_running(&t) == 1);
}

void test_t7_peer_start_with_older_seq_keeps_monotonic() {
    run_trigger t;
    run_trigger_init(&t);
    run_trigger_on_peer_start(&t, 7u);
    run_trigger_on_run_done(&t);
    CHECK(run_trigger_on_peer_start(&t, 3u) == RUN_TRIGGER_START);
    CHECK(t.seq == 7u);
    CHECK(run_trigger_is_running(&t) == 1);
}

void test_t8_run_done_when_idle_is_noop() {
    run_trigger t;
    run_trigger_init(&t);
    run_trigger_on_run_done(&t);
    CHECK(run_trigger_is_running(&t) == 0);
    CHECK(t.seq == 0u);
}

}  // namespace

int main() {
    RUN_TEST(test_t1_init_idle);
    RUN_TEST(test_t2_local_tap_starts_and_notifies);
    RUN_TEST(test_t3_local_tap_while_running_ignored);
    RUN_TEST(test_t4_run_done_then_next_tap_is_seq_2);
    RUN_TEST(test_t5_peer_start_when_idle);
    RUN_TEST(test_t6_peer_start_while_running_ignored);
    RUN_TEST(test_t7_peer_start_with_older_seq_keeps_monotonic);
    RUN_TEST(test_t8_run_done_when_idle_is_noop);
    return testfw::summary();
}
