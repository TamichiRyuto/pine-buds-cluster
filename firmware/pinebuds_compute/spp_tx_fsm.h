// SPP log send state machine (docs/design-ibrt-transport.md §13.3.4).
//
// Pure logic, target-independent, gnu++98, no heap: decides how many ring
// bytes may be committed and whether a new chunk may go out, from the SDK's
// CONNECTED / DISCONNECTED / DATA_SENT events and the log thread's own send
// attempts. It never touches the ring or the SDK itself; the caller passes
// ring positions/lengths in and gets a commit length back.
//
// Threading: the "event side" functions run on BesbtThread (the SPP
// callback); the "log-thread side" functions run on the single SPP log
// thread. Fields are laid out so the log thread only reads the three
// `volatile` fields the callback writes, and only the log thread touches
// the rest -- no lock is needed, same as the s_connected/s_inflight/
// s_done_len statics this replaces. Call the log-thread side once per
// wake-up in this order: reap, ready, sending/send_failed.
//
// The SDK delivers REMDEV_CONNECTED twice per RFCOMM open
// (SPPNEW_EVENT_NEW_OPEN, then SPPNEW_EVENT_OPEN a few ms later): a second
// CONNECTED while a chunk is in flight is ignored (dup_connected counts it,
// the send stays in flight until DATA_SENT reaps it), and DISCONNECTED
// discards any in-flight chunk uncommitted so it is re-peeked from the ring
// after the reconnect (at-least-once). A send left unacknowledged for
// SPP_TX_FSM_TIMEOUT_MS is given up without commit (timeouts++) so a stale
// DLC whose DATA_SENT never arrives cannot wedge the channel.
#ifndef PINEBUDS_COMPUTE_SPP_TX_FSM_H
#define PINEBUDS_COMPUTE_SPP_TX_FSM_H

enum { SPP_TX_FSM_TIMEOUT_MS = 5000 };

struct spp_tx_fsm {
    volatile int connected;      /* RFCOMM link up (set/cleared on BesbtThread) */
    volatile int inflight;       /* a btif_spp_write is unacknowledged */
    volatile unsigned done_len;  /* tx_data_length of the last DATA_SENT (BesbtThread writes, log thread reads) */
    unsigned inflight_base;      /* ring position of the in-flight chunk */
    unsigned inflight_len;       /* ring bytes in the in-flight chunk; 0 = marker chunk or nothing */
    unsigned sent_at_ms;         /* now_ms passed to spp_tx_fsm_sending, used for the in-flight timeout */
    unsigned dup_connected;      /* CONNECTED events ignored because already connected (statistics) */
    unsigned timeouts;           /* sends given up after SPP_TX_FSM_TIMEOUT_MS (statistics) */
};

void spp_tx_fsm_init(struct spp_tx_fsm *f);

/* Event side -- called from the SDK callback on BesbtThread. */
int  spp_tx_fsm_on_connected(struct spp_tx_fsm *f);     /* returns 1 when the link is now considered connected */
void spp_tx_fsm_on_disconnected(struct spp_tx_fsm *f);
void spp_tx_fsm_on_data_sent(struct spp_tx_fsm *f, unsigned len);

/* Log-thread side -- call once per wake-up in this order: reap, ready, sending/send_failed. */
unsigned spp_tx_fsm_reap(struct spp_tx_fsm *f, unsigned *base_out, unsigned now_ms);
int  spp_tx_fsm_ready(const struct spp_tx_fsm *f);
void spp_tx_fsm_sending(struct spp_tx_fsm *f, unsigned base, unsigned ring_len, unsigned now_ms);
void spp_tx_fsm_send_failed(struct spp_tx_fsm *f);

#endif /* PINEBUDS_COMPUTE_SPP_TX_FSM_H */
