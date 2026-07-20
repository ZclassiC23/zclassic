/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Body torn-read repair note + quarantine (lane E3). Contract + rationale:
 * jobs/reducer_frontier.h. When stage_repair_read_active_block_checked
 * (reducer_frontier_replay.c) cannot read the canonical body for a HAVE_DATA
 * height — the on-disk bytes are torn (pread failed) or read fine but hash to
 * the WRONG block — the read used to only DEFER, wedging every downstream
 * stage forever. It now records the failing height HERE (event-driven; no new
 * scan) so the have_data_unreadable Condition clears BLOCK_HAVE_DATA OFF-LOCK
 * and body_fetch re-downloads the body. Deliberately does NOT clear HAVE_DATA
 * from inside the progress-locked replay (a side-channel write racing the
 * reducer's single writer). Single-slot, lowest-height-first (mirrors the
 * reducer's re-derive-lowest-hole discipline); lock-free atomics so the note is
 * safe to record whether or not the caller holds progress_store_tx_lock. */

#include "jobs/reducer_frontier.h"

#include "util/blocker.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

static _Atomic int64_t g_body_read_fail_height = -1;
static _Atomic int     g_body_read_fail_file   = -1;
static _Atomic int64_t g_body_read_fail_pos    = 0;
static _Atomic int     g_body_read_fail_count  = 0;
static _Atomic int     g_body_read_fail_reason = REDUCER_FRONTIER_BODY_READ_OK;

const char *reducer_frontier_body_read_reason_name(
    enum reducer_frontier_body_read_reason r)
{
    switch (r) {
    case REDUCER_FRONTIER_BODY_READ_DISK:  return "disk_read_failed";
    case REDUCER_FRONTIER_BODY_READ_WRONG: return "wrong_block";
    case REDUCER_FRONTIER_BODY_READ_OK:    break;
    }
    return "none";
}

int64_t reducer_frontier_body_read_note_height(void)
{
    return atomic_load(&g_body_read_fail_height);
}

bool reducer_frontier_body_read_note_active(void)
{
    return atomic_load(&g_body_read_fail_height) >= 0;
}

int reducer_frontier_body_read_note_file(void)
{
    return atomic_load(&g_body_read_fail_file);
}

int64_t reducer_frontier_body_read_note_pos(void)
{
    return atomic_load(&g_body_read_fail_pos);
}

/* Name ONE typed TRANSIENT blocker once the per-height failure count crosses
 * the quarantine bound. blocker_set de-dups within its rate-limit window, so a
 * repeated cross is not spam. Naming height/nFile/pos/reason turns a silent
 * "repair defers" into an operator-visible blocker. */
static void body_read_repair_raise_blocker(
    int height, int nFile, int64_t pos,
    enum reducer_frontier_body_read_reason reason, int count)
{
    char reason_s[BLOCKER_REASON_MAX];
    snprintf(reason_s, sizeof(reason_s),
             "body torn read height=%d nFile=%d pos=%lld reason=%s failed %d "
             "times: on-disk block bytes are torn/corrupt after body_persist "
             "verified them — HAVE_DATA dropped for a peer refetch "
             "(have_data_unreadable); H* cannot advance until the body "
             "re-downloads and the stages revalidate it",
             height, nFile, (long long)pos,
             reducer_frontier_body_read_reason_name(reason), count);
    struct blocker_record b;
    if (blocker_init(&b, "reducer_frontier.body_read_torn", "stage_repair",
                     BLOCKER_TRANSIENT, reason_s))
        (void)blocker_set(&b);
}

void reducer_frontier_body_read_note_record(
    int height, int nFile, int64_t pos,
    enum reducer_frontier_body_read_reason reason)
{
    if (height < 0)
        return;
    int64_t cur = atomic_load(&g_body_read_fail_height);
    if (cur >= 0 && (int64_t)height > cur)
        return; /* a lower failing height must heal first (lowest-first) */
    int count;
    if (cur == (int64_t)height) {
        count = atomic_fetch_add(&g_body_read_fail_count, 1) + 1;
    } else {
        atomic_store(&g_body_read_fail_height, (int64_t)height);
        atomic_store(&g_body_read_fail_count, 1);
        count = 1;
    }
    atomic_store(&g_body_read_fail_file, nFile);
    atomic_store(&g_body_read_fail_pos, pos);
    atomic_store(&g_body_read_fail_reason, (int)reason);
    if (count >= REDUCER_FRONTIER_BODY_READ_QUARANTINE_MAX)
        body_read_repair_raise_blocker(height, nFile, pos, reason, count);
}

void reducer_frontier_body_read_note_clear_at(int height)
{
    int64_t cur = atomic_load(&g_body_read_fail_height);
    if (cur < 0 || cur != (int64_t)height)
        return;
    atomic_store(&g_body_read_fail_height, (int64_t)-1);
    atomic_store(&g_body_read_fail_count, 0);
    atomic_store(&g_body_read_fail_reason, (int)REDUCER_FRONTIER_BODY_READ_OK);
    blocker_clear("reducer_frontier.body_read_torn");
}

#ifdef ZCL_TESTING
void reducer_frontier_body_read_note_reset_for_testing(void)
{
    atomic_store(&g_body_read_fail_height, (int64_t)-1);
    atomic_store(&g_body_read_fail_file, -1);
    atomic_store(&g_body_read_fail_pos, (int64_t)0);
    atomic_store(&g_body_read_fail_count, 0);
    atomic_store(&g_body_read_fail_reason, (int)REDUCER_FRONTIER_BODY_READ_OK);
    blocker_clear("reducer_frontier.body_read_torn");
}

int reducer_frontier_body_read_note_count_for_testing(void)
{
    return atomic_load(&g_body_read_fail_count);
}
#endif
