/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * coins_ram — implementation. See storage/coins_ram.h for the contract.
 *
 * Open-addressed linear-probe table keyed (txid[32],vout). Each slot is one
 * coin overlay entry: LIVE (an add) or TOMB (a spend that must shadow a durable
 * coins_kv row as ABSENT). Reads consult the overlay first; a MISS reads
 * through to coins_kv. Mutations stay in RAM until coins_ram_flush drains them
 * to coins_kv inside ONE BEGIN IMMEDIATE that also moves the durable cursor —
 * preserving the per-height atomic commit at flush boundaries and a
 * deterministic crash-replay of the un-flushed tail (coins_ram_reconcile_boot).
 *
 * Raw sqlite carries // raw-sql-ok:progress-kv-kernel-store — coins_kv lives on
 * the cross-thread progress.kv handle (the same hatch coins_kv.c uses). The
 * flush/read-through statements are prepared+finalized within the call.
 *
 * SINGLE-WRITER: the bulk fold runs on the reducer drive holding
 * progress_store_tx_lock; this store is not internally locked. Enable the flag
 * only for the bulk fold (mint / -refold-from-anchor catch-up).
 */
#include "storage/coins_ram.h"

#include "coins/coins.h"
#include "coins/utxo_commitment.h"
#include "crypto/sha3.h"
#include "script/script.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"
#include "storage/snapshot_shielded.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/stage.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── slot ─────────────────────────────────────────────────────────────── */

enum slot_state { SLOT_EMPTY = 0, SLOT_LIVE = 1, SLOT_TOMB = 2 };

struct ram_slot {
    uint8_t  txid[32];
    uint32_t vout;
    uint8_t  state;        /* enum slot_state */
    uint8_t  is_coinbase;  /* LIVE only */
    int32_t  height;       /* LIVE only */
    int64_t  value;        /* LIVE only */
    uint32_t script_len;   /* LIVE only */
    uint8_t *script;       /* LIVE only, heap (NULL iff script_len==0) */
};

/* Default capacity: power-of-two >= ~4M / 0.7 load. 8M slots * ~64B = ~512MB
 * for the slot array (+ heap scripts). tmpfs-resident bulk fold can afford it. */
#define COINS_RAM_DEFAULT_SLOTS (8u * 1024u * 1024u)
#define COINS_RAM_MAX_LOAD_NUM  7
#define COINS_RAM_MAX_LOAD_DEN  10
#define COINS_RAM_DEFAULT_FLUSH_EVERY 50000u

static struct {
    bool             active;
    sqlite3         *db;            /* durable coins_kv handle (read-through+flush) */
    struct ram_slot *slots;
    size_t           cap;           /* power of two */
    size_t           mask;          /* cap-1 */
    size_t           live_count;    /* LIVE slots in the overlay */
    size_t           used;          /* LIVE+TOMB (probe-length budget) */
    uint32_t         flush_every;
    uint32_t         since_flush;   /* heights applied since last flush */
    int32_t          last_applied;  /* highest height noted, -1 = none */
} G;

/* ── flag ─────────────────────────────────────────────────────────────── */

bool coins_ram_enabled(void)
{
    static int cached = -1;  /* -1 unknown, 0 off, 1 on */
    if (cached < 0) {
        const char *v = getenv("ZCL_FOLD_INRAM");
        cached = (v && v[0] && v[0] != '0') ? 1 : 0;
        if (cached)
            LOG_INFO("coins_ram",
                     "[coins_ram] ZCL_FOLD_INRAM active: bulk fold uses the "
                     "in-RAM UTXO hot store (steady-state coins_kv unchanged)");
    }
    return cached == 1;
}

bool coins_ram_active(void) { return G.active && G.slots != NULL; }

/* ── single-writer guard (the phashBlock-into-bucket UAF class) ──
 *
 * _Thread_local: each thread sees its OWN counter. The reducer fold thread
 * brackets its step with coins_ram_writer_enter/exit; coins_ram_writer_thread()
 * returns true ONLY for that thread while inside the bracket. A reader on a
 * DIFFERENT thread (bg-validation pthread, RPC pool, seal_service) sees its own
 * counter at 0 → the coins_kv.c READ shims route to the SQLite path instead of
 * touching the lock-free overlay. The overlay store is only mutated from inside
 * the writer bracket, so a true return means "the overlay is safe for THIS
 * thread to read". Counter form (not bool) so a nested/recursive enter is
 * balanced by the matching number of exits. */
static _Thread_local int t_writer_depth = 0;

void coins_ram_writer_enter(void) { t_writer_depth++; }
void coins_ram_writer_exit(void)
{
    if (t_writer_depth > 0) t_writer_depth--;
}

bool coins_ram_writer_thread(void)
{
    /* A plain load is correct: the flag is _Thread_local, so only the calling
     * thread reads/writes its own instance — no cross-thread access, no torn
     * read, no memory-ordering hazard. */
    return t_writer_depth > 0;
}

/* ── hashing / probing ───────────────────────────────────────────────── */

/* FNV-1a over txid||vout — txid is already a random hash so this is plenty. */
static size_t key_hash(const uint8_t txid[32], uint32_t vout)
{
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < 32; i++) { h ^= txid[i]; h *= 1099511628211ULL; }
    for (int i = 0; i < 4; i++) {
        h ^= (uint8_t)(vout >> (8 * i)); h *= 1099511628211ULL;
    }
    return (size_t)h;
}

static bool key_eq(const struct ram_slot *s, const uint8_t txid[32], uint32_t vout)
{
    return s->vout == vout && memcmp(s->txid, txid, 32) == 0;
}

/* Find the slot for (txid,vout): returns its index if a LIVE/TOMB slot for the
 * key exists, else the index of the first EMPTY slot where it would be inserted.
 * *found set accordingly. Linear probe — TOMBs are real slots (they shadow). */
static size_t probe(const uint8_t txid[32], uint32_t vout, bool *found)
{
    size_t i = key_hash(txid, vout) & G.mask;
    for (;;) {
        struct ram_slot *s = &G.slots[i];
        if (s->state == SLOT_EMPTY) { *found = false; return i; }
        if (key_eq(s, txid, vout))  { *found = true;  return i; }
        i = (i + 1) & G.mask;
    }
}

/* ── grow ─────────────────────────────────────────────────────────────── */

static void slot_release_script(struct ram_slot *s)
{
    if (s->script) { free(s->script); s->script = NULL; }
}

