// Host-side TDD tests for the MPI adapter (adapters/mpi).
//
// The adapter must let an unmodified MPI benchmark source compile and run.
// On host, ranks are simulated in-process: a bootstrap seam sets rank/size
// before the benchmark calls MPI_Init (on target the IBRT link does this).
//
// Test list (t-wada style):
// [x] bootstrap + MPI_Init: MPI_Comm_rank/MPI_Comm_size report rank 0 of 2
// [x] rank 1 bootstrap: MPI_Comm_rank reports 1 (triangulation)
// [x] MPI_Initialized: 0 before MPI_Init, 1 after
// [x] MPI_Finalize: returns MPI_SUCCESS, MPI_Finalized flips to 1
// [x] MPI_Send/MPI_Recv MPI_FLOAT: rank0 -> rank1 payload arrives intact
// [x] MPI_Send beyond max payload bytes: rejected with an error (no overflow)
// [x] MPI_Send with queue full: rejected with an error (not silent success)
// [x] MPI_Recv with no matching message: error (loopback cannot block)
// [x] MPI_Barrier: returns MPI_SUCCESS (real sync deferred to target transport)
// [x] MPI_Wtime: monotonic non-negative seconds (double signature for API
//     compatibility; internally single-precision on target)
// [x] pthread port: MPI_Recv blocks until the peer thread sends, and
//     MPI_Comm_rank reports each thread's own rank
// [x] MPI_Allreduce MPI_SUM on floats across 2 ranks (threaded)
// [x] MPI_Barrier under pthread port: real rendezvous of both ranks
// [x] MPI_Isend completes eagerly; MPI_Wait on it returns MPI_SUCCESS
// [x] MPI_Irecv + MPI_Waitall in sequential mode: message already queued
//     is delivered with status filled
// [x] halo-exchange pattern under pthread port: both ranks post Irecv then
//     Isend then Waitall(2) without deadlock, payloads cross (himeno sendp
//     shape)
// [x] request table exhaustion: starting more requests than slots errors
// Transport seam (docs/design-ibrt-transport.md §3, §8):
// [x] T1 wire connected (threaded): Send crosses the wire (tx counter > 0),
//     Recv gets the payload via mpi_adapter_deliver
// [x] T2 wire disconnected (sequential): Send returns the transport error
//     and nothing lands in the local queue (Recv -> MPI_ERR_OTHER)
// [x] T6 regression: without a transport installed behavior is unchanged
//     (covered by every existing test above)

#include <time.h>

#include "test_framework.h"

#include "mpi.h"
#include "mpi_adapter.h"  // bootstrap seam (not part of the MPI API surface)
#include "mpi_thread_port.h"

static void test_init_rank_size() {
    mpi_adapter_bootstrap(0, 2);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);

    int rank = -1;
    int size = -1;
    CHECK(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
    CHECK(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
    CHECK(rank == 0);
    CHECK(size == 2);

    CHECK(MPI_Finalize() == MPI_SUCCESS);
}

static void test_rank1_bootstrap() {
    mpi_adapter_bootstrap(1, 2);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);

    int rank = -1;
    CHECK(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
    CHECK(rank == 1);

    CHECK(MPI_Finalize() == MPI_SUCCESS);
}

static void test_initialized_finalized_flags() {
    // Fresh adapter state: bootstrap resets the init/finalize flags too.
    mpi_adapter_bootstrap(0, 2);

    int flag = -1;
    CHECK(MPI_Initialized(&flag) == MPI_SUCCESS);
    CHECK(flag == 0);
    CHECK(MPI_Finalized(&flag) == MPI_SUCCESS);
    CHECK(flag == 0);

    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);
    CHECK(MPI_Initialized(&flag) == MPI_SUCCESS);
    CHECK(flag == 1);
    CHECK(MPI_Finalized(&flag) == MPI_SUCCESS);
    CHECK(flag == 0);

    CHECK(MPI_Finalize() == MPI_SUCCESS);
    CHECK(MPI_Finalized(&flag) == MPI_SUCCESS);
    CHECK(flag == 1);
}

