// Tap-triggered run arbitration (design §14). See run_trigger.h for the
// rules; this file just encodes them.

#include "run_trigger.h"

void run_trigger_init(struct run_trigger *t) {
    t->seq = 0;
    t->running = 0;
}

int run_trigger_on_local_tap(struct run_trigger *t, unsigned *seq_out) {
    if (t->running) {
        return RUN_TRIGGER_NONE;
    }
    t->seq += 1;
    t->running = 1;
    *seq_out = t->seq;
    return RUN_TRIGGER_START_NOTIFY;
}

int run_trigger_on_peer_start(struct run_trigger *t, unsigned peer_seq) {
    if (t->running) {
        return RUN_TRIGGER_NONE;
    }
    if (peer_seq > t->seq) { /* keep seq monotonic: max(own, peer) */
        t->seq = peer_seq;
    }
    t->running = 1;
    return RUN_TRIGGER_START;
}

void run_trigger_on_run_done(struct run_trigger *t) {
    t->running = 0;
}

int run_trigger_is_running(const struct run_trigger *t) {
    return t->running;
}