static bool grow(void)
{
    size_t new_cap = G.cap << 1;
    struct ram_slot *ns = zcl_calloc(new_cap, sizeof(*ns), "coins_ram_grow");
    if (!ns)
        LOG_FAIL("coins_ram", "grow: OOM for %zu slots", new_cap);
    size_t old_cap = G.cap, mask = new_cap - 1;
    struct ram_slot *os = G.slots;
    for (size_t i = 0; i < old_cap; i++) {
        if (os[i].state == SLOT_EMPTY) continue;
        size_t j = key_hash(os[i].txid, os[i].vout) & mask;
        while (ns[j].state != SLOT_EMPTY) j = (j + 1) & mask;
        ns[j] = os[i];  /* moves the script pointer ownership */
    }
    free(os);
    G.slots = ns;
    G.cap = new_cap;
    G.mask = mask;
    return true;
}

/* ── lifecycle ───────────────────────────────────────────────────────── */

bool coins_ram_init(struct sqlite3 *db, uint32_t flush_every_blocks)
{
    if (!coins_ram_enabled()) return true;  /* flag off: no-op */
    if (G.active) {
        G.db = db;  /* re-bind handle (idempotent) */
        return true;
    }
    if (!db)
        LOG_FAIL("coins_ram", "init: NULL db handle");

    size_t cap = COINS_RAM_DEFAULT_SLOTS;
    G.slots = zcl_calloc(cap, sizeof(*G.slots), "coins_ram_slots");
    if (!G.slots)
        LOG_FAIL("coins_ram", "init: OOM for %zu slots", cap);
    G.cap = cap;
    G.mask = cap - 1;
    G.live_count = 0;
    G.used = 0;
    G.db = db;
    G.flush_every = flush_every_blocks ? flush_every_blocks
                                       : COINS_RAM_DEFAULT_FLUSH_EVERY;
    G.since_flush = 0;
    G.last_applied = -1;
    G.active = true;
    LOG_INFO("coins_ram",
             "[coins_ram] init: %zu slots (~%zu MB), flush every %u blocks",
             cap, (cap * sizeof(*G.slots)) >> 20, G.flush_every);
    return true;
}

void coins_ram_shutdown(void)
{
    if (G.slots) {
        for (size_t i = 0; i < G.cap; i++) slot_release_script(&G.slots[i]);
        free(G.slots);
    }
    memset(&G, 0, sizeof(G));
}

/* ── mutations ───────────────────────────────────────────────────────── */

bool coins_ram_add(const uint8_t txid[32], uint32_t vout, int64_t value,
                   int32_t height, bool is_coinbase,
                   const uint8_t *script, size_t script_len)
{
    if (!coins_ram_active()) return false;
    if (!txid) return false;
    if (script_len > MAX_SCRIPT_SIZE) script_len = MAX_SCRIPT_SIZE;

    /* Grow when the probe budget (LIVE+TOMB) crosses the load factor. */
    if (G.used + 1 > (G.cap * COINS_RAM_MAX_LOAD_NUM) / COINS_RAM_MAX_LOAD_DEN) {
        if (!grow()) return false;
    }

    bool found = false;
    size_t i = probe(txid, vout, &found);
    struct ram_slot *s = &G.slots[i];

    uint8_t *new_script = NULL;
    if (script && script_len > 0) {
        new_script = zcl_malloc(script_len, "coins_ram_add_script");
        if (!new_script)
            LOG_FAIL("coins_ram", "add: OOM for script_len=%zu", script_len);
        memcpy(new_script, script, script_len);
    }

    if (!found) {                       /* fresh slot (was EMPTY) */
        memcpy(s->txid, txid, 32);
        s->vout = vout;
        G.used++;
        G.live_count++;
    } else {                            /* INSERT OR REPLACE semantics */
        slot_release_script(s);
        if (s->state == SLOT_TOMB) G.live_count++;  /* tomb→live */
        /* LIVE→LIVE: live_count unchanged */
    }
    s->state       = SLOT_LIVE;
    s->is_coinbase = is_coinbase ? 1 : 0;
    s->height      = height;
    s->value       = value;
    s->script_len  = (uint32_t)script_len;
    s->script      = new_script;
    return true;
}

bool coins_ram_spend(const uint8_t txid[32], uint32_t vout)
{
    if (!coins_ram_active()) return false;
    if (!txid) return false;

    bool found = false;
    size_t i = probe(txid, vout, &found);
    struct ram_slot *s = &G.slots[i];

    if (found && s->state == SLOT_LIVE) {
        /* The coin was created in THIS un-flushed window — delete it outright
         * so it never reaches coins_kv (matches coins_kv DELETE-of-row). It is
         * not in coins_kv, so no tombstone is needed to shadow a durable row. */
        slot_release_script(s);
        s->state = SLOT_TOMB;   /* keep the slot to preserve the probe chain;
                                 * a TOMB created here shadows nothing durable
                                 * (none existed) and is dropped at flush. */
        G.live_count--;
        return true;
    }
    if (found && s->state == SLOT_TOMB)
        return true;            /* already spent — no-op (DELETE-of-absent) */

    /* MISS: the coin (if any) lives only in durable coins_kv. Lay a TOMB so
     * reads see it ABSENT and the flush DELETEs the durable row. */
    if (G.used + 1 > (G.cap * COINS_RAM_MAX_LOAD_NUM) / COINS_RAM_MAX_LOAD_DEN) {
        if (!grow()) return false;
        i = probe(txid, vout, &found);  /* re-probe after rehash */
        s = &G.slots[i];
    }
    memcpy(s->txid, txid, 32);
    s->vout = vout;
    s->state = SLOT_TOMB;
    s->script = NULL;
    s->script_len = 0;
    G.used++;
    return true;
}

/* ── reads (overlay shadowing coins_kv) ──────────────────────────────── */

bool coins_ram_get(const uint8_t txid[32], uint32_t vout,
                   int64_t *value_out, uint8_t *script_out, size_t script_cap,
                   size_t *script_len_out)
{
    if (!coins_ram_active() || !txid) return false;
    bool found = false;
    size_t i = probe(txid, vout, &found);
    if (found) {
        const struct ram_slot *s = &G.slots[i];
        if (s->state == SLOT_TOMB) return false;  /* shadowed absent */
        if (value_out)      *value_out = s->value;
        if (script_len_out) *script_len_out = s->script_len;
        if (script_out && script_cap > 0 && s->script && s->script_len > 0) {
            size_t copy = s->script_len < script_cap ? s->script_len : script_cap;
            memcpy(script_out, s->script, copy);
        }
        return true;
    }
    /* cold miss → read through to the durable set */
    return coins_kv_get_sqlite(G.db, txid, vout, value_out, script_out,
                               script_cap, script_len_out);
}

