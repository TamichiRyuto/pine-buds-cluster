// Host-side TDD tests for the fragmentation module (adapters/mpi/mpi_frag).
//
// mpi_frag splits messages into wire frames that fit one IBRT TWS command
// (docs/design-ibrt-transport.md §4), reassembles them on the peer, and
// enforces credit-window flow control (§5). It is target-independent: the
// wire and credits are injected through mpi_frag_port, so everything here
// runs on host. The wire format is pinned byte-by-byte because both buds
// must agree on it (same binary, little-endian Cortex-M4).
//
// Test list (t-wada style):
// [x] F1 single-fragment roundtrip: small payload -> 1 DATA frame
//     (hdr 12B + payload), on_frame delivers byte-exact and emits an ACK;
//     feeding the ACK back releases one credit
// [x] F2 fragmentation: 512B -> 2 DATA frames, each <= MPI_FRAG_FRAME_BYTES,
//     header fields pinned (kind/src/dst/idx/cnt/total_len/tag, LE),
//     reassembled byte-exact in one deliver
// [x] F3 interleaved msg_id during reassembly: error counted, original
//     message still completes intact
// [x] F4 credit denied: send fails with an error, nothing emitted

#include <string.h>

#include "test_framework.h"

#include "mpi.h"       // error code contract shared with the adapter core
#include "mpi_frag.h"

// --- fake port: records frames, delivers into flat capture buffers ---
#define MAX_FRAMES 8

static unsigned char g_frames[MAX_FRAMES][MPI_FRAG_FRAME_BYTES];
static int g_frame_lens[MAX_FRAMES];
static int g_nframes = 0;

static int fake_emit(const void *frame, int frame_len) {
    CHECK(g_nframes < MAX_FRAMES);
    CHECK(frame_len <= MPI_FRAG_FRAME_BYTES);
    memcpy(g_frames[g_nframes], frame, frame_len);
    g_frame_lens[g_nframes] = frame_len;
    ++g_nframes;
    return 0;
}

static int g_deliver_calls = 0;
static int g_del_src = -1, g_del_dst = -1, g_del_tag = -1, g_del_len = -1;
static unsigned char g_del_buf[MPI_ADAPTER_MAX_PAYLOAD_BYTES];

static int fake_deliver(int source, int dest, int tag, const void *buf,
                        int byte_len) {
    ++g_deliver_calls;
    g_del_src = source;
    g_del_dst = dest;
    g_del_tag = tag;
    g_del_len = byte_len;
    memcpy(g_del_buf, buf, byte_len);
    return 0;
}

static int g_credit_ok = 1;
static int g_acquires = 0;
static int g_releases = 0;

static int fake_acquire(void) {
    ++g_acquires;
    return g_credit_ok ? 0 : 1;
}
static void fake_release(void) { ++g_releases; }

static void reset_port_and_init() {
    static mpi_frag_port port;
    port.emit = &fake_emit;
    port.deliver = &fake_deliver;
    port.acquire_credit = &fake_acquire;
    port.release_credit = &fake_release;
    g_nframes = 0;
    g_deliver_calls = 0;
    g_credit_ok = 1;
    g_acquires = 0;
    g_releases = 0;
    mpi_frag_init(&port);
}

static void test_single_fragment_roundtrip() {
    reset_port_and_init();

    unsigned char payload[8];
    for (int i = 0; i < 8; ++i) payload[i] = (unsigned char)(0xA0 + i);

    CHECK(mpi_frag_send(0, 1, 42, payload, 8) == MPI_SUCCESS);
    CHECK(g_nframes == 1);
    CHECK(g_frame_lens[0] == MPI_FRAG_HDR_BYTES + 8);
    CHECK(g_frames[0][0] == MPI_FRAG_KIND_DATA);

    // Peer side: same shared instance plays the receiver.
    CHECK(mpi_frag_on_frame(g_frames[0], g_frame_lens[0]) == MPI_SUCCESS);
    CHECK(g_deliver_calls == 1);
    CHECK(g_del_src == 0);
    CHECK(g_del_dst == 1);
    CHECK(g_del_tag == 42);
    CHECK(g_del_len == 8);
    CHECK(memcmp(g_del_buf, payload, 8) == 0);

    // Receiving DATA must have emitted an ACK frame.
    CHECK(g_nframes == 2);
    CHECK(g_frames[1][0] == MPI_FRAG_KIND_ACK);
    CHECK(g_frame_lens[1] == MPI_FRAG_HDR_BYTES);

    // Feeding the ACK back releases exactly one credit.
    CHECK(g_releases == 0);
    CHECK(mpi_frag_on_frame(g_frames[1], g_frame_lens[1]) == MPI_SUCCESS);
    CHECK(g_releases == 1);
}

