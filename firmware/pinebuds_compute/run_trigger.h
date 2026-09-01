// Tap-triggered run arbitration for the two-bud MPI job (design §14).
//
// Pure logic, target-independent, gnu++98, no heap: decides whether a
// local 5-tap or a START frame from the peer bud should start a GEMM-MPI
// run, and whether the peer must be told. Both buds have to run the job
// together (rank 0 and 1 exchange data), so a tap on one bud starts both.
//
// Rules: while a run is in progress every input is ignored. When idle, a
// local tap starts a run with a fresh sequence number and asks the caller
// to notify the peer (RUN_TRIGGER_START_NOTIFY); a START from the peer
// starts a run without notifying (RUN_TRIGGER_START). TWS commands are not
// echoed back to the sender, so no sequence-based duplicate filtering is
// needed; seq only labels runs in the log and is kept monotonic by
// adopting max(own, peer).
//
// Threading: callers serialise access (app thread tap, BesbtThread RX and
// the compute thread all touch one struct).
#ifndef PINEBUDS_RUN_TRIGGER_H
#define PINEBUDS_RUN_TRIGGER_H

struct run_trigger {
    unsigned seq;  /* sequence number of the latest run (0 = none yet) */
    int running;   /* 1 while a run is in progress */
};

enum run_trigger_action {
    RUN_TRIGGER_NONE = 0,         /* ignore the input */
    RUN_TRIGGER_START = 1,        /* start a run, peer already knows */
    RUN_TRIGGER_START_NOTIFY = 2  /* start a run and send START(seq) to the peer */
};

void run_trigger_init(struct run_trigger *t);

/* Local 5-tap. On START_NOTIFY *seq_out is the new run's sequence number. */
int run_trigger_on_local_tap(struct run_trigger *t, unsigned *seq_out);

/* START(peer_seq) frame received from the peer bud. */
int run_trigger_on_peer_start(struct run_trigger *t, unsigned peer_seq);

/* The run (MPI_Finalize) finished; no-op when idle. */
void run_trigger_on_run_done(struct run_trigger *t);

int run_trigger_is_running(const struct run_trigger *t);

#endif