bool coins_ram_get_prevout(const uint8_t txid[32], uint32_t vout,
                           int64_t *value_out, uint8_t *script_out,
                           size_t script_cap, size_t *script_len_out,
                           int32_t *height_out, bool *is_coinbase_out)
{
    if (!coins_ram_active() || !txid) return false;
    bool found = false;
    size_t i = probe(txid, vout, &found);
    if (found) {
        const struct ram_slot *s = &G.slots[i];
        if (s->state == SLOT_TOMB) return false;  /* shadowed absent */
        if (value_out)        *value_out = s->value;
        if (script_len_out)   *script_len_out = s->script_len;
        if (height_out)       *height_out = s->height;
        if (is_coinbase_out)  *is_coinbase_out = s->is_coinbase != 0;
        if (script_out && script_cap > 0 && s->script && s->script_len > 0) {
            size_t copy = s->script_len < script_cap ? s->script_len : script_cap;
            memcpy(script_out, s->script, copy);
        }
        return true;
    }
    /* cold miss → point read-through to the durable set */
    return coins_kv_get_prevout_sqlite(G.db, txid, vout, value_out,
                                       script_out, script_cap, script_len_out,
                                       height_out, is_coinbase_out);
}

bool coins_ram_exists(const uint8_t txid[32], uint32_t vout)
{
    if (!coins_ram_active() || !txid) return false;
    bool found = false;
    size_t i = probe(txid, vout, &found);
    if (found)
        return G.slots[i].state == SLOT_LIVE;
    return coins_kv_exists_sqlite(G.db, txid, vout);
}

bool coins_ram_get_coins(const uint8_t txid[32], struct coins *out)
{
    if (!out) return false;
    coins_init(out);
    if (!coins_ram_active() || !txid) return false;

    /* Start from the durable reconstruction (covers vouts that live only in
     * coins_kv), then overlay the RAM entries for this txid: LIVE adds/replaces
     * a vout, TOMB removes it. The result is the effective live coins for txid.
     * The bulk fold's prevout resolver only needs the unspent vouts + their
     * pre-image; this merge yields exactly that. */
    struct coins base;
    coins_init(&base);
    bool have_base = coins_kv_get_coins_sqlite(G.db, txid, &base);

    /* Determine max vout across base + any LIVE overlay rows for this txid. */
    uint32_t max_vout = 0;
    bool any = false;
    int32_t  meta_height = 0;
    bool     meta_cb = false;
    bool     meta_set = false;
    if (have_base && base.num_vout > 0) {
        max_vout = (uint32_t)(base.num_vout - 1);
        any = true;
        meta_height = base.height;
        meta_cb = base.is_coinbase;
        meta_set = true;
    }
    for (size_t i = 0; i < G.cap; i++) {
        const struct ram_slot *s = &G.slots[i];
        if (s->state != SLOT_LIVE) continue;
        if (memcmp(s->txid, txid, 32) != 0) continue;
        if (s->vout > max_vout) max_vout = s->vout;
        any = true;
        if (!meta_set) { meta_height = s->height; meta_cb = s->is_coinbase != 0;
                         meta_set = true; }
    }
    if (!any) { coins_free(&base); return false; }

    if (!coins_alloc(out, (size_t)max_vout + 1)) {
        coins_free(&base);
        return false;
    }
    out->version     = 1;
    out->height      = meta_height;
    out->is_coinbase = meta_cb;

    /* Lay down the durable base first. */
    if (have_base) {
        for (size_t v = 0; v < base.num_vout && v < out->num_vout; v++) {
            if (tx_out_is_null(&base.vout[v])) continue;
            out->vout[v] = base.vout[v];  /* shallow copy of the embedded script */
        }
    }
    /* Overlay RAM rows: LIVE writes the vout, TOMB nulls it. */
    for (size_t i = 0; i < G.cap; i++) {
        const struct ram_slot *s = &G.slots[i];
        if (s->state == SLOT_EMPTY) continue;
        if (memcmp(s->txid, txid, 32) != 0) continue;
        if (s->vout >= out->num_vout) continue;
        struct tx_out *o = &out->vout[s->vout];
        if (s->state == SLOT_TOMB) {
            tx_out_set_null(o);         /* shadow absent (value=-1 sentinel) */
            continue;
        }
        o->value = s->value;
        size_t slen = s->script_len;
        if (slen > MAX_SCRIPT_SIZE) slen = MAX_SCRIPT_SIZE;
        if (slen) memcpy(o->script_pub_key.data, s->script, slen);
        o->script_pub_key.size = slen;
    }
    coins_cleanup(out);
    coins_free(&base);
    return true;
}

/* ── aggregates ──────────────────────────────────────────────────────── */

int64_t coins_ram_count(void)
{
    if (!coins_ram_active()) return -1;
    int64_t durable = coins_kv_count_sqlite(G.db);
    if (durable < 0) return -1;
    /* effective = durable + LIVE-not-shadowing-durable - TOMBs-shadowing-durable.
     * Compute precisely by adjusting durable with each overlay slot. */
    int64_t eff = durable;
    for (size_t i = 0; i < G.cap; i++) {
        const struct ram_slot *s = &G.slots[i];
        if (s->state == SLOT_EMPTY) continue;
        bool in_durable = coins_kv_exists_sqlite(G.db, s->txid, s->vout);
        if (s->state == SLOT_LIVE && !in_durable) eff++;
        else if (s->state == SLOT_TOMB && in_durable) eff--;
    }
    return eff;
}