static void test_two_fragment_reassembly_and_wire_format() {
    reset_port_and_init();

    unsigned char payload[MPI_ADAPTER_MAX_PAYLOAD_BYTES];
    for (int i = 0; i < MPI_ADAPTER_MAX_PAYLOAD_BYTES; ++i) {
        payload[i] = (unsigned char)(i & 0xFF);
    }

    CHECK(mpi_frag_send(1, 0, 7, payload, MPI_ADAPTER_MAX_PAYLOAD_BYTES) ==
          MPI_SUCCESS);
    CHECK(g_nframes == 2);
    CHECK(g_acquires == 2);  // one credit per DATA fragment

    // Wire format pinning (design §4 offset table, little-endian).
    for (int f = 0; f < 2; ++f) {
        const unsigned char *h = g_frames[f];
        CHECK(g_frame_lens[f] <= MPI_FRAG_FRAME_BYTES);
        CHECK(h[0] == MPI_FRAG_KIND_DATA);   // kind
        CHECK(h[1] == 1);                    // src
        CHECK(h[2] == 0);                    // dst
        CHECK(h[4] == f);                    // frag_idx
        CHECK(h[5] == 2);                    // frag_cnt
        CHECK(h[6] == (MPI_ADAPTER_MAX_PAYLOAD_BYTES & 0xFF));         // total_len lo
        CHECK(h[7] == ((MPI_ADAPTER_MAX_PAYLOAD_BYTES >> 8) & 0xFF));  // total_len hi
        CHECK(h[8] == 7 && h[9] == 0 && h[10] == 0 && h[11] == 0);     // tag LE
    }
    CHECK(g_frames[0][3] == g_frames[1][3]);  // same msg_id

    CHECK(mpi_frag_on_frame(g_frames[0], g_frame_lens[0]) == MPI_SUCCESS);
    CHECK(g_deliver_calls == 0);  // not yet complete
    CHECK(mpi_frag_on_frame(g_frames[1], g_frame_lens[1]) == MPI_SUCCESS);
    CHECK(g_deliver_calls == 1);
    CHECK(g_del_src == 1);
    CHECK(g_del_dst == 0);
    CHECK(g_del_tag == 7);
    CHECK(g_del_len == MPI_ADAPTER_MAX_PAYLOAD_BYTES);
    CHECK(memcmp(g_del_buf, payload, MPI_ADAPTER_MAX_PAYLOAD_BYTES) == 0);
}

static void test_interleaved_msg_id_is_error() {
    reset_port_and_init();

    unsigned char payload[MPI_ADAPTER_MAX_PAYLOAD_BYTES];
    for (int i = 0; i < MPI_ADAPTER_MAX_PAYLOAD_BYTES; ++i) {
        payload[i] = (unsigned char)(0x55 ^ (i & 0xFF));
    }
    CHECK(mpi_frag_send(0, 1, 3, payload, MPI_ADAPTER_MAX_PAYLOAD_BYTES) ==
          MPI_SUCCESS);
    CHECK(g_nframes == 2);

    unsigned tx0 = 0, rx0 = 0, err0 = 0;
    mpi_frag_counters(&tx0, &rx0, &err0);

    // First fragment starts reassembly.
    CHECK(mpi_frag_on_frame(g_frames[0], g_frame_lens[0]) == MPI_SUCCESS);

    // Inject a fragment from the same source with a different msg_id.
    unsigned char rogue[MPI_FRAG_HDR_BYTES + 4];
    memcpy(rogue, g_frames[0], MPI_FRAG_HDR_BYTES + 4);
    rogue[3] = (unsigned char)(rogue[3] + 1);  // msg_id
    CHECK(mpi_frag_on_frame(rogue, MPI_FRAG_HDR_BYTES + 4) == MPI_ERR_INTERN);

    unsigned tx1 = 0, rx1 = 0, err1 = 0;
    mpi_frag_counters(&tx1, &rx1, &err1);
    CHECK(err1 == err0 + 1);

    // The in-progress message still completes intact.
    CHECK(mpi_frag_on_frame(g_frames[1], g_frame_lens[1]) == MPI_SUCCESS);
    CHECK(g_deliver_calls == 1);
    CHECK(g_del_len == MPI_ADAPTER_MAX_PAYLOAD_BYTES);
    CHECK(memcmp(g_del_buf, payload, MPI_ADAPTER_MAX_PAYLOAD_BYTES) == 0);
}

static void test_credit_denied_fails_send() {
    reset_port_and_init();
    g_credit_ok = 0;

    float x = 1.0f;
    CHECK(mpi_frag_send(0, 1, 1, &x, (int)sizeof(x)) == MPI_ERR_OTHER);
    CHECK(g_nframes == 0);
}

int main() {
    RUN_TEST(test_single_fragment_roundtrip);
    RUN_TEST(test_two_fragment_reassembly_and_wire_format);
    RUN_TEST(test_interleaved_msg_id_is_error);
    RUN_TEST(test_credit_denied_fails_send);
    return testfw::summary();
}
