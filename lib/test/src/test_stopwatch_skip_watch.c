/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_stopwatch_skip_watch — hermetic tests for the skip-streak detector
 * behind `zclassic23 ops state --subsystem=stopwatch_evidence`.
 *
 * The defect under regression: the C3 stopwatch recorded verdict=skip on
 * every scheduled run from 2026-07-28 06:02 onward and NOTHING said so. The
 * judge grades a skip as FAIL but nothing ran the judge, and the architecture
 * scorer reads `tail -n 5`, so one old pass kept the number at 85 for four
 * consecutive skips. The four contracts asserted here are exactly the ones
 * that make that impossible to repeat quietly:
 *
 *   (a) N-1 consecutive skips is QUIET   — one miss is a bouncing fixture,
 *                                          not evidence; a detector that
 *                                          cries wolf gets ignored.
 *   (b) N consecutive skips RAISES       — the condition survived a full
 *                                          timer interval; it is gone.
 *   (c) a pass CLEARS the streak         — recovery is recognised, and the
 *                                          alarm cannot latch on stale news.
 *   (d) a BENIGN skip never raises       — "nothing configured, nothing to
 *                                          prove" must read differently from
 *                                          "the peer I need is dead".
 *
 * Plus the surrounding contract: config/harness classes fire at 1 because
 * they can never self-heal, an unknown or legacy skip is loud-ish rather than
 * assumed benign, a non-skip verdict breaks a skip streak, absence of a
 * ledger is data and not an alarm, and the typed dump reports all of it.
 *
 * Every block builds its ledger under a private tmp dir exported through the
 * same env vars the collector scripts read, so the operator's real ledgers,
 * the live node, and $HOME are never touched. */

#include "test/test_core.h"

#include "json/json.h"
#include "services/stopwatch_skip_watch.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SSW_CHECK(name, expr) do { \
    printf("stopwatch_skip_watch: %s... ", (name)); \
    if (expr) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

/* One ledger row in the exact shape c3_stopwatch_run_and_record.sh appends. */
static void row_skip(char *buf, size_t cap, long long ts, const char *reason,
                     const char *artifact)
{
    snprintf(buf, cap,
             "{\"ts\":%lld,\"verdict\":\"skip\",\"exit_code\":2,"
             "\"artifact_dir\":\"%s\",\"skip_reason\":\"%s\"}\n",
             ts, artifact, reason);
}

static size_t append(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(dst);
    snprintf(dst + n, cap - n, "%s", src);
    return strlen(dst);
}

static bool scan_text(const char *text, struct stopwatch_skip_report *rep)
{
    return stopwatch_skip_scan(text, strlen(text), rep);
}

/* The live-shaped reason: the exact string the C3 harness writes when the
 * serving fixture peer does not answer (cold_start_to_tip_stopwatch.sh). */
#define REASON_PEER_DEAD "serving peer not reachable: 127.0.0.1:39070"
/* The one genuinely benign reason class — only PROOF B can reach it. */
#define REASON_BENIGN "no valid --client-rpc / ZCL_ND_CLIENT_RPCPORT given"

static bool write_file(const char *path, const char *body)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    fputs(body, f);
    return fclose(f) == 0;
}