bool coins_ram_setinfo(int64_t *num_txs, int64_t *num_txouts,
                       int64_t *total_amount)
{
    if (!coins_ram_active()) return false;
    /* num_txs (distinct txid) over the merged set is expensive to compute
     * exactly without materialising; the bulk-fold callers of setinfo are the
     * gettxoutsetinfo RPC, which is NOT on the hot path. Materialise the
     * effective set's aggregates via the commitment iterator's data source:
     * count outputs + sum value directly. distinct-txid is approximated by the
     * durable value when no overlay rows touch new txids (the common
     * at-flush-boundary case where the overlay is empty). */
    int64_t dtx = 0, dout = 0, damt = 0;
    if (!coins_kv_setinfo_sqlite(G.db, &dtx, &dout, &damt))
        return false;
    int64_t eff_out = dout, eff_amt = damt;
    for (size_t i = 0; i < G.cap; i++) {
        const struct ram_slot *s = &G.slots[i];
        if (s->state == SLOT_EMPTY) continue;
        bool in_durable = coins_kv_exists_sqlite(G.db, s->txid, s->vout);
        if (s->state == SLOT_LIVE && !in_durable) {
            eff_out++; eff_amt += s->value;
        } else if (s->state == SLOT_TOMB && in_durable) {
            int64_t v = 0;
            if (coins_kv_get_sqlite(G.db, s->txid, s->vout, &v, NULL, 0, NULL)) {
                eff_out--; eff_amt -= v;
            }
        }
    }
    if (num_txs)      *num_txs      = dtx;        /* approx (see note) */
    if (num_txouts)   *num_txouts   = eff_out;
    if (total_amount) *total_amount = eff_amt;
    return true;
}

/* ── commitment (effective set, canonical (txid,vout) order) ─────────── */

/* A flat record for the merge-sort commitment: every live output of the
 * effective set with the fields the canonical encoder needs. */
struct comm_rec {
    uint8_t  txid[32];
    uint32_t vout;
    int64_t  value;
    int32_t  height;
    uint8_t  is_coinbase;
    uint32_t script_len;
    const uint8_t *script;   /* points into the slot (LIVE) or the heap copy */
};

static int comm_cmp(const void *a, const void *b)
{
    const struct comm_rec *x = a, *y = b;
    int c = memcmp(x->txid, y->txid, 32);
    if (c) return c;
    return (x->vout < y->vout) ? -1 : (x->vout > y->vout) ? 1 : 0;
}

/* Materialise the EFFECTIVE live set (durable coins_kv rows NOT shadowed by a
 * RAM overlay entry + every RAM LIVE row) into a freshly-malloc'd, (txid,vout)-
 * sorted array. *out_recs / *out_n receive the array + length; *out_scr_pool is
 * the heap holding durable-row scripts (RAM-LIVE scripts point into their slot,
 * stable while the single-writer fold is paused). On success returns true and
 * the caller frees *out_recs and *out_scr_pool. Shared by coins_ram_commitment
 * and coins_ram_snapshot_write so both emit the IDENTICAL canonical stream. */
static bool build_effective_sorted(struct comm_rec **out_recs, size_t *out_n,
                                    uint8_t **out_scr_pool)
{
    *out_recs = NULL; *out_n = 0; *out_scr_pool = NULL;
    sqlite3 *db = G.db;
    int64_t durable = coins_kv_count_sqlite(db);
    if (durable < 0) return false;
    size_t cap = (size_t)durable + G.live_count + 16;
    struct comm_rec *recs = zcl_malloc(cap * sizeof(*recs), "coins_ram_eff");
    if (!recs)
        LOG_FAIL("coins_ram", "effective: OOM for %zu recs", cap);
    uint8_t *scr_pool = NULL;
    size_t   scr_used = 0, scr_cap = 0;
    size_t   n = 0;

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT txid, vout, value, script, height, is_coinbase "
            "FROM coins ORDER BY txid, vout", -1, &s, NULL) != SQLITE_OK) {
        free(recs);
        LOG_FAIL("coins_ram", "effective: prepare failed: %s", sqlite3_errmsg(db));
    }

    int rc;
    bool oom = false;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {  // raw-sql-ok:progress-kv-kernel-store
        const uint8_t *txid = (const uint8_t *)sqlite3_column_blob(s, 0);
        int txid_len = sqlite3_column_bytes(s, 0);
        if (!txid || txid_len < 32) continue;
        uint32_t vout = (uint32_t)sqlite3_column_int(s, 1);

        /* Skip a durable row shadowed by a RAM overlay entry (TOMB removes it;
         * LIVE replaces it — the LIVE value is emitted from the overlay pass). */
        bool found = false;
        (void)probe(txid, vout, &found);
        if (found) continue;

        if (n >= cap) { oom = true; break; }  /* defensive — cap had slack */
        struct comm_rec *r = &recs[n];
        memcpy(r->txid, txid, 32);
        r->vout        = vout;
        r->value       = sqlite3_column_int64(s, 2);
        const uint8_t *script = (const uint8_t *)sqlite3_column_blob(s, 3);
        int slen = sqlite3_column_bytes(s, 3);
        r->height      = sqlite3_column_int(s, 4);
        r->is_coinbase = sqlite3_column_int(s, 5) ? 1 : 0;
        r->script_len  = (uint32_t)(slen > 0 ? slen : 0);
        r->script      = NULL;
        if (slen > 0 && script) {
            if (scr_used + (size_t)slen > scr_cap) {
                size_t ncap = scr_cap ? scr_cap * 2 : (1u << 20);
                while (ncap < scr_used + (size_t)slen) ncap *= 2;
                uint8_t *np = zcl_realloc(scr_pool, ncap, "coins_ram_scrpool");
                if (!np) { oom = true; break; }
                scr_pool = np; scr_cap = ncap;
                size_t off = 0;  /* re-point earlier records into the moved pool */
                for (size_t k = 0; k < n; k++) {
                    if (recs[k].script_len) {
                        recs[k].script = scr_pool + off;
                        off += recs[k].script_len;
                    }
                }
            }
            memcpy(scr_pool + scr_used, script, (size_t)slen);
            r->script = scr_pool + scr_used;
            scr_used += (size_t)slen;
        }
        n++;
    }
    sqlite3_finalize(s);
    if (oom || (rc != SQLITE_DONE && rc != SQLITE_ROW)) {
        free(recs); free(scr_pool);
        LOG_FAIL("coins_ram", "effective: durable scan failed (oom=%d rc=%d)",
                 oom, rc);
    }

    /* Add the RAM LIVE rows. */
    for (size_t i = 0; i < G.cap; i++) {
        const struct ram_slot *sl = &G.slots[i];
        if (sl->state != SLOT_LIVE) continue;
        if (n >= cap) {
            cap = n + 1024;
            struct comm_rec *nr = zcl_realloc(recs, cap * sizeof(*recs),
                                              "coins_ram_eff_grow");
            if (!nr) { free(recs); free(scr_pool);
                LOG_FAIL("coins_ram", "effective: OOM growing recs"); }
            recs = nr;
        }
        struct comm_rec *r = &recs[n++];
        memcpy(r->txid, sl->txid, 32);
        r->vout        = sl->vout;
        r->value       = sl->value;
        r->height      = sl->height;
        r->is_coinbase = sl->is_coinbase;
        r->script_len  = sl->script_len;
        r->script      = sl->script;
    }

    qsort(recs, n, sizeof(*recs), comm_cmp);
    *out_recs = recs; *out_n = n; *out_scr_pool = scr_pool;
    return true;
}

