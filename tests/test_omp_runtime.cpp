// Host-side TDD tests for the OpenMP runtime (adapters/omp).
//
// Stage 1 provided omp.h so sources including it compile with pragmas
// ignored. Stage 2 (design doc §15) keeps that header and adds the three
// libgomp-ABI entry points GCC emits for `#pragma omp parallel for`
// (GOMP_parallel + omp_get_num_threads/omp_get_thread_num inside the
// outlined function), driven through a worker port seam (omp_adapter.h).
// The target port runs the worker on the second Cortex-M4F via cp_accel;
// here a fake port records the calls.
//
// Test list (t-wada style):
// [x] omp_get_num_threads returns 1, omp_get_thread_num returns 0
// [x] omp_get_max_threads / omp_get_num_procs return 1
// [x] omp_set_num_threads is accepted and ignored (still 1 thread)
// [x] omp_get_wtime: monotonic non-negative seconds
// [x] R1 no port: GOMP_parallel runs fn(data) once inline, 1 thread / id 0
// [x] R2 port (worker_count 1): max_threads 2, num_procs 2, outside a
//        region num_threads is still 1
// [x] R3 team of 2: fn runs twice (worker + inline), worker sees id 1,
//        inline sees id 0, both see 2 threads, join once after inline
// [ ] R4 omp_set_num_threads: 1 -> no worker; 5 -> clamped to 2;
//        0 / negative -> ignored
// [ ] R5 num_threads argument overrides nthreads-var (1 -> sequential,
//        2 -> team of 2)
// [ ] R6 worker_start failure -> sequential fallback, no join
// [ ] R7 nested region runs inline with 1 thread / id 0; outer restored
// [ ] R8 after the region: 1 thread / id 0; omp_set_port(NULL) -> max 1

#include "test_framework.h"

#include "omp.h"
#include "omp_adapter.h"

namespace {

// What the outlined function observed on each call.
struct Seen {
    int calls;
    void *data;
    int num_threads[4];
    int thread_num[4];
};
Seen g_seen;

void reset_seen() {
    g_seen.calls = 0;
    g_seen.data = 0;
    for (int i = 0; i < 4; ++i) {
        g_seen.num_threads[i] = -1;
        g_seen.thread_num[i] = -1;
    }
}

void record_fn(void *data) {
    int i = g_seen.calls++;
    g_seen.data = data;
    if (i < 4) {
        g_seen.num_threads[i] = omp_get_num_threads();
        g_seen.thread_num[i] = omp_get_thread_num();
    }
}

// Fake port: records calls, runs the worker's fn synchronously inside
// worker_start with self_is_worker reporting 1 for that call only.
struct FakePort {
    int starts;
    int joins;
    int start_rc;      // what worker_start returns
    int in_worker;     // 1 while running fn on behalf of the "worker"
    int join_seen_calls;  // g_seen.calls at the moment of worker_join
};
FakePort g_fake;

int fake_worker_count() { return 1; }
int fake_worker_start(void (*fn)(void *), void *data) {
    ++g_fake.starts;
    if (g_fake.start_rc != 0) {
        return g_fake.start_rc;
    }
    g_fake.in_worker = 1;
    fn(data);
    g_fake.in_worker = 0;
    return 0;
}
void fake_worker_join() {
    ++g_fake.joins;
    g_fake.join_seen_calls = g_seen.calls;
}
int fake_self_is_worker() { return g_fake.in_worker; }

const omp_port g_fake_port = {&fake_worker_count, &fake_worker_start,
                              &fake_worker_join, &fake_self_is_worker};

void install_fake_port() {
    g_fake.starts = 0;
    g_fake.joins = 0;
    g_fake.start_rc = 0;
    g_fake.in_worker = 0;
    g_fake.join_seen_calls = -1;
    omp_set_port(&g_fake_port);
}

}  // namespace

static void test_sequential_identity() {
    CHECK(omp_get_num_threads() == 1);
    CHECK(omp_get_thread_num() == 0);
}