static void test_send_recv_float() {
    // Rank 0 sends; then we re-bootstrap as rank 1 in the same process and
    // receive. The adapter's loopback queue must survive re-bootstrap.
    mpi_adapter_bootstrap(0, 2);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);
    float out[3] = {1.0f, 2.0f, 3.0f};
    CHECK(MPI_Send(out, 3, MPI_FLOAT, 1, 42, MPI_COMM_WORLD) == MPI_SUCCESS);
    CHECK(MPI_Finalize() == MPI_SUCCESS);

    mpi_adapter_bootstrap(1, 2);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);
    float in[3] = {0.0f, 0.0f, 0.0f};
    MPI_Status status;
    CHECK(MPI_Recv(in, 3, MPI_FLOAT, 0, 42, MPI_COMM_WORLD, &status) ==
          MPI_SUCCESS);
    CHECK_EQ_F(in[0], 1.0f);
    CHECK_EQ_F(in[1], 2.0f);
    CHECK_EQ_F(in[2], 3.0f);
    CHECK(status.MPI_SOURCE == 0);
    CHECK(status.MPI_TAG == 42);
    CHECK(MPI_Finalize() == MPI_SUCCESS);
}

static void test_loopback_error_paths() {
    mpi_adapter_bootstrap(0, 2);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);

    // Payload larger than one queue slot must be rejected, not overflowed.
    float big[MPI_ADAPTER_MAX_PAYLOAD_BYTES / 4 + 1];
    const int big_count = (int)(sizeof(big) / sizeof(big[0]));
    for (int i = 0; i < big_count; ++i) big[i] = 0.0f;
    CHECK(MPI_Send(big, big_count, MPI_FLOAT, 1, 1, MPI_COMM_WORLD) ==
          MPI_ERR_COUNT);

    // The loopback cannot block, so an unmatched receive is an error.
    float in = 0.0f;
    MPI_Status status;
    CHECK(MPI_Recv(&in, 1, MPI_FLOAT, 1, 99, MPI_COMM_WORLD, &status) ==
          MPI_ERR_OTHER);

    // Filling every slot succeeds; one more send must report the full queue.
    float x = 7.0f;
    for (int i = 0; i < MPI_ADAPTER_QUEUE_SLOTS; ++i) {
        CHECK(MPI_Send(&x, 1, MPI_FLOAT, 1, i, MPI_COMM_WORLD) == MPI_SUCCESS);
    }
    CHECK(MPI_Send(&x, 1, MPI_FLOAT, 1, 999, MPI_COMM_WORLD) ==
          MPI_ERR_INTERN);
    CHECK(MPI_Finalize() == MPI_SUCCESS);

    // Drain as rank 1 so the queue is empty for whatever runs next.
    mpi_adapter_bootstrap(1, 2);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);
    for (int i = 0; i < MPI_ADAPTER_QUEUE_SLOTS; ++i) {
        CHECK(MPI_Recv(&in, 1, MPI_FLOAT, 0, i, MPI_COMM_WORLD, &status) ==
              MPI_SUCCESS);
        CHECK_EQ_F(in, 7.0f);
    }
    CHECK(MPI_Finalize() == MPI_SUCCESS);
}

static void test_barrier_returns_success() {
    // The sequential host loopback cannot express real synchronization;
    // Barrier is a no-op here and gains real sync in the target transport.
    mpi_adapter_bootstrap(0, 2);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);
    CHECK(MPI_Barrier(MPI_COMM_WORLD) == MPI_SUCCESS);
    CHECK(MPI_Finalize() == MPI_SUCCESS);
}

static void test_wtime_monotonic() {
    mpi_adapter_bootstrap(0, 2);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);

    double t0 = MPI_Wtime();
    CHECK(t0 >= 0.0);
    // Burn a little CPU so a coarse clock still has a chance to advance.
    volatile float sink = 0.0f;
    for (int i = 0; i < 100000; ++i) sink = sink + 1.0f;
    double t1 = MPI_Wtime();
    CHECK(t1 >= t0);

    CHECK(MPI_Finalize() == MPI_SUCCESS);
}

// Results from rank threads; the testfw counters are not thread-safe, so
// rank bodies only record into these and the main thread does the CHECKs.
static int g_threaded_send_rc = -1;
static int g_threaded_recv_rc = -1;
static float g_threaded_recv_val = 0.0f;
static int g_threaded_observed_rank[2] = {-1, -1};

