/* arena_present — bridge a zdogfight replay stream into the native C23
 * presentation system (M6).
 *
 * Reads one canonical replay file (as written by arena_runner:
 * "ZDOGREPL" magic, u32 LE version, u64 LE seed, u8 planes_per_team,
 * 7-byte ctl frames for every plane in index order for every tick,
 * then the trailing 2163-byte canonical final state), RE-SIMULATES the
 * match from the seed and the recorded controls, and refuses to present
 * anything whose recomputed final state differs from the trailing
 * state block (named mismatch, exit 1).
 *
 * The narrative (kill events) is derived from deterministic alive->dead
 * transitions during the re-simulation, never from a separate event
 * log, so the presentation can never diverge from the authoritative
 * replay.
 *
 * Output: one bounded renderer-neutral model document
 * (ZCL_PRESENT_MODEL_TIMELINE) encoded with zcl_present_model_encode_v1
 * — inert text, exact-root label, one CLOSE action, no callbacks, no
 * paths, no handles. The existing presentation host renders it; pixels
 * never feed back into match state.
 *
 * Defensive rules: every allocation checked, every error logged with
 * context, bounded everything.
 *
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/hex.h"
#include "presentation/model.h"
#include "sha3/sha3.h"
#include "zdogfight/zdogfight.h"

#define AP_LOG "arena_present"
#define AP_MAX_TICKS (ZDOG_TICK_LIMIT + 1u)

static int ap_fail(const char *what)
{
    fprintf(stderr, "%s: %s\n", AP_LOG, what);
    return 1;
}

static uint32_t ap_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t ap_u64le(const uint8_t *p)
{
    uint64_t v = 0;
    for (size_t i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

int main(int argc, char **argv)
{
    const char *replay_path = NULL;
    const char *out_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--replay") == 0 && i + 1 < argc)
            replay_path = argv[++i];
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
            out_path = argv[++i];
        else
            return ap_fail("usage: arena_present --replay <file> "
                           "[--out <model-wire-file>]");
    }
    if (!replay_path)
        return ap_fail("--replay is required");

    /* Bounded read: header + 36000*8*7 ctl + 2163 state < 2.1 MiB. */
    static const size_t cap =
        8 + 4 + 8 + 1 + (size_t)AP_MAX_TICKS *ZDOG_MAX_PLANES *
                            ZDOG_CTL_WIRE_LEN +
        ZDOG_STATE_WIRE_MAX;
    FILE *f = fopen(replay_path, "rb");
    if (!f)
        return ap_fail("cannot open replay file");
    uint8_t *buf = malloc(cap);
    if (!buf) {
        fclose(f);
        return ap_fail("cannot allocate the bounded replay buffer");
    }
    size_t len = fread(buf, 1, cap, f);
    int read_err = ferror(f);
    int too_big = !feof(f);
    fclose(f);
    if (read_err) {
        free(buf);
        return ap_fail("cannot read replay file");
    }
    if (too_big) {
        free(buf);
        return ap_fail("replay exceeds its bounded maximum size");
    }
    const size_t hdr = 8 + 4 + 8 + 1;
    if (len < hdr + ZDOG_STATE_WIRE_MAX ||
        memcmp(buf, "ZDOGREPL", 8) != 0) {
        free(buf);
        return ap_fail("MISMATCH replay-magic: not a zdogfight replay");
    }
    if (ap_u32le(buf + 8) != 1) {
        free(buf);
        return ap_fail("MISMATCH replay-version: unsupported version");
    }
    uint64_t seed = ap_u64le(buf + 12);
    unsigned ppt = buf[20];
    if (ppt < 1 || ppt > 4) {
        free(buf);
        return ap_fail("MISMATCH replay-header: planes-per-team out of range");
    }
    unsigned np = 2 * ppt;
    size_t frame = (size_t)np * ZDOG_CTL_WIRE_LEN;
    size_t body = len - hdr - ZDOG_STATE_WIRE_MAX;
    if (body % frame != 0) {
        free(buf);
        return ap_fail("MISMATCH replay-body: truncated control frame");
    }
    size_t ticks = body / frame;
    if (ticks > ZDOG_TICK_LIMIT) {
        free(buf);
        return ap_fail("MISMATCH replay-body: more ticks than the rules allow");
    }
    const uint8_t *tail = buf + hdr + body;

    /* Re-simulate, deriving the kill narrative from alive->dead
     * transitions. */
    zdog_match m;
    zdog_match_init(&m, seed, ppt);
    zdog_ctl *ctls = malloc(sizeof(*ctls) * ZDOG_MAX_PLANES);
    if (!ctls) {
        free(buf);
        return ap_fail("cannot allocate the control frame");
    }
    struct zcl_present_model_v1 model;
    zcl_present_model_init_v1(&model, ZCL_PRESENT_MODEL_TIMELINE);
    (void)snprintf(model.request_id, sizeof(model.request_id),
                   "arena-replay");
    (void)snprintf(model.title, sizeof(model.title), "zdogfight match");
    uint32_t kills_recorded = 0, kills_total = 0;

    uint8_t alive_before[ZDOG_MAX_PLANES];
    for (unsigned i = 0; i < np; i++)
        alive_before[i] = m.planes[i].alive;
    for (size_t t = 0; t < ticks; t++) {
        const uint8_t *fp = buf + hdr + t * frame;
        for (unsigned i = 0; i < np; i++) {
            if (!zdog_ctl_decode(fp + i * ZDOG_CTL_WIRE_LEN,
                                 ZDOG_CTL_WIRE_LEN, &ctls[i])) {
                free(ctls);
                free(buf);
                return ap_fail("MISMATCH replay-body: undecodable control");
            }
        }
        for (unsigned i = np; i < ZDOG_MAX_PLANES; i++)
            ctls[i] = (zdog_ctl){0, 0, 0, 0};
        zdog_tick(&m, ctls);
        for (unsigned i = 0; i < np; i++) {
            if (alive_before[i] && !m.planes[i].alive) {
                kills_total++;
                if (kills_recorded < ZCL_PRESENT_MODEL_ITEMS_MAX - 1u) {
                    struct zcl_present_model_item_v1 *it =
                        &model.items[model.item_count++];
                    memset(it, 0, sizeof(*it));
                    it->kind = ZCL_PRESENT_ITEM_TIMELINE_EVENT;
                    it->status = ZCL_PRESENT_STATUS_INFO;
                    it->parent_index = ZCL_PRESENT_MODEL_PARENT_NONE;
                    (void)snprintf(it->label, sizeof(it->label),
                                   "tick %llu", (unsigned long long)m.tick);
                    (void)snprintf(it->value, sizeof(it->value),
                                   "%s plane %u down (score %u-%u)",
                                   m.planes[i].team == 0 ? "red" : "blue", i,
                                   m.score[0], m.score[1]);
                    kills_recorded++;
                }
            }
            alive_before[i] = m.planes[i].alive;
        }
    }
    free(ctls);

    /* The bridge is a verifier first: refuse any replay whose trailing
     * state is not exactly the re-simulated final state. */
    uint8_t state[ZDOG_STATE_WIRE_MAX];
    size_t state_len = zdog_state_encode(&m, state, sizeof(state));
    if (state_len != ZDOG_STATE_WIRE_MAX ||
        memcmp(state, tail, ZDOG_STATE_WIRE_MAX) != 0) {
        free(buf);
        return ap_fail("MISMATCH final-state: replay does not re-simulate "
                       "to its trailing state");
    }
    if (m.phase != ZDOG_PHASE_DONE) {
        free(buf);
        return ap_fail("MISMATCH match-incomplete: replay ends before the "
                       "match is done");
    }

    uint8_t root_bin[32];
    zcl_sha3_256(state, sizeof(state), root_bin);
    char root_hex[65];
    zcl_hex_encode(root_bin, 32, root_hex);

    if (kills_total > kills_recorded) {
        struct zcl_present_model_item_v1 *it = &model.items[model.item_count++];
        memset(it, 0, sizeof(*it));
        it->kind = ZCL_PRESENT_ITEM_TEXT;
        it->status = ZCL_PRESENT_STATUS_NEUTRAL;
        it->parent_index = ZCL_PRESENT_MODEL_PARENT_NONE;
        (void)snprintf(it->label, sizeof(it->label), "…");
        (void)snprintf(it->value, sizeof(it->value),
                       "%u further events elided (bounded model)",
                       kills_total - kills_recorded);
    }
    const char *winner = m.winner == ZDOG_WINNER_RED    ? "red"
                         : m.winner == ZDOG_WINNER_BLUE ? "blue"
                                                        : "draw";
    (void)snprintf(model.summary, sizeof(model.summary),
                   "winner %s — score %u-%u in %llu ticks",
                   winner, m.score[0], m.score[1],
                   (unsigned long long)m.tick);
    (void)snprintf(model.exact_root, sizeof(model.exact_root), "%s", root_hex);
    model.actions[0].kind = ZCL_PRESENT_ACTION_CLOSE;
    (void)snprintf(model.actions[0].id, sizeof(model.actions[0].id), "close");
    (void)snprintf(model.actions[0].label,
                   sizeof(model.actions[0].label), "Close");
    model.action_count = 1;

    char merr[160];
    if (!zcl_present_model_validate_v1(&model, merr, sizeof(merr))) {
        free(buf);
        fprintf(stderr, "%s: model invalid: %s\n", AP_LOG, merr);
        return 1;
    }
    uint8_t *wire = malloc(ZCL_PRESENT_MODEL_WIRE_MAX);
    if (!wire) {
        free(buf);
        return ap_fail("cannot allocate the model wire buffer");
    }
    size_t wire_len = 0;
    if (!zcl_present_model_encode_v1(&model, wire, ZCL_PRESENT_MODEL_WIRE_MAX,
                                     &wire_len, merr, sizeof(merr))) {
        free(wire);
        free(buf);
        fprintf(stderr, "%s: model encode failed: %s\n", AP_LOG, merr);
        return 1;
    }
    FILE *of = stdout;
    if (out_path) {
        of = fopen(out_path, "wb");
        if (!of) {
            free(wire);
            free(buf);
            return ap_fail("cannot open the model output file");
        }
    }
    size_t put = fwrite(wire, 1, wire_len, of);
    if (out_path)
        fclose(of);
    free(wire);
    free(buf);
    if (put != wire_len)
        return ap_fail("cannot write the model wire");
    fprintf(stderr, "%s: events=%u winner=%s score=%u-%u ticks=%llu "
                    "final_state_root=%s wire_bytes=%zu\n",
            AP_LOG, kills_total, winner, m.score[0], m.score[1],
            (unsigned long long)m.tick, root_hex, wire_len);
    return 0;
}