int test_stopwatch_skip_watch(void)
{
    int failures = 0;
    char text[8192];
    char row[512];
    struct stopwatch_skip_report rep;

    /* (a) N-1 = ONE fixture_absent skip is quiet. */
    {
        text[0] = '\0';
        row_skip(row, sizeof(row), 1000, REASON_PEER_DEAD, "/a/1");
        append(text, sizeof(text), row);
        bool ok = scan_text(text, &rep);
        ok = ok && rep.present && rep.rows_scanned == 1;
        ok = ok && rep.skip_streak == 1 && rep.no_pass_streak == 1;
        ok = ok && strcmp(rep.skip_class, "fixture_absent") == 0;
        ok = ok && rep.threshold == 2;
        ok = ok && rep.alarm == false;
        char msg[STOPWATCH_SKIP_ALARM_MAX];
        stopwatch_skip_alarm_text(&rep, msg, sizeof(msg));
        ok = ok && strstr(msg, "ALARM") == NULL;
        SSW_CHECK("one fixture_absent skip is quiet (below threshold)", ok);
    }

    /* (b) N = TWO consecutive fixture_absent skips raise the alarm. */
    {
        text[0] = '\0';
        row_skip(row, sizeof(row), 1000, REASON_PEER_DEAD, "/a/1");
        append(text, sizeof(text), row);
        row_skip(row, sizeof(row), 2000, REASON_PEER_DEAD, "/a/2");
        append(text, sizeof(text), row);
        bool ok = scan_text(text, &rep);
        ok = ok && rep.skip_streak == 2 && rep.threshold == 2;
        ok = ok && rep.alarm == true;
        ok = ok && strcmp(rep.skip_class, "fixture_absent") == 0;
        ok = ok && strcmp(rep.skip_reason, REASON_PEER_DEAD) == 0;
        char msg[STOPWATCH_SKIP_ALARM_MAX];
        stopwatch_skip_alarm_text(&rep, msg, sizeof(msg));
        ok = ok && strstr(msg, "ALARM") != NULL;
        ok = ok && strstr(msg, "class=fixture_absent") != NULL;
        ok = ok && strstr(msg, "skip_streak=2") != NULL;
        SSW_CHECK("two fixture_absent skips raise a named alarm", ok);
    }

    /* Four skips — the shape the live C3 ledger actually reached — still
     * names the same class and keeps counting rather than saturating. */
    {
        text[0] = '\0';
        for (int i = 0; i < 4; i++) {
            row_skip(row, sizeof(row), 1000 + i, REASON_PEER_DEAD, "/a/x");
            append(text, sizeof(text), row);
        }
        bool ok = scan_text(text, &rep);
        ok = ok && rep.skip_streak == 4 && rep.no_pass_streak == 4;
        ok = ok && rep.alarm == true && rep.last_pass_ts == -1;
        SSW_CHECK("four consecutive skips keep counting past the threshold",
                  ok);
    }

    /* (c) a pass clears the streak and the alarm. */
    {
        text[0] = '\0';
        for (int i = 0; i < 4; i++) {
            row_skip(row, sizeof(row), 1000 + i, REASON_PEER_DEAD, "/a/x");
            append(text, sizeof(text), row);
        }
        append(text, sizeof(text),
               "{\"ts\":9000,\"verdict\":\"pass\",\"exit_code\":0,"
               "\"wall_clock_seconds\":42,\"artifact_dir\":\"/a/p\"}\n");
        bool ok = scan_text(text, &rep);
        ok = ok && rep.skip_streak == 0 && rep.no_pass_streak == 0;
        ok = ok && rep.alarm == false && rep.threshold == 0;
        ok = ok && rep.last_pass_ts == 9000 && rep.last_ts == 9000;
        ok = ok && strcmp(rep.last_verdict, "pass") == 0;
        ok = ok && rep.skip_class[0] == '\0';
        SSW_CHECK("a pass clears the skip streak and the alarm", ok);
    }

    /* (d) a BENIGN skip never raises, no matter how long the run. */
    {
        text[0] = '\0';
        for (int i = 0; i < 20; i++) {
            row_skip(row, sizeof(row), 1000 + i, REASON_BENIGN, "/a/b");
            append(text, sizeof(text), row);
        }
        bool ok = scan_text(text, &rep);
        ok = ok && rep.skip_streak == 20;
        ok = ok && strcmp(rep.skip_class, "not_configured") == 0;
        ok = ok && rep.threshold == 0 && rep.alarm == false;
        char msg[STOPWATCH_SKIP_ALARM_MAX];
        stopwatch_skip_alarm_text(&rep, msg, sizeof(msg));
        ok = ok && strstr(msg, "ALARM") == NULL;
        ok = ok && strstr(msg, "benign") != NULL;
        SSW_CHECK("20 benign not_configured skips never raise an alarm", ok);
    }

    /* config_error fires on the FIRST occurrence — it can never self-heal. */
    {
        text[0] = '\0';
        row_skip(row, sizeof(row), 1000,
                 "node binary absent/not executable: /nope/zclassic23", "/a/c");
        append(text, sizeof(text), row);
        bool ok = scan_text(text, &rep);
        ok = ok && strcmp(rep.skip_class, "config_error") == 0;
        ok = ok && rep.threshold == 1 && rep.skip_streak == 1;
        ok = ok && rep.alarm == true;
        SSW_CHECK("one config_error skip raises immediately (never self-heals)",
                  ok);
    }

    /* harness_misuse: exit 2 with NO artifact means skip() never ran, so the
     * harness died in argv parsing. Byte-indistinguishable from a dead
     * fixture in the old ledger; now its own class, at threshold 1. */
    {
        text[0] = '\0';
        append(text, sizeof(text),
               "{\"ts\":1000,\"verdict\":\"skip\",\"exit_code\":2,"
               "\"artifact_dir\":\"\",\"skip_reason\":\"\"}\n");
        bool ok = scan_text(text, &rep);
        ok = ok && strcmp(rep.skip_class, "harness_misuse") == 0;
        ok = ok && rep.threshold == 1 && rep.alarm == true;
        SSW_CHECK("skip with no artifact classifies as harness_misuse", ok);
    }

    /* A legacy row predating skip_reason is UNCLASSIFIED, never assumed
     * benign — an unknown skip is a gap in the class table, and the honest
     * answer is to say so. */
    {
        text[0] = '\0';
        append(text, sizeof(text),
               "{\"ts\":1000,\"verdict\":\"skip\",\"exit_code\":2,"
               "\"artifact_dir\":\"/a/legacy\"}\n");
        bool ok = scan_text(text, &rep);
        ok = ok && strcmp(rep.skip_class, "unclassified") == 0;
        ok = ok && rep.threshold == 2 && rep.alarm == false;
        append(text, sizeof(text),
               "{\"ts\":2000,\"verdict\":\"skip\",\"exit_code\":2,"
               "\"artifact_dir\":\"/a/legacy2\"}\n");
        ok = ok && scan_text(text, &rep);
        ok = ok && rep.skip_streak == 2 && rep.alarm == true;
        SSW_CHECK("legacy row with no skip_reason is unclassified, not benign",
                  ok);
    }

    /* A non-skip verdict BREAKS a skip streak but not the no-pass streak —
     * the two numbers answer different questions and must not be conflated. */
    {
        text[0] = '\0';
        row_skip(row, sizeof(row), 1000, REASON_PEER_DEAD, "/a/1");
        append(text, sizeof(text), row);
        append(text, sizeof(text),
               "{\"ts\":2000,\"verdict\":\"seam\",\"exit_code\":3,"
               "\"artifact_dir\":\"/a/s\"}\n");
        row_skip(row, sizeof(row), 3000, REASON_PEER_DEAD, "/a/3");
        append(text, sizeof(text), row);
        bool ok = scan_text(text, &rep);
        ok = ok && rep.skip_streak == 1 && rep.no_pass_streak == 3;
        ok = ok && rep.alarm == false;
        SSW_CHECK("a seam between skips breaks the skip streak, not no_pass",
                  ok);
    }

    /* Malformed / foreign lines are counted, never folded as evidence. */
    {
        text[0] = '\0';
        append(text, sizeof(text), "not json at all\n");
        append(text, sizeof(text), "\n");
        row_skip(row, sizeof(row), 1000, REASON_PEER_DEAD, "/a/1");
        append(text, sizeof(text), row);
        bool ok = scan_text(text, &rep);
        ok = ok && rep.rows_scanned == 1 && rep.malformed_rows == 1;
        ok = ok && rep.skip_streak == 1;
        SSW_CHECK("malformed rows are counted, not folded into a streak", ok);
    }

    /* Absence is data: an empty scan reports nothing present and no alarm. */
    {
        bool ok = scan_text("", &rep);
        ok = ok && !rep.present && rep.rows_scanned == 0 && !rep.alarm;
        ok = ok && rep.last_ts == -1 && rep.last_pass_ts == -1;
        SSW_CHECK("an empty ledger is absence, never an alarm", ok);
    }

    /* ── file + env + typed-dump path, all under a private tmp dir ──── */
    {
        char dir[] = "/tmp/zcl-stopwatch-skip-test.XXXXXX";
        bool ok = mkdtemp(dir) != NULL;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/history.jsonl", dir);

        text[0] = '\0';
        for (int i = 0; i < 4; i++) {
            row_skip(row, sizeof(row), 1000 + i, REASON_PEER_DEAD, "/a/x");
            append(text, sizeof(text), row);
        }
        ok = ok && write_file(path, text);

        ok = ok && stopwatch_skip_read_ledger(path, &rep);
        ok = ok && rep.present && rep.skip_streak == 4 && rep.alarm;

        /* A path that does not exist is present=false and NOT an error —
         * a host that never installed the gate must stay silent. */
        char absent[PATH_MAX];
        snprintf(absent, sizeof(absent), "%s/no-such-ledger.jsonl", dir);
        struct stopwatch_skip_report none;
        ok = ok && stopwatch_skip_read_ledger(absent, &none);
        ok = ok && !none.present && !none.alarm;

        /* Resolution honours the collector's own env override. */
        ok = ok && setenv("ZCL_C3_STOPWATCH_HISTORY", path, 1) == 0;
        ok = ok && setenv("ZCL_NETDISRUPT_STOPWATCH_HISTORY", absent, 1) == 0;
        char resolved[PATH_MAX];
        ok = ok && stopwatch_skip_resolve_ledger("c3", resolved,
                                                 sizeof(resolved));
        ok = ok && strcmp(resolved, path) == 0;
        ok = ok && !stopwatch_skip_resolve_ledger("bogus", resolved,
                                                  sizeof(resolved));

        /* The typed surface reports the alarm — this is the requirement that
         * the operator can see it without reading a log file. */
        struct json_value dump;
        json_init(&dump);
        ok = ok && stopwatch_evidence_dump_state_json(&dump, NULL);
        ok = ok && json_get_bool(json_get(&dump, "alarm")) == true;
        ok = ok && json_get_int(json_get(&dump, "alarm_count")) == 1;
        const struct json_value *ledgers = json_get(&dump, "ledgers");
        ok = ok && ledgers && json_size(ledgers) == 2;
        const struct json_value *c3 = ledgers ? json_at(ledgers, 0) : NULL;
        ok = ok && c3 && strcmp(json_get_str(json_get(c3, "ledger")),
                                "c3") == 0;
        ok = ok && json_get_int(json_get(c3, "skip_streak")) == 4;
        ok = ok && strcmp(json_get_str(json_get(c3, "skip_class")),
                          "fixture_absent") == 0;
        ok = ok && json_get_bool(json_get(c3, "alarm")) == true;
        const struct json_value *nd = ledgers ? json_at(ledgers, 1) : NULL;
        ok = ok && nd && json_get_bool(json_get(nd, "present")) == false;
        ok = ok && json_get_bool(json_get(nd, "alarm")) == false;
        json_free(&dump);

        /* Keyed form selects one ledger. */
        json_init(&dump);
        ok = ok && stopwatch_evidence_dump_state_json(&dump, "netdisrupt");
        const struct json_value *one = json_get(&dump, "ledgers");
        ok = ok && one && json_size(one) == 1;
        ok = ok && json_get_bool(json_get(&dump, "alarm")) == false;
        json_free(&dump);

        unsetenv("ZCL_C3_STOPWATCH_HISTORY");
        unsetenv("ZCL_NETDISRUPT_STOPWATCH_HISTORY");
        unlink(path);
        rmdir(dir);
        SSW_CHECK("ledger file + env resolution + typed dump report the alarm",
                  ok);
    }

    return failures;
}
