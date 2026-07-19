/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * v2_transport — Noise_XX handshake driver + record wrapper over the Phase-0
 * session library. See net/v2_transport.h for the contract and
 * docs/work/os/A4-noise-transport-p1.md for the design. All new transport logic
 * lives here; it is the taken side of a single `if (node->transport)` on the
 * hot send/recv paths. Plaintext peers (transport==NULL) never reach this TU. */

#include "net/v2_transport.h"

#include <string.h>

#include "session/noise_handshake.h"
#include "session/session_transport.h"
#include "support/cleanse.h"
#include "core/utiltime.h"
#include "json/json.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

/* ── growable heap buffer ─────────────────────────────────────────── */

static bool buf_append(uint8_t **buf, size_t *len, size_t *cap,
                       const void *src, size_t n)
{
    if (n == 0)
        return true;
    if (*len + n > *cap) {
        size_t ncap = *cap ? *cap : 256;
        while (ncap < *len + n)
            ncap *= 2;
        uint8_t *nb = zcl_realloc(*buf, ncap, "v2_transport.buf");
        if (!nb)
            return false;
        *buf = nb;
        *cap = ncap;
    }
    memcpy(*buf + *len, src, n);
    *len += n;
    return true;
}

/* ── record sealing (ESTABLISHED) ─────────────────────────────────── */

/* Split `buf` into SESSION_MAX_PAYLOAD-byte DATA records; append each sealed
 * wire frame to (*out,*out_len,*out_cap). Caller holds t->lock. */
static bool seal_locked(struct v2_transport *t, const uint8_t *buf, size_t total,
                        uint8_t **out, size_t *out_len, size_t *out_cap)
{
    size_t off = 0;
    while (off < total) {
        size_t chunk = total - off;
        if (chunk > SESSION_MAX_PAYLOAD)
            chunk = SESSION_MAX_PAYLOAD;
        uint8_t frame[SESSION_FRAME_MAX_WIRE];
        size_t flen = 0;
        if (!session_transport_encrypt(&t->rec, SESSION_CH_DATA,
                                       buf + off, chunk, frame, &flen))
            return false;
        if (!buf_append(out, out_len, out_cap, frame, flen))
            return false;
        t->send_frames++;
        off += chunk;
    }
    return true;
}

/* Split() -> record layer; capture peer static; flush any pending outbound
 * bytes sealed into (*wire,*wlen,*wcap). Caller holds t->lock. */
static bool establish_and_flush_locked(struct v2_transport *t,
                                       uint8_t **wire, size_t *wlen, size_t *wcap)
{
    uint8_t sk[32], rk[32];
    bool ok = noise_hs_split(&t->hs, sk, rk);
    if (ok)
        session_transport_init(&t->rec, sk, rk);
    memory_cleanse(sk, sizeof(sk));
    memory_cleanse(rk, sizeof(rk));
    if (!ok)
        return false;

    uint8_t rs[32];
    if (noise_hs_remote_static(&t->hs, rs)) {
        memcpy(t->peer_static, rs, sizeof(t->peer_static));
        t->have_peer_static = true;
    }
    memory_cleanse(rs, sizeof(rs));
    noise_hs_cleanse(&t->hs);
    t->state = V2_ESTABLISHED;

    if (t->pending_len &&
        !seal_locked(t, t->pending, t->pending_len, wire, wlen, wcap))
        return false;
    if (t->pending) {
        memory_cleanse(t->pending, t->pending_cap);
        free(t->pending);
        t->pending = NULL;
        t->pending_len = 0;
        t->pending_cap = 0;
    }
    return true;
}

/* ── begin ────────────────────────────────────────────────────────── */