static void threaded_send_recv_body(int rank) {
    MPI_Comm_rank(MPI_COMM_WORLD, &g_threaded_observed_rank[rank]);
    if (rank == 1) {
        // Runs first (rank 0 sleeps), so this Recv must genuinely block.
        float in = 0.0f;
        MPI_Status status;
        g_threaded_recv_rc =
            MPI_Recv(&in, 1, MPI_FLOAT, 0, 7, MPI_COMM_WORLD, &status);
        g_threaded_recv_val = in;
    } else {
        struct timespec ts = {0, 50L * 1000L * 1000L};  // 50 ms head start
        nanosleep(&ts, 0);
        float out = 5.0f;
        g_threaded_send_rc = MPI_Send(&out, 1, MPI_FLOAT, 1, 7, MPI_COMM_WORLD);
    }
}

static void test_threaded_blocking_recv() {
    mpi_adapter_bootstrap(0, 2);  // size = 2; per-thread rank comes from port
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);
    mpi_thread_port::run_two_ranks(&threaded_send_recv_body);
    CHECK(g_threaded_send_rc == MPI_SUCCESS);
    CHECK(g_threaded_recv_rc == MPI_SUCCESS);
    CHECK_EQ_F(g_threaded_recv_val, 5.0f);
    CHECK(g_threaded_observed_rank[0] == 0);
    CHECK(g_threaded_observed_rank[1] == 1);
    CHECK(MPI_Finalize() == MPI_SUCCESS);
}

static int g_allreduce_rc[2] = {-1, -1};
static float g_allreduce_out[2][2];

static void threaded_allreduce_body(int rank) {
    float in[2];
    in[0] = (rank == 0) ? 1.0f : 10.0f;
    in[1] = (rank == 0) ? 2.0f : 20.0f;
    float out[2] = {0.0f, 0.0f};
    g_allreduce_rc[rank] =
        MPI_Allreduce(in, out, 2, MPI_FLOAT, MPI_SUM, MPI_COMM_WORLD);
    g_allreduce_out[rank][0] = out[0];
    g_allreduce_out[rank][1] = out[1];
}

static void test_threaded_allreduce_sum() {
    mpi_adapter_bootstrap(0, 2);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);
    mpi_thread_port::run_two_ranks(&threaded_allreduce_body);
    CHECK(g_allreduce_rc[0] == MPI_SUCCESS);
    CHECK(g_allreduce_rc[1] == MPI_SUCCESS);
    CHECK_EQ_F(g_allreduce_out[0][0], 11.0f);
    CHECK_EQ_F(g_allreduce_out[0][1], 22.0f);
    CHECK_EQ_F(g_allreduce_out[1][0], 11.0f);
    CHECK_EQ_F(g_allreduce_out[1][1], 22.0f);
    CHECK(MPI_Finalize() == MPI_SUCCESS);
}

static int g_barrier_rc[2] = {-1, -1};
static volatile int g_pre_barrier_flag = 0;
static int g_flag_seen_by_rank1 = -1;

static void threaded_barrier_body(int rank) {
    if (rank == 0) {
        // Arrive late; a real barrier makes rank 1 wait for this flag.
        struct timespec ts = {0, 50L * 1000L * 1000L};
        nanosleep(&ts, 0);
        g_pre_barrier_flag = 1;
        g_barrier_rc[0] = MPI_Barrier(MPI_COMM_WORLD);
    } else {
        g_barrier_rc[1] = MPI_Barrier(MPI_COMM_WORLD);
        g_flag_seen_by_rank1 = g_pre_barrier_flag;
    }
}

static void test_threaded_barrier_rendezvous() {
    g_pre_barrier_flag = 0;
    mpi_adapter_bootstrap(0, 2);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);
    mpi_thread_port::run_two_ranks(&threaded_barrier_body);
    CHECK(g_barrier_rc[0] == MPI_SUCCESS);
    CHECK(g_barrier_rc[1] == MPI_SUCCESS);
    CHECK(g_flag_seen_by_rank1 == 1);
    CHECK(MPI_Finalize() == MPI_SUCCESS);
}