int coins_ram_commitment(uint8_t out[32])
{
    if (!coins_ram_active() || !out) return -1;
    struct comm_rec *recs = NULL; size_t n = 0; uint8_t *scr_pool = NULL;
    if (!build_effective_sorted(&recs, &n, &scr_pool))
        return -1;

    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    for (size_t i = 0; i < n; i++) {
        const struct comm_rec *r = &recs[i];
        utxo_commitment_sha3_write_record(&ctx, r->txid, r->vout, r->value,
                                          r->script_len ? r->script : NULL,
                                          r->script_len,
                                          (uint32_t)r->height, r->is_coinbase);
    }
    sha3_256_finalize(&ctx, out);
    free(recs);
    free(scr_pool);
    return 0;
}

/* Stream the EFFECTIVE set to a ZCLUTXO snapshot in the exact format
 * coins_kv_snapshot_write produces. Used when the in-RAM overlay is active so
 * the anchor self-mint (which reads coins_kv via SQL inside the stage txn,
 * BEFORE the post-commit flush drains the overlay) sees the COMPLETE applied
 * set and its body SHA3 still equals the compiled checkpoint. The body encoder
 * is utxo_sha3_serialize_record — byte-identical to coins_kv_snapshot_write and
 * coins_ram_commitment. */
/* ONE canonical overlay-active writer, versioned by DATA exactly like
 * coins_kv_snapshot_write (see storage/coins_kv.h): shielded_or_null == NULL =>
 * v1 (coins only); a Sapling-only frontier (sprout_len == 0 && nf_count == 0,
 * sapling_len > 0) => v2 (single Sapling-frontier section); a Sprout frontier
 * and/or nullifier set => v3 (full storage/snapshot_shielded.h section). Emits
 * the byte-identical stream coins_kv_snapshot_write would for the same set. */
bool coins_ram_snapshot_write(const char *out_path, int32_t height,
                              const uint8_t anchor_block_hash[32],
                              const struct snapshot_shielded *shielded_or_null,
                              uint8_t out_sha3[32], uint64_t *out_count,
                              int64_t *out_total_supply)
{
    if (!coins_ram_active() || !out_path || !out_path[0])
        LOG_FAIL("coins_ram", "snapshot_write: inactive or null path");

    /* Format version from the shielded content: NULL => v1; Sapling-only => v2;
     * Sprout and/or nullifiers => v3. */
    uint32_t snap_version = 1;
    if (shielded_or_null) {
        if (shielded_or_null->sprout_len > 0 || shielded_or_null->nf_count > 0)
            snap_version = 3;
        else if (shielded_or_null->sapling_len > 0)
            snap_version = 2;
    }

    struct comm_rec *recs = NULL; size_t n = 0; uint8_t *scr_pool = NULL;
    if (!build_effective_sorted(&recs, &n, &scr_pool))
        return false;

    char tmp_path[1100];
    int np = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", out_path);
    if (np < 0 || (size_t)np >= sizeof(tmp_path)) {
        free(recs); free(scr_pool);
        LOG_FAIL("coins_ram", "snapshot_write: path too long");
    }
    FILE *out = fopen(tmp_path, "wb");
    if (!out) { free(recs); free(scr_pool);
        LOG_FAIL("coins_ram", "snapshot_write: fopen(%s) failed", tmp_path); }

    /* 104-byte header reserved, rewritten at the end (same layout as
     * coins_kv_snapshot_write). */
    uint8_t header[104] = {0};
    bool ok = fwrite(header, 1, sizeof(header), out) == sizeof(header);

    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    uint64_t count = 0;
    int64_t  total_supply = 0;
    for (size_t i = 0; ok && i < n; i++) {
        const struct comm_rec *r = &recs[i];
        uint8_t inline_buf[UTXO_SHA3_RECORD_MAX(1024)];
        uint8_t *rec = inline_buf, *heap = NULL;
        size_t bcap = sizeof(inline_buf);
        if (r->script_len > 1024) {
            bcap = UTXO_SHA3_RECORD_MAX(r->script_len);
            heap = zcl_malloc(bcap, "coins_ram_snap_rec");
            if (!heap) { ok = false; break; }
            rec = heap;
        }
        size_t rec_len = 0;
        if (!utxo_sha3_serialize_record(rec, bcap, &rec_len, r->txid, r->vout,
                                        r->value,
                                        r->script_len ? r->script : NULL,
                                        r->script_len, (uint32_t)r->height,
                                        r->is_coinbase)) {
            if (heap) free(heap);
            ok = false;
            break;
        }
        if (fwrite(rec, 1, rec_len, out) != rec_len) {
            if (heap) free(heap);
            ok = false;
            break;
        }
        sha3_256_write(&ctx, rec, rec_len);
        if (heap) free(heap);
        total_supply += r->value;
        count++;
    }
    free(recs); free(scr_pool);

    if (!ok) { fclose(out); unlink(tmp_path);
        LOG_FAIL("coins_ram", "snapshot_write: body write failed"); }

    /* OPTIONAL shielded section, appended after the UTXO records and inside the
     * body SHA3 region. v2 => single Sapling frontier ([u32 sapling_len][blob],
     * byte-identical to the historical layout); v3 => full shielded section. */
    if (snap_version == 2) {
        const struct snapshot_shielded *sh = shielded_or_null;
        uint8_t lenbuf[4];
        for (int i = 0; i < 4; i++)
            lenbuf[i] = (uint8_t)(sh->sapling_len >> (8 * i));
        if (fwrite(lenbuf, 1, 4, out) != 4 ||
            (sh->sapling_len &&
             fwrite(sh->sapling, 1, sh->sapling_len, out) != sh->sapling_len)) {
            fclose(out); unlink(tmp_path);
            LOG_FAIL("coins_ram", "snapshot_write: sapling frontier write failed");
        }
        sha3_256_write(&ctx, lenbuf, 4);
        if (sh->sapling_len)
            sha3_256_write(&ctx, sh->sapling, sh->sapling_len);
    } else if (snap_version == 3) {
        if (!snapshot_shielded_write(out, &ctx, shielded_or_null)) {
            fclose(out); unlink(tmp_path);
            LOG_FAIL("coins_ram", "snapshot_write: shielded section failed");
        }
    }

    uint8_t body_sha3[32];
    sha3_256_finalize(&ctx, body_sha3);

    memcpy(header, "ZCLUTXO\x00", 8);
    /* version u32 at off 8 (1/2/3); height u32 at off 16; count u64 at off 24;
     * total_supply i64 at off 32; anchor hash at off 40; sha3 at off 72. */
    header[8] = (uint8_t)snap_version;
    header[16] = (uint8_t)height; header[17] = (uint8_t)(height >> 8);
    header[18] = (uint8_t)(height >> 16); header[19] = (uint8_t)(height >> 24);
    for (int i = 0; i < 8; i++) header[24 + i] = (uint8_t)(count >> (8 * i));
    for (int i = 0; i < 8; i++)
        header[32 + i] = (uint8_t)((uint64_t)total_supply >> (8 * i));
    if (anchor_block_hash) memcpy(header + 40, anchor_block_hash, 32);
    memcpy(header + 72, body_sha3, 32);

    if (fseek(out, 0, SEEK_SET) != 0 ||
        fwrite(header, 1, sizeof(header), out) != sizeof(header) ||
        fflush(out) != 0) {
        fclose(out); unlink(tmp_path);
        LOG_FAIL("coins_ram", "snapshot_write: header rewrite failed");
    }
    fclose(out);
    if (rename(tmp_path, out_path) != 0) {
        unlink(tmp_path);
        LOG_FAIL("coins_ram", "snapshot_write: rename failed");
    }
    if (out_sha3) memcpy(out_sha3, body_sha3, 32);
    if (out_count) *out_count = count;
    if (out_total_supply) *out_total_supply = total_supply;
    return true;
}