struct v2_transport *v2_transport_begin(bool is_initiator,
                                        const uint8_t identity_priv[32],
                                        const unsigned char magic[4],
                                        uint8_t **initial_out, size_t *initial_len)
{
    if (initial_out)
        *initial_out = NULL;
    if (initial_len)
        *initial_len = 0;
    if (!identity_priv || !magic)
        LOG_NULL("net", "v2_transport_begin: NULL identity/magic");

    struct v2_transport *t = zcl_calloc(1, sizeof(*t), "v2_transport");
    if (!t)
        LOG_NULL("net", "v2_transport_begin: calloc failed");
    zcl_mutex_init(&t->lock);
    t->is_initiator = is_initiator;
    memcpy(t->magic, magic, 4);
    t->hs_started_us = GetTimeMicros();

    uint8_t prologue[V2_TRANSPORT_PROLOGUE_LEN];
    memcpy(prologue, magic, 4);
    prologue[4] = V2_TRANSPORT_VERSION_BYTE;
    prologue[5] = V2_TRANSPORT_SUITE_ID;

    if (!noise_hs_init(&t->hs, noise_pattern_xx(), is_initiator,
                       prologue, sizeof(prologue), identity_priv, NULL)) {
        zcl_mutex_destroy(&t->lock);
        free(t);
        LOG_NULL("net", "v2_transport_begin: noise_hs_init failed");
    }

    if (is_initiator) {
        uint8_t out[NOISE_MAX_MESSAGE];
        size_t out_len = 0;
        if (!noise_hs_write_message(&t->hs, NULL, 0, out, &out_len)) {
            noise_hs_cleanse(&t->hs);
            zcl_mutex_destroy(&t->lock);
            free(t);
            LOG_NULL("net", "v2_transport_begin: noise msg1 write failed");
        }
        if (initial_out && initial_len) {
            uint8_t *m = zcl_malloc(out_len ? out_len : 1, "v2_transport.msg1");
            if (!m) {
                noise_hs_cleanse(&t->hs);
                zcl_mutex_destroy(&t->lock);
                free(t);
                LOG_NULL("net", "v2_transport_begin: msg1 buffer alloc failed");
            }
            memcpy(m, out, out_len);
            *initial_out = m;
            *initial_len = out_len;
        }
        t->state = V2_KEY_SENT;
    } else {
        t->state = V2_DETECT;
    }
    return t;
}

/* ── write seam ───────────────────────────────────────────────────── */

bool v2_transport_write(struct v2_transport *t, const uint8_t *buf, size_t total,
                        uint8_t **out, size_t *out_len)
{
    if (!t || !out || !out_len)
        LOG_FAIL("net", "v2_transport_write: NULL argument");
    *out = NULL;
    *out_len = 0;

    zcl_mutex_lock(&t->lock);

    if (t->state == V2_ESTABLISHED) {
        uint8_t *o = NULL;
        size_t olen = 0, ocap = 0;
        if (!seal_locked(t, buf, total, &o, &olen, &ocap)) {
            t->state = V2_FAILED;
            free(o);
            zcl_mutex_unlock(&t->lock);
            LOG_FAIL("net", "v2_transport_write: seal failed");
        }
        *out = o;
        *out_len = olen;
        zcl_mutex_unlock(&t->lock);
        return true;
    }

    if (t->state == V2_DETECT || t->state == V2_KEY_SENT ||
        t->state == V2_KEY_RECV) {
        /* Handshake in flight: buffer the assembled v1 message; it is sealed
         * and flushed on ESTABLISHED (see establish_and_flush_locked). */
        if (!buf_append(&t->pending, &t->pending_len, &t->pending_cap,
                        buf, total)) {
            t->state = V2_FAILED;
            zcl_mutex_unlock(&t->lock);
            LOG_FAIL("net", "v2_transport_write: pending buffer OOM");
        }
        zcl_mutex_unlock(&t->lock);
        return true;
    }

    /* V2_FAILED / V2_PLAINTEXT_FALLBACK — transport unusable. */
    zcl_mutex_unlock(&t->lock);
    LOG_FAIL("net", "v2_transport_write: terminal state %d", (int)t->state);
}

/* ── read seam ────────────────────────────────────────────────────── */

enum step_result { STEP_CONSUMED, STEP_NEEDMORE, STEP_STOP, STEP_FAIL };

