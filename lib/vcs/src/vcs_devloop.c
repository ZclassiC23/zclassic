/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * vcs_devloop — implementation. See vcs/vcs_devloop.h. */

#include "vcs/vcs_devloop.h"

#include "vcs/vcs.h"
#include "vcs/vcs_index.h"

#include "platform/time_compat.h"
#include "util/log_macros.h"

#include <stdio.h>
#include <string.h>

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool vcs_devloop_hex32_decode(const char *hex, uint8_t out[32])
{
    if (!hex || !out)
        return false;
    if (strlen(hex) != 64)
        return false;
    for (size_t i = 0; i < 32; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

void vcs_devloop_anchor_cycle(const char *repo_root,
                              const struct vcs_devloop_verdict *v,
                              struct vcs_devloop_anchor_result *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->status = VCS_DEVLOOP_ANCHOR_ERROR;

    if (!repo_root || !repo_root[0] || !v) {
        snprintf(out->error, sizeof(out->error),
                 "vcs_devloop: invalid arguments (repo_root or verdict is NULL)");
        LOG_WARN("vcs.devloop", "anchor_cycle: invalid arguments");
        return;
    }

    struct vcs_repo *r = vcs_open(repo_root);
    if (!r) {
        snprintf(out->error, sizeof(out->error),
                 "vcs_open failed for repo_root=%s", repo_root);
        LOG_WARN("vcs.devloop", "anchor_cycle: vcs_open failed root=%s",
                 repo_root);
        return;
    }

    /* First-run ergonomics: a repo with no HEAD ref yet is about to take its
     * first snapshot of the whole worktree, which is the one call in the
     * hot dev-loop path that is not O(changed files). Log the one-time
     * cost rather than staying silent about it. */
    struct vcs_index *idx = vcs_repo_index(r);
    uint8_t head_probe[32];
    bool head_found = false;
    bool first_snapshot =
        idx && vcs_index_ref_get(idx, "HEAD", head_probe, &head_found) &&
        !head_found;

    uint8_t generation[32];
    memset(generation, 0, sizeof(generation));
    bool have_generation = v->generation_hex && v->generation_hex[0] &&
                          vcs_devloop_hex32_decode(v->generation_hex, generation);
    if (v->generation_hex && v->generation_hex[0] && !have_generation)
        LOG_WARN("vcs.devloop",
                 "anchor_cycle: unparsable generation hex (binding zero): %s",
                 v->generation_hex);

    struct vcs_snapshot_meta meta = {0};
    meta.verdict_status = v->verdict_status;
    meta.phase = v->phase;
    meta.elapsed_ms = v->elapsed_ms < 0 ? 0 : (uint64_t)v->elapsed_ms;
    meta.generation_sha256 = have_generation ? generation : NULL;
    meta.agent_id = v->agent_id;
    meta.session_id = v->session_id;
    meta.task_ref = v->task_ref;

    int64_t t0 = platform_time_monotonic_us();
    uint8_t commit_id[32];
    int rc = vcs_snapshot(r, &meta, commit_id);
    int64_t t1 = platform_time_monotonic_us();

    if (first_snapshot)
        LOG_INFO("vcs.devloop",
                 "anchor_cycle: first snapshot of the working tree took %lld ms",
                 (long long)((t1 - t0) / 1000));

    vcs_close(r);

    if (rc == VCS_OK) {
        out->status = VCS_DEVLOOP_ANCHOR_OK;
        memcpy(out->commit_id, commit_id, 32);
        return;
    }
    if (rc == VCS_REFUSED) {
        out->status = VCS_DEVLOOP_ANCHOR_REFUSED;
        snprintf(out->error, sizeof(out->error),
                 "sealed-path change refused (advisory here; the dev-loop "
                 "publish already happened) — run the owner-gated unseal "
                 "ritual before the next anchor");
        LOG_WARN("vcs.devloop",
                 "anchor_cycle: sealed-path refusal (advisory; publish already happened)");
        return;
    }
    snprintf(out->error, sizeof(out->error), "vcs_snapshot failed (rc=%d)", rc);
    LOG_WARN("vcs.devloop", "anchor_cycle: vcs_snapshot failed rc=%d", rc);
}