/* ── flush ───────────────────────────────────────────────────────────── */

static bool coins_ram_table_exists(sqlite3 *db, const char *name, bool *exists)
{
    if (exists)
        *exists = false;
    if (!db || !name)
        return false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("coins_ram", "flush witness table probe prepare failed: %s",
                 sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (exists)
        *exists = (rc == SQLITE_ROW);
    sqlite3_finalize(st);
    return rc == SQLITE_ROW || rc == SQLITE_DONE;
}

static bool coins_ram_delete_tail_if_table_exists(sqlite3 *db,
                                                  const char *table,
                                                  int64_t first_height)
{
    bool exists = false;
    if (!coins_ram_table_exists(db, table, &exists))
        return false;
    if (!exists)
        return true;

    char sql[128];
    snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE height >= ?1", table);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("coins_ram", "reconcile_boot: tail purge prepare failed "
                 "table=%s: %s", table, sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)first_height);
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        LOG_WARN("coins_ram", "reconcile_boot: tail purge failed table=%s "
                 "first_height=%lld rc=%d",
                 table, (long long)first_height, rc);
        return false;
    }
    return true;
}

static bool coins_ram_purge_replay_tail(sqlite3 *db, int64_t want_cursor)
{
    static const char *const replay_tail_tables[] = {
        "coins",
        "utxo_apply_log",
        "utxo_apply_delta",
        "nullifiers",
        "sprout_anchors",
        "sapling_anchors",
    };
    for (size_t i = 0; i < sizeof(replay_tail_tables) /
                            sizeof(replay_tail_tables[0]); i++) {
        if (!coins_ram_delete_tail_if_table_exists(db, replay_tail_tables[i],
                                                   want_cursor))
            return false;
    }

    int64_t tip_cursor = want_cursor > 0 ? want_cursor - 1 : 0;
    if (!coins_ram_delete_tail_if_table_exists(db, "tip_finalize_log",
                                               tip_cursor))
        return false;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO stage_cursor (name, cursor, updated_at) "
            "VALUES ('tip_finalize', ?1, strftime('%s','now')) "
            "ON CONFLICT(name) DO UPDATE SET "
            "  cursor=CASE WHEN cursor > excluded.cursor "
            "              THEN excluded.cursor ELSE cursor END,"
            "  updated_at=excluded.updated_at",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("coins_ram", "reconcile_boot: tip cursor clamp prepare "
                 "failed: %s", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)tip_cursor);
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        LOG_WARN("coins_ram", "reconcile_boot: tip cursor clamp failed rc=%d",
                 rc);
        return false;
    }
    return true;
}

static bool coins_ram_utxo_log_ok_at(sqlite3 *db, int32_t height,
                                     bool *ok_at_height)
{
    if (ok_at_height)
        *ok_at_height = false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT ok FROM utxo_apply_log WHERE height=?1",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("coins_ram", "flush witness log probe prepare failed: %s",
                 sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)height);
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW && ok_at_height)
        *ok_at_height = sqlite3_column_int(st, 0) == 1;
    sqlite3_finalize(st);
    return rc == SQLITE_ROW || rc == SQLITE_DONE;
}

static bool coins_ram_delta_exists_at(sqlite3 *db, int32_t height,
                                      bool *exists_at_height)
{
    if (exists_at_height)
        *exists_at_height = false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM utxo_apply_delta WHERE height=?1",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("coins_ram", "flush witness delta probe prepare failed: %s",
                 sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)height);
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (exists_at_height)
        *exists_at_height = (rc == SQLITE_ROW);
    sqlite3_finalize(st);
    return rc == SQLITE_ROW || rc == SQLITE_DONE;
}

static bool coins_ram_flush_has_reducer_witness(sqlite3 *db,
                                                int32_t flushed_height)
{
    if (flushed_height < 0)
        return true;

    bool has_log = false;
    if (!coins_ram_table_exists(db, "utxo_apply_log", &has_log))
        return false;
    if (has_log) {
        bool ok_at_height = false;
        if (!coins_ram_utxo_log_ok_at(db, flushed_height, &ok_at_height))
            return false;
        if (!ok_at_height) {
            LOG_WARN("coins_ram",
                     "flush witness refused: missing ok=1 utxo_apply_log "
                     "height=%d",
                     flushed_height);
            return false;
        }
    }

    bool has_delta = false;
    if (!coins_ram_table_exists(db, "utxo_apply_delta", &has_delta))
        return false;
    if (has_delta) {
        bool delta_at_height = false;
        if (!coins_ram_delta_exists_at(db, flushed_height, &delta_at_height))
            return false;
        if (!delta_at_height) {
            LOG_WARN("coins_ram",
                     "flush witness refused: missing utxo_apply_delta "
                     "height=%d",
                     flushed_height);
            return false;
        }
    }
    return true;
}