bool v2_transport_feed(struct v2_transport *t,
                       const uint8_t *in, size_t n,
                       uint8_t **wire_out, size_t *wire_out_len,
                       uint8_t **plaintext, size_t *plaintext_len)
{
    if (!t || !wire_out || !wire_out_len || !plaintext || !plaintext_len)
        LOG_FAIL("net", "v2_transport_feed: NULL argument");
    *wire_out = NULL;
    *wire_out_len = 0;
    *plaintext = NULL;
    *plaintext_len = 0;

    zcl_mutex_lock(&t->lock);

    uint8_t *wire = NULL;
    size_t wlen = 0, wcap = 0;
    uint8_t *pt = NULL;
    size_t plen = 0, pcap = 0;
    size_t in_off = 0;
    bool ok = true;

    for (;;) {
        /* refill acc from `in`, bounded by SESSION_FRAME_MAX_WIRE */
        size_t space = SESSION_FRAME_MAX_WIRE - t->acc_len;
        if (space && in_off < n) {
            size_t take = n - in_off;
            if (take > space)
                take = space;
            memcpy(t->acc + t->acc_len, in + in_off, take);
            t->acc_len += take;
            in_off += take;
        }

        enum step_result r = STEP_NEEDMORE;
        switch (t->state) {
        case V2_DETECT: {
            if (t->acc_len < 4) { r = STEP_NEEDMORE; break; }
            if (memcmp(t->acc, t->magic, 4) == 0) {
                /* v1 zclassicd peer: surface the buffered raw bytes as
                 * plaintext and signal the caller to drop the transport. */
                if (!buf_append(&pt, &plen, &pcap, t->acc, t->acc_len)) {
                    r = STEP_FAIL; break;
                }
                t->acc_len = 0;
                t->state = V2_PLAINTEXT_FALLBACK;
                r = STEP_STOP;
                break;
            }
            if (t->acc_len < 32) { r = STEP_NEEDMORE; break; }
            uint8_t payload[NOISE_MAX_MESSAGE];
            size_t payload_len = 0;
            if (!noise_hs_read_message(&t->hs, t->acc, 32,
                                       payload, &payload_len)) {
                r = STEP_FAIL; break;
            }
            uint8_t msg2[NOISE_MAX_MESSAGE];
            size_t msg2_len = 0;
            if (!noise_hs_write_message(&t->hs, NULL, 0, msg2, &msg2_len)) {
                r = STEP_FAIL; break;
            }
            if (!buf_append(&wire, &wlen, &wcap, msg2, msg2_len)) {
                r = STEP_FAIL; break;
            }
            memmove(t->acc, t->acc + 32, t->acc_len - 32);
            t->acc_len -= 32;
            t->state = V2_KEY_RECV;
            r = STEP_CONSUMED;
            break;
        }
        case V2_KEY_SENT: {
            if (t->acc_len < 96) { r = STEP_NEEDMORE; break; }
            uint8_t payload[NOISE_MAX_MESSAGE];
            size_t payload_len = 0;
            if (!noise_hs_read_message(&t->hs, t->acc, 96,
                                       payload, &payload_len)) {
                r = STEP_FAIL; break;
            }
            uint8_t msg3[NOISE_MAX_MESSAGE];
            size_t msg3_len = 0;
            if (!noise_hs_write_message(&t->hs, NULL, 0, msg3, &msg3_len)) {
                r = STEP_FAIL; break;
            }
            if (!buf_append(&wire, &wlen, &wcap, msg3, msg3_len)) {
                r = STEP_FAIL; break;
            }
            if (!establish_and_flush_locked(t, &wire, &wlen, &wcap)) {
                r = STEP_FAIL; break;
            }
            memmove(t->acc, t->acc + 96, t->acc_len - 96);
            t->acc_len -= 96;
            r = STEP_CONSUMED;
            break;
        }
        case V2_KEY_RECV: {
            if (t->acc_len < 64) { r = STEP_NEEDMORE; break; }
            uint8_t payload[NOISE_MAX_MESSAGE];
            size_t payload_len = 0;
            if (!noise_hs_read_message(&t->hs, t->acc, 64,
                                       payload, &payload_len)) {
                r = STEP_FAIL; break;
            }
            if (!establish_and_flush_locked(t, &wire, &wlen, &wcap)) {
                r = STEP_FAIL; break;
            }
            memmove(t->acc, t->acc + 64, t->acc_len - 64);
            t->acc_len -= 64;
            r = STEP_CONSUMED;
            break;
        }
        case V2_ESTABLISHED: {
            if (t->acc_len < SESSION_FRAME_LEN_BYTES) { r = STEP_NEEDMORE; break; }
            size_t L = (size_t)t->acc[0] |
                       ((size_t)t->acc[1] << 8) |
                       ((size_t)t->acc[2] << 16);
            size_t frame = SESSION_FRAME_LEN_BYTES + L;
            if (L == 0 || frame > SESSION_FRAME_MAX_WIRE) {
                r = STEP_FAIL; break;
            }
            if (t->acc_len < frame) { r = STEP_NEEDMORE; break; }
            uint8_t ptbuf[SESSION_MAX_PAYLOAD];
            size_t ptlen = 0;
            enum session_channel ch = SESSION_CH_DATA;
            if (!session_transport_decrypt(&t->rec, &ch, t->acc, frame,
                                           ptbuf, &ptlen)) {
                r = STEP_FAIL; break;
            }
            if (ch != SESSION_CH_DATA) { r = STEP_FAIL; break; }
            if (!buf_append(&pt, &plen, &pcap, ptbuf, ptlen)) {
                r = STEP_FAIL; break;
            }
            t->recv_frames++;
            memmove(t->acc, t->acc + frame, t->acc_len - frame);
            t->acc_len -= frame;
            r = STEP_CONSUMED;
            break;
        }
        case V2_PLAINTEXT_FALLBACK:
            r = STEP_STOP;
            break;
        default: /* V2_FAILED */
            r = STEP_FAIL;
            break;
        }

        if (r == STEP_FAIL) { ok = false; break; }
        if (r == STEP_CONSUMED) continue;
        if (r == STEP_STOP) break;
        /* STEP_NEEDMORE */
        if (in_off >= n)
            break;                 /* remainder buffered; wait for next recv */
        /* input still available yet the pending unit needs more than acc can
         * hold (acc is full) — a frame larger than SESSION_FRAME_MAX_WIRE. */
        ok = false;
        break;
    }

    if (!ok) {
        t->state = V2_FAILED;
        free(wire);
        free(pt);
        zcl_mutex_unlock(&t->lock);
        LOG_FAIL("net", "v2_transport_feed: hard failure state=%d", (int)t->state);
    }

    *wire_out = wire;
    *wire_out_len = wlen;
    *plaintext = pt;
    *plaintext_len = plen;
    zcl_mutex_unlock(&t->lock);
    return true;
}

