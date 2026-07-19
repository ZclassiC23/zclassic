/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * v2_transport — per-connection Noise_XX handshake driver + record wrapper that
 * sits strictly BELOW p2p_node's message framing and ABOVE the raw socket. It
 * transforms bytes only: the plaintext handed up after decrypt is byte-identical
 * to what a v1 peer sent raw, and the ciphertext is produced from the exact
 * `struct msg_header` + payload p2p_node_end_message already assembled. A peer
 * with `node->transport == NULL` (every zclassicd peer, and every zcl23 peer
 * until -v2transport negotiates) takes the exact v1 code path with zero added
 * hot-path branches. No consensus/relay/dispatch code observes transport mode.
 *
 * Layered over the Phase-0 session library (lib/session): Noise_XX handshake
 * (noise_handshake.h) then a 3-byte-length ChaCha20-Poly1305 record layer
 * (session_transport.h). See docs/work/os/A4-noise-transport-p1.md and
 * docs/work/secure-transport-design.md. */

#ifndef ZCL_NET_V2_TRANSPORT_H
#define ZCL_NET_V2_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "session/noise_handshake.h"
#include "session/session_transport.h"
#include "util/sync.h"

/* Handshake / lifecycle state of one transport. */
enum v2_hs_state {
    V2_DETECT = 0,          /* responder: awaiting first bytes (magic vs eph pubkey) */
    V2_KEY_SENT,            /* initiator: our msg1 (`-> e`) queued, awaiting msg2 */
    V2_KEY_RECV,            /* responder: consumed msg1, msg2 queued, awaiting msg3 */
    V2_ESTABLISHED,         /* Split() done; session_transport record layer live */
    V2_PLAINTEXT_FALLBACK,  /* responder saw v1 network magic — caller must drop the
                             * transport and speak plaintext; buffered bytes surfaced */
    V2_FAILED,              /* abort: caller must drop the peer */
};

/* Prologue bytes bound into the Noise transcript on both sides. */
#define V2_TRANSPORT_VERSION_BYTE 0x02
#define V2_TRANSPORT_SUITE_ID     0x01
#define V2_TRANSPORT_PROLOGUE_LEN 6   /* magic(4) | version(1) | suite(1) */

struct v2_transport {
    enum v2_hs_state state;
    bool is_initiator;
    unsigned char magic[4];             /* network magic, for the DETECT classifier */

    struct noise_handshake hs;          /* live DETECT..pre-ESTABLISHED */
    struct session_transport rec;       /* live once ESTABLISHED */

    uint8_t peer_static[32];            /* XX-learned remote static (TOFU pin, Phase 2) */
    bool have_peer_static;

    /* Inbound accumulator: a handshake message or one session frame may span
     * recv() boundaries. Bounded by SESSION_FRAME_MAX_WIRE — a claimed frame
     * larger than that is malformed and fails closed. */
    uint8_t acc[SESSION_FRAME_MAX_WIRE];
    size_t  acc_len;

    /* Outbound application bytes assembled BEFORE the handshake completes
     * (e.g. the initiator's push_version fires before Noise finishes). Buffered
     * here by v2_transport_write and flushed sealed on ESTABLISHED. */
    uint8_t *pending;
    size_t   pending_len, pending_cap;

    zcl_mutex_t lock;                   /* leaf lock: guards state/acc/pending/hs/rec */
    uint64_t send_frames, recv_frames;  /* record counters (diagnostics) */
    int64_t  hs_started_us;             /* handshake-start timestamp (bench/DoS) */
};

/* Allocate + arm a transport. is_initiator=true (outbound dial): produces msg1
 * (`-> e`) into *initial_out (heap, caller frees) and enters V2_KEY_SENT.
 * is_initiator=false (inbound accept): *initial_out=NULL, enters V2_DETECT.
 * `identity_priv` is the 32-byte persistent static scalar; `magic` the 4-byte
 * network magic. Returns NULL on OOM / handshake-init failure (caller keeps the
 * plaintext path). */
struct v2_transport *v2_transport_begin(bool is_initiator,
                                        const uint8_t identity_priv[32],
                                        const unsigned char magic[4],
                                        uint8_t **initial_out, size_t *initial_len);

/* WRITE seam. Take one already-assembled v1 message (header+payload,
 * buf[0..total)). If ESTABLISHED, seal it into >=1 session DATA records appended
 * to *out (heap, caller frees) — a >SESSION_MAX_PAYLOAD message becomes N
 * records the receiver's v1 accumulator reassembles transparently. If still
 * handshaking, buffer it (out=NULL,*out_len=0, returns true) for a sealed flush
 * on completion. Returns false (state->V2_FAILED) on a terminal state or AEAD
 * failure — caller drops the peer. */
bool v2_transport_write(struct v2_transport *t, const uint8_t *buf, size_t total,
                        uint8_t **out, size_t *out_len);

/* READ seam. Feed raw recv bytes in[0..n). Drives the handshake while state <
 * V2_ESTABLISHED, queuing reply bytes (+ any flushed sealed pending) to
 * *wire_out (heap, caller frees, send verbatim). Once ESTABLISHED, peels
 * complete records and appends decrypted DATA-channel bytes to *plaintext (heap,
 * caller frees; may be 0 bytes if a record is still partial). On a v1-magic
 * responder detection it sets state=V2_PLAINTEXT_FALLBACK and surfaces the
 * buffered raw bytes via *plaintext (caller then frees+NULLs the transport).
 * Returns false on any AEAD/turn/oversize failure — caller drops the peer. */
bool v2_transport_feed(struct v2_transport *t,
                       const uint8_t *in, size_t n,
                       uint8_t **wire_out, size_t *wire_out_len,
                       uint8_t **plaintext, size_t *plaintext_len);

/* First-bytes classifier: true when the first <=4 bytes equal the network magic
 * (=> a v1 zclassicd peer). Only meaningful in V2_DETECT. */
bool v2_transport_is_plaintext_magic(const uint8_t *first, size_t n,
                                     const unsigned char magic[4]);

/* Cleanse all secret material and free. NULL-safe. */
void v2_transport_free(struct v2_transport *t);

/* Diagnostics: fill an object for one peer (mode/state/frame counters). */
struct json_value;
bool v2_transport_dump_peer(struct json_value *out, const struct v2_transport *t);

#endif /* ZCL_NET_V2_TRANSPORT_H */