/* Drain the overlay into coins_kv (adds via coins_kv_add_many, spends via
 * coins_kv_spend_many) and advance the durable cursor — ALL inside ONE
 * BEGIN IMMEDIATE on the progress.kv handle. Mirrors apply_coins_kv's
 * adds-before-spends ordering at the batch granularity: a coin created and
 * spent within the un-flushed window never reaches coins_kv (the spend
 * deleted the LIVE slot), so the overlay contains only net-live LIVE rows and
 * TOMBs that shadow durable rows — adds and spends here touch disjoint keys,
 * so ordering between the two phases is immaterial, but we keep adds-first to
 * mirror the per-block contract. */
bool coins_ram_flush(int32_t flushed_height)
{
    if (!coins_ram_active()) return false;
    if (stage_batch_active())
        LOG_FAIL("coins_ram",
                 "flush: stage batch is active at height=%d; defer until "
                 "after stage_batch_end",
                 flushed_height);
    sqlite3 *db = G.db;
    if (!coins_ram_flush_has_reducer_witness(db, flushed_height))
        LOG_FAIL("coins_ram",
                 "flush: reducer witness missing at height=%d",
                 flushed_height);

    /* Gather LIVE adds and TOMB spends into row arrays. */
    size_t nadd = G.live_count, nspend = 0;
    for (size_t i = 0; i < G.cap; i++)
        if (G.slots[i].state == SLOT_TOMB) nspend++;

    struct coins_kv_add_row   *adds   = NULL;
    struct coins_kv_spend_row *spends = NULL;
    if (nadd) {
        adds = zcl_malloc(nadd * sizeof(*adds), "coins_ram_flush_adds");
        if (!adds) LOG_FAIL("coins_ram", "flush: OOM adds");
    }
    if (nspend) {
        spends = zcl_malloc(nspend * sizeof(*spends), "coins_ram_flush_spends");
        if (!spends) { free(adds); LOG_FAIL("coins_ram", "flush: OOM spends"); }
    }
    size_t ai = 0, si = 0;
    for (size_t i = 0; i < G.cap; i++) {
        const struct ram_slot *s = &G.slots[i];
        if (s->state == SLOT_LIVE) {
            adds[ai].txid        = s->txid;
            adds[ai].vout        = s->vout;
            adds[ai].value       = s->value;
            adds[ai].height      = s->height;
            adds[ai].is_coinbase = s->is_coinbase != 0;
            adds[ai].script      = s->script_len ? s->script : NULL;
            adds[ai].script_len  = s->script_len;
            ai++;
        } else if (s->state == SLOT_TOMB) {
            spends[si].txid = s->txid;
            spends[si].vout = s->vout;
            si++;
        }
    }

    progress_store_tx_lock();
    char *err = NULL;
    bool ok = true;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("coins_ram", "flush: BEGIN failed: %s", err ? err : "?");
        ok = false;
    }
    /* Drain through the RAW SQLite variants — the public _many shims would
     * re-enter the overlay (we are inside the overlay's own flush). TOMBs first
     * would also work (disjoint keys), but mirror the per-block adds-before-
     * spends contract. */
    if (ok && nadd && !coins_kv_add_many_sqlite(db, adds, nadd))   ok = false;
    if (ok && nspend && !coins_kv_spend_many_sqlite(db, spends, nspend)) ok = false;

    /* Co-write the durable cursor + frontier + flush watermark inside THIS txn
     * so coins_kv, the utxo_apply stage cursor, coins_applied_height, and the
     * crash-replay watermark all reflect the SAME flushed height atomically. */
    if (ok) {
        uint8_t wm[8];
        for (int i = 0; i < 8; i++) wm[i] = (uint8_t)(((uint64_t)(uint32_t)flushed_height) >> (8 * i));
        if (!progress_meta_set_in_tx(db, COINS_RAM_FLUSHED_HEIGHT_KEY, wm, 8))
            ok = false;
    }
    if (ok && !coins_kv_set_applied_height_in_tx(db, flushed_height + 1))
        ok = false;
    if (ok) {
        /* Move the utxo_apply stage cursor to flushed_height+1 (next to apply)
         * so a resume after a clean flush picks up exactly above the flush. */
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO stage_cursor (name, cursor, updated_at) "
            "VALUES ('utxo_apply', ?1, strftime('%s','now')) "
            "ON CONFLICT(name) DO UPDATE SET cursor=excluded.cursor, "
            "updated_at=excluded.updated_at", -1, &st, NULL) != SQLITE_OK) {
            ok = false;
        } else {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)(flushed_height + 1));
            if (sqlite3_step(st) != SQLITE_DONE) ok = false;  // raw-sql-ok:progress-kv-kernel-store
            sqlite3_finalize(st);
        }
    }

    if (ok && sqlite3_exec(db, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN("coins_ram", "flush: COMMIT failed: %s", err ? err : "?");
        ok = false;
    }
    if (!ok) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        progress_store_tx_unlock();
        if (err) sqlite3_free(err);
        free(adds); free(spends);
        LOG_FAIL("coins_ram", "flush: failed at height=%d (rolled back)",
                 flushed_height);
    }
    if (err) sqlite3_free(err);
    progress_store_tx_unlock();
    free(adds); free(spends);

    /* The overlay is now durable in coins_kv — clear it (reads fall through to
     * coins_kv, which is now the truth for these keys). */
    for (size_t i = 0; i < G.cap; i++) {
        slot_release_script(&G.slots[i]);
        G.slots[i].state = SLOT_EMPTY;
    }
    G.live_count = 0;
    G.used = 0;
    G.since_flush = 0;
    LOG_INFO("coins_ram", "[coins_ram] flushed through height=%d "
             "(%zu adds, %zu spends)", flushed_height, nadd, nspend);
    return true;
}

