// SPP log send state machine (design §13.3.4). See spp_tx_fsm.h for the
// rules; this file just encodes them.

#include "spp_tx_fsm.h"

void spp_tx_fsm_init(struct spp_tx_fsm *f) {
    f->connected = 0;
    f->inflight = 0;
    f->done_len = 0;
    f->inflight_base = 0;
    f->inflight_len = 0;
    f->sent_at_ms = 0;
}

int spp_tx_fsm_on_connected(struct spp_tx_fsm *f) {
    f->connected = 1;
    f->inflight = 0;
    return 1;
}

void spp_tx_fsm_on_disconnected(struct spp_tx_fsm *f) {
    f->connected = 0;
    f->inflight = 0;
}

void spp_tx_fsm_on_data_sent(struct spp_tx_fsm *f, unsigned len) {
    f->done_len = len;
    f->inflight = 0;
}

unsigned spp_tx_fsm_reap(struct spp_tx_fsm *f, unsigned *base_out, unsigned now_ms) {
    (void)now_ms;
    if (f->inflight) {
        return 0;
    }
    if (f->inflight_len != 0) {
        unsigned n = (f->done_len < f->inflight_len) ? f->done_len : f->inflight_len;
        *base_out = f->inflight_base;
        f->inflight_len = 0;
        return n;
    }
    return 0;
}

int spp_tx_fsm_ready(const struct spp_tx_fsm *f) {
    return f->connected && !f->inflight;
}

void spp_tx_fsm_sending(struct spp_tx_fsm *f, unsigned base, unsigned ring_len, unsigned now_ms) {
    f->inflight = 1;
    f->inflight_base = base;
    f->inflight_len = ring_len;
    f->sent_at_ms = now_ms;
}

void spp_tx_fsm_send_failed(struct spp_tx_fsm *f) {
    f->inflight = 0;
    f->inflight_len = 0;
}
