/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Store controller — proof-of-work order gate. Split out of
 * store_controller.c to stay under the app/ file-size ceiling (E1);
 * same shape (controller), same translation unit's worth of logic —
 * store_handle_request() in store_controller.c calls
 * store_pow_verify_and_claim() declared in store_controller_internal.h.
 *
 * ── Proof-of-work order gate ─────────────────────────────
 *
 * CSRF (store_controller.c) stops a tricked BROWSER from submitting an
 * order the visitor never intended — it does nothing against a direct
 * attacker with their own client, who can GET a product page, copy its
 * CSRF token, and flood POST /store/orders (or /store/buy/:id): each
 * hit mints a real Sapling z-address (real CPU) and writes an unbounded
 * `orders` row (real disk), all unauthenticated. This gate adds a
 * hashcash-style client puzzle ON TOP of CSRF (CSRF stays the floor,
 * unchanged in store_controller.c): the client must find a nonce such
 * that SHA3-256(peer_id || timestamp || nonce) has FAST_SYNC_POW_BITS
 * leading zero bits — reusing fast_sync_verify_pow, the same primitive
 * lib/net/src/fast_sync.c uses to gate snapshot-sync requests (this
 * repo's other PoW-gate work, wf/file-service-pow-gate, had landed no
 * commits beyond origin/main as of this branch, so there was no richer
 * puzzle-issuance infra to build on — fast_sync_verify_pow is used
 * directly). `peer_id` here is SHA3-256("store:order:pow:<product_id>"),
 * binding a solved puzzle to one product so it cannot be replayed
 * against another. An honest buyer solves ONE puzzle (~0.5s on a
 * native solver; see the browser JS solver in app/views/src/store_view.c
 * for the human-facing path) per order; a flood pays that CPU cost on
 * every attempt instead of getting mint+DB-write for free.
 *
 * NOTE on client identity: this HTTP surface is Tor-onion-only (see
 * lib/net/src/onion_service.c / tor_integration.c — the dynhost bridge
 * carries method/path/body only, no circuit or IP identity reaches the
 * app layer by design), so there is no per-IP axis to cap on. The bound
 * that IS available and meaningful is per-product (see the pending-order
 * caps in store_controller.c) plus the existing global 100 req/s
 * listener-wide limiter in onion_service.c. */

#include "controllers/store_controller_internal.h"

static void store_pow_bind_product(int64_t product_id, uint8_t out[32])
{
    char ctx[64];
    snprintf(ctx, sizeof(ctx), "store:order:pow:%lld", (long long)product_id);
    sha3_256((const unsigned char *)ctx, strlen(ctx), out);
}

/* Public: the product-bound puzzle commitment, hex-encoded, for the view
 * layer to embed in the order form (the client hashes SHA3-256(peer ||
 * ts || nonce) itself — see the JS solver in app/views/src/store_view.c —
 * so it needs this same 32-byte commitment, not just the product id).
 * Mirrors store_csrf_token/store_csrf_context's split: security-relevant
 * derivation stays in the controller, the view only embeds the result. */
void store_pow_challenge(int64_t product_id, char peer_id_hex[65])
{
    uint8_t bind[32];
    static const char hex[] = "0123456789abcdef";

    store_pow_bind_product(product_id, bind);
    for (size_t i = 0; i < 32; i++) {
        peer_id_hex[i * 2]     = hex[(bind[i] >> 4) & 0x0f];
        peer_id_hex[i * 2 + 1] = hex[bind[i] & 0x0f];
    }
    peer_id_hex[64] = '\0';
}

/* Bounded ring of recently-accepted (product, timestamp, nonce) solutions.
 * fast_sync_verify_pow is a pure function — it has no memory of which
 * nonces it already accepted — so without this, ONE solved puzzle could
 * be replayed for unlimited orders within its ~5-minute validity window,
 * defeating "a flood pays CPU per attempt". Fixed-size, never malloc'd;
 * process-local (does not need to survive a restart — the caps in
 * store_controller.c still bound the worst case even if the ring is
 * empty right after one). */
#define STORE_POW_USED_RING 4096
static uint8_t s_pow_used_key[STORE_POW_USED_RING][32];
static int64_t s_pow_used_at[STORE_POW_USED_RING];
static size_t s_pow_used_next = 0;
static pthread_mutex_t s_pow_used_lock = PTHREAD_MUTEX_INITIALIZER;

/* A hair over fast_sync_verify_pow's own [-300s,+60s] timestamp
 * acceptance window, so no solution that could still verify has already
 * aged out of the replay ring. */
#define STORE_POW_USED_TTL_SECS 400

static bool store_pow_claim_once(const struct fast_sync_pow *pow)
{
    uint8_t key[32];
    sha3_256((const unsigned char *)pow, sizeof(*pow), key);
    int64_t now = (int64_t)platform_time_wall_time_t();
    bool replay = false;

    pthread_mutex_lock(&s_pow_used_lock);
    for (size_t i = 0; i < STORE_POW_USED_RING; i++) {
        if (now - s_pow_used_at[i] < STORE_POW_USED_TTL_SECS &&
            memcmp(s_pow_used_key[i], key, 32) == 0) {
            replay = true;
            break;
        }
    }
    if (!replay) {
        memcpy(s_pow_used_key[s_pow_used_next], key, 32);
        s_pow_used_at[s_pow_used_next] = now;
        s_pow_used_next = (s_pow_used_next + 1) % STORE_POW_USED_RING;
    }
    pthread_mutex_unlock(&s_pow_used_lock);
    return !replay;
}

/* Verify + claim in one step. Returns true only for a solution that (a)
 * parses, (b) hashes correctly and is fresh (fast_sync_verify_pow), and
 * (c) has not been used before. Never claims a ring slot on a failed
 * verify — only a genuinely solved puzzle costs one, so a flood of
 * unsolved guesses cannot exhaust the ring and lock out real buyers.
 * Declared in store_controller_internal.h; called from
 * store_handle_request() in store_controller.c. */
bool store_pow_verify_and_claim(int64_t product_id,
                                const char *pow_ts_str,
                                const char *pow_nonce_str)
{
    struct fast_sync_pow pow;
    char *end = NULL;
    long long ts;
    unsigned long long nonce;

    if (!pow_ts_str || !pow_ts_str[0] || !pow_nonce_str || !pow_nonce_str[0])
        return false; // raw-return-ok:missing-puzzle-fields-refused-not-a-server-error

    ts = strtoll(pow_ts_str, &end, 10);
    if (!end || *end != '\0')
        return false; // raw-return-ok:malformed-client-field-not-a-server-error
    end = NULL;
    nonce = strtoull(pow_nonce_str, &end, 10);
    if (!end || *end != '\0')
        return false; // raw-return-ok:malformed-client-field-not-a-server-error

    memset(&pow, 0, sizeof(pow));
    store_pow_bind_product(product_id, pow.peer_id);
    pow.timestamp = (int64_t)ts;
    pow.nonce = (uint64_t)nonce;

    if (!fast_sync_verify_pow(&pow))
        return false; // raw-return-ok:unsolved-or-stale-puzzle-not-a-server-error
    return store_pow_claim_once(&pow);
}