static void test_thread_capacity_is_one() {
    CHECK(omp_get_max_threads() == 1);
    CHECK(omp_get_num_procs() == 1);

    // Benchmarks may ask for more threads; the stub accepts and ignores it.
    omp_set_num_threads(4);
    CHECK(omp_get_max_threads() == 1);
    CHECK(omp_get_num_threads() == 1);
}

static void test_wtime_monotonic() {
    double t0 = omp_get_wtime();
    CHECK(t0 >= 0.0);
    volatile float sink = 0.0f;
    for (int i = 0; i < 100000; ++i) sink = sink + 1.0f;
    double t1 = omp_get_wtime();
    CHECK(t1 >= t0);
}

static void test_r1_no_port_runs_inline_once() {
    omp_set_port(0);
    int payload = 42;
    reset_seen();

    GOMP_parallel(&record_fn, &payload, 0, 0);

    CHECK(g_seen.calls == 1);
    CHECK(g_seen.data == &payload);
    CHECK(g_seen.num_threads[0] == 1);
    CHECK(g_seen.thread_num[0] == 0);
}

static void test_r2_port_reports_two_threads_available() {
    install_fake_port();

    CHECK(omp_get_max_threads() == 2);
    CHECK(omp_get_num_procs() == 2);
    // Outside a parallel region there is still only the primary thread.
    CHECK(omp_get_num_threads() == 1);
    CHECK(omp_get_thread_num() == 0);

    omp_set_port(0);
}

static void test_r3_team_of_two_runs_worker_and_inline() {
    install_fake_port();
    int payload = 7;
    reset_seen();

    GOMP_parallel(&record_fn, &payload, 0, 0);

    // fn ran twice with the same data: once on the (fake) worker, once
    // inline on the primary, and both saw a team of 2.
    CHECK(g_seen.calls == 2);
    CHECK(g_seen.data == &payload);
    CHECK(g_fake.starts == 1);
    CHECK(g_seen.num_threads[0] == 2);
    CHECK(g_seen.num_threads[1] == 2);
    // The fake worker runs fn synchronously inside worker_start, so call 0
    // is the worker (id 1) and call 1 is the primary (id 0).
    CHECK(g_seen.thread_num[0] == 1);
    CHECK(g_seen.thread_num[1] == 0);
    // Exactly one join, issued after the primary finished its own share.
    CHECK(g_fake.joins == 1);
    CHECK(g_fake.join_seen_calls == 2);

    omp_set_port(0);
}

static void test_r4_set_num_threads_clamps_to_capacity() {
    install_fake_port();
    int payload = 0;

    // 1 -> sequential even with a worker available: no worker_start.
    omp_set_num_threads(1);
    CHECK(omp_get_max_threads() == 1);
    reset_seen();
    GOMP_parallel(&record_fn, &payload, 0, 0);
    CHECK(g_seen.calls == 1);
    CHECK(g_fake.starts == 0);
    CHECK(g_seen.num_threads[0] == 1);
    CHECK(g_seen.thread_num[0] == 0);

    // 5 -> clamped to the capacity (2).
    omp_set_num_threads(5);
    CHECK(omp_get_max_threads() == 2);
    reset_seen();
    GOMP_parallel(&record_fn, &payload, 0, 0);
    CHECK(g_seen.calls == 2);
    CHECK(g_fake.starts == 1);

    // 0 / negative -> ignored, previous value (2) stays.
    omp_set_num_threads(0);
    CHECK(omp_get_max_threads() == 2);
    omp_set_num_threads(-3);
    CHECK(omp_get_max_threads() == 2);

    omp_set_port(0);
}

int main() {
    RUN_TEST(test_sequential_identity);
    RUN_TEST(test_thread_capacity_is_one);
    RUN_TEST(test_wtime_monotonic);
    RUN_TEST(test_r1_no_port_runs_inline_once);
    RUN_TEST(test_r2_port_reports_two_threads_available);
    RUN_TEST(test_r3_team_of_two_runs_worker_and_inline);
    RUN_TEST(test_r4_set_num_threads_clamps_to_capacity);
    return testfw::summary();
}