static void test_isend_irecv_waitall_sequential() {
    // Send side: eager Isend, then Wait on the send request.
    mpi_adapter_bootstrap(0, 2);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);
    float out[2] = {3.0f, 4.0f};
    MPI_Request sreq;
    CHECK(MPI_Isend(out, 2, MPI_FLOAT, 1, 5, MPI_COMM_WORLD, &sreq) ==
          MPI_SUCCESS);
    MPI_Status sstatus;
    CHECK(MPI_Wait(&sreq, &sstatus) == MPI_SUCCESS);
    CHECK(sreq == MPI_REQUEST_NULL);
    CHECK(MPI_Finalize() == MPI_SUCCESS);

    // Receive side: Irecv finds the queued message at Waitall time.
    mpi_adapter_bootstrap(1, 2);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);
    float in[2] = {0.0f, 0.0f};
    MPI_Request rreq;
    CHECK(MPI_Irecv(in, 2, MPI_FLOAT, 0, 5, MPI_COMM_WORLD, &rreq) ==
          MPI_SUCCESS);
    MPI_Status rstatus;
    CHECK(MPI_Waitall(1, &rreq, &rstatus) == MPI_SUCCESS);
    CHECK_EQ_F(in[0], 3.0f);
    CHECK_EQ_F(in[1], 4.0f);
    CHECK(rstatus.MPI_SOURCE == 0);
    CHECK(rstatus.MPI_TAG == 5);
    CHECK(rreq == MPI_REQUEST_NULL);
    CHECK(MPI_Finalize() == MPI_SUCCESS);
}

// Himeno sendp-shaped exchange: each rank posts Irecv, then Isend, then
// Waitall over both — must complete without deadlock, payloads crossing.
static int g_halo_rc[2] = {-1, -1};
static float g_halo_in[2] = {0.0f, 0.0f};

static void threaded_halo_body(int rank) {
    int peer = 1 - rank;
    float out = (rank == 0) ? 100.0f : 200.0f;
    float in = 0.0f;
    MPI_Request reqs[2];
    MPI_Status stats[2];

    int rc = MPI_Irecv(&in, 1, MPI_FLOAT, peer, 1, MPI_COMM_WORLD, &reqs[0]);
    if (rc == MPI_SUCCESS) {
        rc = MPI_Isend(&out, 1, MPI_FLOAT, peer, 1, MPI_COMM_WORLD, &reqs[1]);
    }
    if (rc == MPI_SUCCESS) {
        rc = MPI_Waitall(2, reqs, stats);
    }
    g_halo_rc[rank] = rc;
    g_halo_in[rank] = in;
}

static void test_threaded_halo_exchange() {
    mpi_adapter_bootstrap(0, 2);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);
    mpi_thread_port::run_two_ranks(&threaded_halo_body);
    CHECK(g_halo_rc[0] == MPI_SUCCESS);
    CHECK(g_halo_rc[1] == MPI_SUCCESS);
    CHECK_EQ_F(g_halo_in[0], 200.0f);
    CHECK_EQ_F(g_halo_in[1], 100.0f);
    CHECK(MPI_Finalize() == MPI_SUCCESS);
}

static void test_request_table_exhaustion() {
    // Fill every request slot as rank 1, then satisfy and drain them so
    // later tests start with a clean table.
    mpi_adapter_bootstrap(1, 2);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);
    float in[MPI_ADAPTER_MAX_REQUESTS];
    MPI_Request reqs[MPI_ADAPTER_MAX_REQUESTS];
    for (int i = 0; i < MPI_ADAPTER_MAX_REQUESTS; ++i) {
        in[i] = 0.0f;
        CHECK(MPI_Irecv(&in[i], 1, MPI_FLOAT, 0, i, MPI_COMM_WORLD,
                        &reqs[i]) == MPI_SUCCESS);
    }
    float dummy = 0.0f;
    MPI_Request overflow;
    CHECK(MPI_Irecv(&dummy, 1, MPI_FLOAT, 0, 999, MPI_COMM_WORLD,
                    &overflow) != MPI_SUCCESS);
    CHECK(MPI_Finalize() == MPI_SUCCESS);

    mpi_adapter_bootstrap(0, 2);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);
    float x = 9.0f;
    for (int i = 0; i < MPI_ADAPTER_MAX_REQUESTS; ++i) {
        CHECK(MPI_Send(&x, 1, MPI_FLOAT, 1, i, MPI_COMM_WORLD) == MPI_SUCCESS);
    }
    CHECK(MPI_Finalize() == MPI_SUCCESS);

    mpi_adapter_bootstrap(1, 2);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);
    MPI_Status stats[MPI_ADAPTER_MAX_REQUESTS];
    CHECK(MPI_Waitall(MPI_ADAPTER_MAX_REQUESTS, reqs, stats) == MPI_SUCCESS);
    for (int i = 0; i < MPI_ADAPTER_MAX_REQUESTS; ++i) {
        CHECK_EQ_F(in[i], 9.0f);
    }
    CHECK(MPI_Finalize() == MPI_SUCCESS);
}