/* ── helpers ──────────────────────────────────────────────────────── */

bool v2_transport_is_plaintext_magic(const uint8_t *first, size_t n,
                                     const unsigned char magic[4])
{
    if (!first || !magic || n < 4)
        return false;
    return memcmp(first, magic, 4) == 0;
}

void v2_transport_free(struct v2_transport *t)
{
    if (!t)
        return;
    session_transport_cleanse(&t->rec);
    noise_hs_cleanse(&t->hs);
    memory_cleanse(t->peer_static, sizeof(t->peer_static));
    memory_cleanse(t->acc, sizeof(t->acc));
    if (t->pending) {
        memory_cleanse(t->pending, t->pending_cap);
        free(t->pending);
    }
    zcl_mutex_destroy(&t->lock);
    free(t);
}

static const char *v2_state_str(enum v2_hs_state s)
{
    switch (s) {
    case V2_DETECT:             return "detect";
    case V2_KEY_SENT:           return "key_sent";
    case V2_KEY_RECV:           return "key_recv";
    case V2_ESTABLISHED:        return "established";
    case V2_PLAINTEXT_FALLBACK: return "plaintext_fallback";
    case V2_FAILED:             return "failed";
    }
    return "unknown";
}

bool v2_transport_dump_peer(struct json_value *out, const struct v2_transport *t)
{
    if (!out)
        LOG_FAIL("net", "v2_transport_dump_peer: NULL out");
    json_set_object(out);
    if (!t) {
        json_push_kv_str(out, "mode", "plaintext");
        return true;
    }
    json_push_kv_str(out, "mode",
                     t->state == V2_ESTABLISHED ? "noise_xx" : "handshaking");
    json_push_kv_str(out, "state", v2_state_str(t->state));
    json_push_kv_bool(out, "is_initiator", t->is_initiator);
    json_push_kv_int(out, "send_frames", (int64_t)t->send_frames);
    json_push_kv_int(out, "recv_frames", (int64_t)t->recv_frames);
    return true;
}