bool coins_ram_flush_due(void)
{
    if (!coins_ram_active()) return true;
    if (G.last_applied < 0 || G.since_flush < G.flush_every)
        return true;
    return coins_ram_flush(G.last_applied);
}

bool coins_ram_note_applied(int32_t height)
{
    if (!coins_ram_active()) return true;
    G.last_applied = height;
    G.since_flush++;
    if (G.since_flush >= G.flush_every) {
        if (stage_batch_active())
            return true;
        return coins_ram_flush(height);
    }
    return true;
}

bool coins_ram_flush_final(void)
{
    if (!coins_ram_active()) return true;
    if (G.since_flush == 0 || G.last_applied < 0)
        return true;  /* nothing applied since the last flush */
    return coins_ram_flush(G.last_applied);
}

/* ── boot crash-replay reconcile ─────────────────────────────────────── */

static bool coins_ram_boot_replay_marker_present(sqlite3 *db, bool *present)
{
    if (present)
        *present = false;
    if (!db)
        return false;

    uint8_t buf[64] = {0};
    size_t len = 0;
    bool found = false;
    if (!progress_meta_get(db, "mint_anchor_in_progress_v1",
                           buf, sizeof(buf), &len, &found))
        return false;
    if (found) {
        if (present)
            *present = true;
        return true;
    }
    len = 0;
    found = false;
    if (!progress_meta_get(db, "refold_in_progress",
                           buf, sizeof(buf), &len, &found))
        return false;
    if (present)
        *present = found;
    return true;
}

bool coins_ram_reconcile_boot(struct sqlite3 *db)
{
    if (!coins_ram_enabled()) return true;  /* flag off: nothing to reconcile */
    if (!db)
        LOG_FAIL("coins_ram", "reconcile_boot: NULL db");

    /* Read the durable flush watermark. Absent → this datadir was never
     * bulk-folded in RAM mode; nothing to rewind. */
    uint8_t wm[8] = {0};
    size_t wlen = 0;
    bool wfound = false;
    if (!progress_meta_get(db, COINS_RAM_FLUSHED_HEIGHT_KEY, wm, sizeof(wm),
                           &wlen, &wfound))
        LOG_FAIL("coins_ram", "reconcile_boot: watermark read failed");
    int32_t watermark = -1;
    bool replay_marker = false;
    if (!coins_ram_boot_replay_marker_present(db, &replay_marker))
        LOG_FAIL("coins_ram", "reconcile_boot: replay marker read failed");
    if (!wfound) {
        if (!replay_marker)
            return true;
    } else if (wlen != 8) {
        LOG_WARN("coins_ram", "reconcile_boot: watermark malformed (len=%zu)",
                 wlen);
        return false;
    } else {
        uint64_t u = 0;
        for (int i = 0; i < 8; i++) u |= (uint64_t)wm[i] << (8 * i);
        watermark = (int32_t)u;
    }

    /* Read the persisted utxo_apply cursor. */
    progress_store_tx_lock();
    sqlite3_stmt *st = NULL;
    int64_t cursor = -1;
    if (sqlite3_prepare_v2(db,
            "SELECT cursor FROM stage_cursor WHERE name='utxo_apply'",
            -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW)  // raw-sql-ok:progress-kv-kernel-store
            cursor = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    } else {
        progress_store_tx_unlock();
        LOG_FAIL("coins_ram", "reconcile_boot: cursor read prepare failed");
    }
    progress_store_tx_unlock();

    /* Cursor below the durable watermark (+1) → coins_kv already holds more
     * than the cursor claims. Do not purge durable log evidence in that
     * inconsistent-but-conservative shape; another reconcile owns cursor lift. */
    int64_t want_cursor = (int64_t)watermark + 1;
    if (cursor < 0 || cursor < want_cursor)
        return true;

    bool rewind_cursor = cursor > want_cursor;
    bool purge_equal_tail = replay_marker && cursor == want_cursor;
    if (!rewind_cursor && !purge_equal_tail)
        return true;

    /* Crash between flushes: the cursor advanced past the last durable flush,
     * or a mint/refold marker says a prior crash/stop left replay-domain rows
     * at the exact replay cursor. Rewind cursor + coins_applied_height when
     * needed, and purge downstream rows at/above the replay point: those rows
     * were written before the overlay flushed, so after process death they no
     * longer describe durable coins. This includes durable `coins` rows whose
     * creation height is at/above the replay cursor: keeping them makes the
     * replayed block collide with its own stale output. Keeping log/finalize
     * rows also leaves tip_finalize ahead of the coin frontier and can trap
     * mint/refold in an unwind/replay loop. */
    progress_store_tx_lock();
    char *err = NULL;
    bool ok = true;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) ok = false;
    if (ok && rewind_cursor) {
        sqlite3_stmt *u2 = NULL;
        if (sqlite3_prepare_v2(db,
            "UPDATE stage_cursor SET cursor=?1, updated_at=strftime('%s','now') "
            "WHERE name='utxo_apply'", -1, &u2, NULL) != SQLITE_OK) {
            ok = false;
        } else {
            sqlite3_bind_int64(u2, 1, want_cursor);
            if (sqlite3_step(u2) != SQLITE_DONE) ok = false;  // raw-sql-ok:progress-kv-kernel-store
            sqlite3_finalize(u2);
        }
    }
    if (ok && rewind_cursor &&
        !coins_kv_set_applied_height_in_tx(db, (int32_t)want_cursor))
        ok = false;
    if (ok && !coins_ram_purge_replay_tail(db, want_cursor))
        ok = false;
    if (ok && sqlite3_exec(db, "COMMIT", NULL, NULL, &err) != SQLITE_OK) ok = false;
    if (!ok) sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    if (err) sqlite3_free(err);
    progress_store_tx_unlock();
    if (!ok)
        LOG_FAIL("coins_ram", "reconcile_boot: rewind commit failed");

    if (rewind_cursor) {
        LOG_INFO("coins_ram",
                 "[coins_ram] crash-replay: rewound utxo_apply cursor %lld -> "
                 "%lld (durable flush watermark=%d); the fold re-applies the "
                 "tail",
                 (long long)cursor, (long long)want_cursor, watermark);
    } else {
        LOG_INFO("coins_ram",
                 "[coins_ram] crash-replay: purged stale replay tail at "
                 "utxo_apply cursor %lld (durable flush watermark=%d)",
                 (long long)want_cursor, watermark);
    }
    return true;
}