// --- Transport seam tests (fake wire harness) ---
// The fake wire is what the IBRT glue will be on target: transport.send
// carries the message to the peer's side and injects it via
// mpi_adapter_deliver. A tx counter proves the message really went through
// the wire and not through the local-queue shortcut.
static int g_wire_tx_count = 0;
static int g_wire_connected = 1;

static int fake_wire_send(int src, int dest, int tag, const void *buf,
                          int byte_len) {
    if (!g_wire_connected) {
        return MPI_ERR_OTHER;
    }
    ++g_wire_tx_count;
    return mpi_adapter_deliver(src, dest, tag, buf, byte_len);
}

static int g_transport_recv_rc = -1;
static float g_transport_recv_val = 0.0f;
static int g_transport_send_rc = -1;

static void threaded_transport_body(int rank) {
    if (rank == 1) {
        float in = 0.0f;
        MPI_Status status;
        g_transport_recv_rc =
            MPI_Recv(&in, 1, MPI_FLOAT, 0, 11, MPI_COMM_WORLD, &status);
        g_transport_recv_val = in;
    } else {
        struct timespec ts = {0, 20L * 1000L * 1000L};
        nanosleep(&ts, 0);
        float out = 6.0f;
        g_transport_send_rc =
            MPI_Send(&out, 1, MPI_FLOAT, 1, 11, MPI_COMM_WORLD);
    }
}

static void test_transport_wire_crossing() {
    mpi_adapter_transport transport;
    transport.send = &fake_wire_send;
    mpi_adapter_set_transport(&transport);
    g_wire_tx_count = 0;
    g_wire_connected = 1;

    mpi_adapter_bootstrap(0, 2);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);
    mpi_thread_port::run_two_ranks(&threaded_transport_body);
    CHECK(g_transport_send_rc == MPI_SUCCESS);
    CHECK(g_transport_recv_rc == MPI_SUCCESS);
    CHECK_EQ_F(g_transport_recv_val, 6.0f);
    CHECK(g_wire_tx_count > 0);
    CHECK(MPI_Finalize() == MPI_SUCCESS);

    mpi_adapter_set_transport(0);
}

static void test_transport_disconnected_wire() {
    mpi_adapter_transport transport;
    transport.send = &fake_wire_send;
    mpi_adapter_set_transport(&transport);
    g_wire_tx_count = 0;
    g_wire_connected = 0;

    // Sequential mode: a failed wire send must surface the error and must
    // NOT fall back to the local queue.
    mpi_adapter_bootstrap(0, 2);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);
    float out = 1.0f;
    CHECK(MPI_Send(&out, 1, MPI_FLOAT, 1, 12, MPI_COMM_WORLD) ==
          MPI_ERR_OTHER);
    CHECK(g_wire_tx_count == 0);
    CHECK(MPI_Finalize() == MPI_SUCCESS);

    mpi_adapter_bootstrap(1, 2);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);
    float in = 0.0f;
    MPI_Status status;
    CHECK(MPI_Recv(&in, 1, MPI_FLOAT, 0, 12, MPI_COMM_WORLD, &status) ==
          MPI_ERR_OTHER);
    CHECK(MPI_Finalize() == MPI_SUCCESS);

    mpi_adapter_set_transport(0);
}

int main() {
    RUN_TEST(test_init_rank_size);
    RUN_TEST(test_rank1_bootstrap);
    RUN_TEST(test_initialized_finalized_flags);
    RUN_TEST(test_send_recv_float);
    RUN_TEST(test_loopback_error_paths);
    RUN_TEST(test_barrier_returns_success);
    RUN_TEST(test_wtime_monotonic);
    RUN_TEST(test_threaded_blocking_recv);
    RUN_TEST(test_threaded_allreduce_sum);
    RUN_TEST(test_threaded_barrier_rendezvous);
    RUN_TEST(test_isend_irecv_waitall_sequential);
    RUN_TEST(test_threaded_halo_exchange);
    RUN_TEST(test_request_table_exhaustion);
    RUN_TEST(test_transport_wire_crossing);
    RUN_TEST(test_transport_disconnected_wire);
    return testfw::summary();
}
