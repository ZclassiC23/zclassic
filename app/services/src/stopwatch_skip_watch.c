/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * stopwatch_skip_watch — see services/stopwatch_skip_watch.h for the full
 * contract (what a skip class means, why this is reporter-only, and why it
 * arms no supervised child). This file is a pure file-scan observer: no DB
 * writes, no threads, no chain locks, no allocation, nothing latched. */

// one-result-type-ok:stopwatch-skip-reporter-no-fallible-surface
//
// A read-only evidence reader, not a fallible service executor. Its surfaces
// are bool probes (classify, scan, read, resolve), a formatter, and the
// project-wide bool dump_state_json convention. Every "false" here means the
// CALLER passed bad arguments; a missing or unreadable ledger is data
// (present=false), never an error a caller branches on, because absence of a
// stopwatch ledger is exactly what a host that never installed the gate looks
// like.

#include "services/stopwatch_skip_watch.h"

#include "json/json.h"
#include "util/log_macros.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One row of the shared class table. match == NULL marks a structural
 * fallback row (see the .def header). */
struct stopwatch_skip_class_row {
    const char *name;
    unsigned    threshold;
    const char *match;
};

static const struct stopwatch_skip_class_row g_class_rows[] = {
#define STOPWATCH_SKIP_CLASS(name_, thr_, match_) { (name_), (thr_), (match_) },
#define STOPWATCH_SKIP_FALLBACK(name_, thr_)      { (name_), (thr_), NULL },
#include "services/stopwatch_skip_classes.def"
#undef STOPWATCH_SKIP_FALLBACK
#undef STOPWATCH_SKIP_CLASS
};

#define STOPWATCH_CLASS_ROW_COUNT \
    (sizeof(g_class_rows) / sizeof(g_class_rows[0]))

#define STOPWATCH_CLASS_HARNESS_MISUSE "harness_misuse"
#define STOPWATCH_CLASS_UNCLASSIFIED   "unclassified"

/* ── tiny bounded helpers (no allocation, single-line JSON rows) ────── */

static void copy_bounded(char *dst, size_t cap, const char *src, size_t len)
{
    if (!dst || cap == 0)
        return;
    if (!src)
        len = 0;
    if (len >= cap)
        len = cap - 1;
    if (len)
        memcpy(dst, src, len);
    dst[len] = '\0';
}

/* First occurrence of NUL-terminated `needle` inside [hay, hay+len). */
static const char *find_sub(const char *hay, size_t len, const char *needle)
{
    size_t nlen = strlen(needle);
    if (!hay || nlen == 0 || nlen > len)
        return NULL;
    for (size_t i = 0; i + nlen <= len; i++) {
        if (memcmp(hay + i, needle, nlen) == 0)
            return hay + i;
    }
    return NULL;
}

/* Locate `"key":` in one row and return the first byte of its value, or NULL.
 * Deliberately simple: these rows are flat, single-line, unnested JSON with
 * no duplicate keys — the same assumption the shell judge's fld_num()/
 * fld_str() already make. */
static const char *row_value(const char *row, size_t len, const char *key)
{
    char needle[64];
    if (snprintf(needle, sizeof(needle), "\"%s\":", key) >= (int)sizeof(needle))
        return NULL;
    const char *at = find_sub(row, len, needle);
    if (!at)
        return NULL;
    at += strlen(needle);
    const char *end = row + len;
    while (at < end && (*at == ' ' || *at == '\t'))
        at++;
    return at < end ? at : NULL;
}

/* Copy a string-valued field into dst. Returns true when the FIELD EXISTS as
 * a JSON string (even if empty) — the caller needs "absent" and "present but
 * empty" to read differently. */
static bool row_str(const char *row, size_t len, const char *key,
                    char *dst, size_t cap)
{
    if (dst && cap)
        dst[0] = '\0';
    const char *at = row_value(row, len, key);
    if (!at || *at != '"')
        return false;
    at++;
    const char *end = row + len;
    size_t n = 0;
    while (at < end && *at != '"') {
        char c = *at;
        if (c == '\\' && at + 1 < end) {
            at++;
            c = *at;
            if (c == 'n' || c == 't' || c == 'r')
                c = ' ';
        }
        if (dst && cap && n + 1 < cap)
            dst[n++] = c;
        at++;
    }
    if (dst && cap)
        dst[n < cap ? n : cap - 1] = '\0';
    return at < end;
}

/* Read an integer-valued field. Returns false when absent or non-numeric
 * (JSON null included — the collector records null for anything it could not
 * measure, and null is not a number). */
static bool row_int(const char *row, size_t len, const char *key, int64_t *out)
{
    const char *at = row_value(row, len, key);
    if (!at)
        return false;
    const char *end = row + len;
    bool neg = false;
    if (*at == '-') {
        neg = true;
        at++;
    }
    if (at >= end || *at < '0' || *at > '9')
        return false;
    int64_t v = 0;
    while (at < end && *at >= '0' && *at <= '9') {
        if (v > (INT64_MAX - (*at - '0')) / 10)
            return false;
        v = v * 10 + (*at - '0');
        at++;
    }
    if (out)
        *out = neg ? -v : v;
    return true;
}

/* ── classification ─────────────────────────────────────────────────── */

static const struct stopwatch_skip_class_row *class_by_name(const char *name)
{
    for (size_t i = 0; i < STOPWATCH_CLASS_ROW_COUNT; i++) {
        if (strcmp(g_class_rows[i].name, name) == 0)
            return &g_class_rows[i];
    }
    return NULL;
}

bool stopwatch_skip_classify(const char *reason, bool reason_field_present,
                             bool has_artifact, char *cls, size_t cls_cap,
                             unsigned *threshold)
{
    if (!cls || cls_cap == 0)
        LOG_FAIL("stopwatch", "class output buffer is NULL/empty");
    if (!threshold)
        LOG_FAIL("stopwatch", "threshold output is NULL");

    if (reason && reason[0]) {
        size_t rlen = strlen(reason);
        for (size_t i = 0; i < STOPWATCH_CLASS_ROW_COUNT; i++) {
            const struct stopwatch_skip_class_row *r = &g_class_rows[i];
            if (!r->match)
                continue;
            if (find_sub(reason, rlen, r->match)) {
                copy_bounded(cls, cls_cap, r->name, strlen(r->name));
                *threshold = r->threshold;
                return true;
            }
        }
    }

    /* No substring matched. Two structural outcomes, and the difference
     * matters: a skip that wrote NO artifact never reached the harness's
     * skip() at all, so it died in argv parsing (harness_misuse, threshold
     * 1). Anything else — including every ledger row written before
     * skip_reason existed — is honestly unknown, not benign. */
    const char *pick = STOPWATCH_CLASS_UNCLASSIFIED;
    if (reason_field_present && (!reason || !reason[0]) && !has_artifact)
        pick = STOPWATCH_CLASS_HARNESS_MISUSE;

    const struct stopwatch_skip_class_row *row = class_by_name(pick);
    if (!row)
        LOG_FAIL("stopwatch", "fallback class '%s' missing from the table",
                 pick);
    copy_bounded(cls, cls_cap, row->name, strlen(row->name));
    *threshold = row->threshold;
    return true;
}

/* ── scan ───────────────────────────────────────────────────────────── */

/* Running state the per-row folder needs beyond the report itself: whether
 * the MOST RECENT skip row carried a skip_reason field at all, and whether it
 * named an artifact dir. Both feed classification once the fold ends. */
struct scan_state {
    bool last_reason_present;
    bool last_has_artifact;
};

static void scan_init(struct stopwatch_skip_report *out,
                      struct scan_state *st)
{
    memset(out, 0, sizeof(*out));
    out->last_ts = -1;
    out->last_pass_ts = -1;
    memset(st, 0, sizeof(*st));
}

static void scan_row(const char *row, size_t rlen,
                     struct stopwatch_skip_report *out, struct scan_state *st)
{
    if (rlen == 0)
        return;

    char verdict[STOPWATCH_SKIP_VERDICT_MAX];
    if (!row_str(row, rlen, "verdict", verdict, sizeof(verdict)) ||
        verdict[0] == '\0') {
        out->malformed_rows++;
        return;
    }
    out->rows_scanned++;
    out->present = true;

    int64_t ts = -1;
    if (!row_int(row, rlen, "ts", &ts))
        ts = -1;
    out->last_ts = ts;
    copy_bounded(out->last_verdict, sizeof(out->last_verdict), verdict,
                 strlen(verdict));

    if (strcmp(verdict, "pass") == 0) {
        out->skip_streak = 0;
        out->no_pass_streak = 0;
        out->last_pass_ts = ts;
        return;
    }

    out->no_pass_streak++;
    if (strcmp(verdict, "skip") != 0) {
        out->skip_streak = 0;
        return;
    }

    out->skip_streak++;
    char reason[STOPWATCH_SKIP_REASON_MAX];
    char artifact[512];
    st->last_reason_present =
        row_str(row, rlen, "skip_reason", reason, sizeof(reason));
    st->last_has_artifact =
        row_str(row, rlen, "artifact_dir", artifact, sizeof(artifact)) &&
        artifact[0] != '\0';
    copy_bounded(out->skip_reason, sizeof(out->skip_reason),
                 st->last_reason_present ? reason : "",
                 st->last_reason_present ? strlen(reason) : 0);
}

static bool scan_finish(struct stopwatch_skip_report *out,
                        const struct scan_state *st)
{
    if (out->skip_streak == 0)
        return true;
    if (!stopwatch_skip_classify(out->skip_reason, st->last_reason_present,
                                 st->last_has_artifact, out->skip_class,
                                 sizeof(out->skip_class), &out->threshold))
        return false;
    out->alarm = out->threshold > 0 && out->skip_streak >= out->threshold;
    return true;
}

bool stopwatch_skip_scan(const char *text, size_t len,
                         struct stopwatch_skip_report *out)
{
    if (!out)
        LOG_FAIL("stopwatch", "report output is NULL");
    struct scan_state st;
    scan_init(out, &st);
    if (!text && len)
        LOG_FAIL("stopwatch", "ledger text is NULL with len=%zu", len);

    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && text[j] != '\n')
            j++;
        scan_row(text + i, j - i, out, &st);
        i = (j < len) ? j + 1 : len;
    }
    return scan_finish(out, &st);
}

/* ── ledger IO ──────────────────────────────────────────────────────── */

/* One ledger row is a flat JSON object under 1 KiB; 4 KiB is generous. A row
 * longer than this is not a stopwatch row — its tail is consumed and dropped
 * rather than folded as a second, phantom row. */
#define STOPWATCH_ROW_MAX 4096

bool stopwatch_skip_read_ledger(const char *path,
                                struct stopwatch_skip_report *out)
{
    if (!out)
        LOG_FAIL("stopwatch", "report output is NULL");
    struct scan_state st;
    scan_init(out, &st);
    if (!path || !path[0])
        LOG_FAIL("stopwatch", "ledger path is NULL/empty");

    FILE *f = fopen(path, "rb");
    if (!f)
        return true;            /* absent ledger: present=false, not an error */

    /* Bounded read: seek to the last STOPWATCH_SKIP_TAIL_BYTES so a rotated
     * or very long ledger stays a cheap dumper, then stream it a row at a
     * time so peak memory is one row, not one tail. */
    bool partial_head = false;
    if (fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        if (size > (long)STOPWATCH_SKIP_TAIL_BYTES) {
            if (fseek(f, size - (long)STOPWATCH_SKIP_TAIL_BYTES,
                      SEEK_SET) == 0)
                partial_head = true;
            else
                rewind(f);
        } else {
            rewind(f);
        }
    }

    char row[STOPWATCH_ROW_MAX];
    bool first = true;
    bool overlong = false;
    while (fgets(row, sizeof(row), f)) {
        size_t rlen = strlen(row);
        bool complete = rlen > 0 && row[rlen - 1] == '\n';
        if (complete)
            rlen--;
        if (overlong) {
            /* Tail of a row that did not fit — never folded. */
            overlong = !complete;
            continue;
        }
        if (!complete && rlen == sizeof(row) - 1) {
            overlong = true;
            out->malformed_rows++;
            continue;
        }
        if (first) {
            first = false;
            /* The first line after a mid-file seek is a fragment; drop it
             * rather than folding a torn row as evidence. */
            if (partial_head)
                continue;
        }
        scan_row(row, rlen, out, &st);
    }
    fclose(f);
    return scan_finish(out, &st);
}

bool stopwatch_skip_resolve_ledger(const char *which, char *out, size_t cap)
{
    if (!out || cap == 0)
        LOG_FAIL("stopwatch", "path output buffer is NULL/empty");
    out[0] = '\0';
    if (!which || !which[0])
        LOG_FAIL("stopwatch", "ledger selector is NULL/empty");

    const char *file_env = NULL;
    const char *dir_env = NULL;
    const char *dir_default = NULL;
    if (strcmp(which, "c3") == 0) {
        file_env = "ZCL_C3_STOPWATCH_HISTORY";
        dir_env = "ZCL_C3_HISTORY_DIR";
        dir_default = ".local/state/zclassic23-c3-stopwatch";
    } else if (strcmp(which, "netdisrupt") == 0) {
        file_env = "ZCL_NETDISRUPT_STOPWATCH_HISTORY";
        dir_env = "ZCL_ND_HISTORY_DIR";
        dir_default = ".local/state/zclassic23-netdisrupt-stopwatch";
    } else {
        LOG_FAIL("stopwatch", "unknown ledger selector '%s'", which);
    }

    const char *v = getenv(file_env);
    if (v && v[0]) {
        copy_bounded(out, cap, v, strlen(v));
        return true;
    }
    v = getenv(dir_env);
    if (v && v[0]) {
        if (snprintf(out, cap, "%s/history.jsonl", v) >= (int)cap)
            LOG_FAIL("stopwatch", "%s path too long for buffer", dir_env);
        return true;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        out[0] = '\0';
        LOG_FAIL("stopwatch", "no %s, no %s and no HOME to resolve the %s "
                 "ledger path from", file_env, dir_env, which);
    }
    if (snprintf(out, cap, "%s/%s/history.jsonl", home, dir_default) >=
        (int)cap)
        LOG_FAIL("stopwatch", "default %s ledger path too long", which);
    return true;
}

const char *stopwatch_skip_alarm_text(const struct stopwatch_skip_report *r,
                                      char *buf, size_t cap)
{
    if (!buf || cap == 0)
        return "";
    if (!r) {
        copy_bounded(buf, cap, "no report", strlen("no report"));
        return buf;
    }
    if (!r->present) {
        snprintf(buf, cap, "quiet no_ledger — the stopwatch gate has never "
                 "written a row on this host");
        return buf;
    }
    if (r->skip_streak == 0) {
        snprintf(buf, cap,
                 "quiet last_verdict=%s skip_streak=0 no_pass_streak=%u",
                 r->last_verdict[0] ? r->last_verdict : "-",
                 r->no_pass_streak);
        return buf;
    }
    snprintf(buf, cap,
             "%s class=%s skip_streak=%u threshold=%u no_pass_streak=%u "
             "reason=\"%s\"%s",
             r->alarm ? "ALARM" : "quiet",
             r->skip_class[0] ? r->skip_class : "-", r->skip_streak,
             r->threshold, r->no_pass_streak,
             r->skip_reason[0] ? r->skip_reason : "-",
             r->threshold == 0
                 ? " — benign: nothing was configured, so there was nothing "
                   "to prove"
                 : (r->alarm ? " — this proof has not run for that many "
                               "consecutive scheduled attempts"
                             : ""));
    return buf;
}

/* ── typed surface: dumpstate stopwatch_evidence ────────────────────── */

static void push_ledger_json(struct json_value *parent, const char *which)
{
    struct json_value obj;
    json_init(&obj);
    json_set_object(&obj);

    char path[PATH_MAX];
    bool resolved = stopwatch_skip_resolve_ledger(which, path, sizeof(path));
    json_push_kv_str(&obj, "ledger", which);
    json_push_kv_str(&obj, "path", resolved ? path : "unresolved");

    struct stopwatch_skip_report rep;
    bool read_ok = resolved && stopwatch_skip_read_ledger(path, &rep);
    if (!read_ok) {
        memset(&rep, 0, sizeof(rep));
        rep.last_ts = -1;
        rep.last_pass_ts = -1;
    }
    json_push_kv_bool(&obj, "present", rep.present);
    json_push_kv_int(&obj, "rows_scanned", rep.rows_scanned);
    json_push_kv_int(&obj, "malformed_rows", rep.malformed_rows);
    json_push_kv_str(&obj, "last_verdict",
                     rep.last_verdict[0] ? rep.last_verdict : "-");
    json_push_kv_int(&obj, "last_ts", rep.last_ts);
    json_push_kv_int(&obj, "last_pass_ts", rep.last_pass_ts);
    json_push_kv_int(&obj, "skip_streak", rep.skip_streak);
    json_push_kv_int(&obj, "no_pass_streak", rep.no_pass_streak);
    json_push_kv_str(&obj, "skip_class",
                     rep.skip_class[0] ? rep.skip_class : "-");
    json_push_kv_str(&obj, "skip_reason",
                     rep.skip_reason[0] ? rep.skip_reason : "-");
    json_push_kv_int(&obj, "alarm_threshold", rep.threshold);
    json_push_kv_bool(&obj, "alarm", rep.alarm);

    char text[STOPWATCH_SKIP_ALARM_MAX];
    json_push_kv_str(&obj, "summary",
                     stopwatch_skip_alarm_text(&rep, text, sizeof(text)));
    json_push_back(parent, &obj);
    json_free(&obj);
}

bool stopwatch_evidence_dump_state_json(struct json_value *out,
                                        const char *key)
{
    if (!out)
        LOG_FAIL("stopwatch", "dump output is NULL");
    json_set_object(out);
    json_push_kv_str(out, "role",
                     "REPORTER ONLY — describes the stopwatch evidence "
                     "ledgers on disk; never grades a run, never clears a "
                     "verdict, never feeds the architecture score");

    struct json_value arr;
    json_init(&arr);
    json_set_array(&arr);
    if (key && key[0]) {
        if (strcmp(key, "c3") != 0 && strcmp(key, "netdisrupt") != 0) {
            json_free(&arr);
            json_push_kv_str(out, "error",
                             "unknown key: use c3, netdisrupt, or no key");
            return true;
        }
        push_ledger_json(&arr, key);
    } else {
        push_ledger_json(&arr, "c3");
        push_ledger_json(&arr, "netdisrupt");
    }
    json_push_kv(out, "ledgers", &arr);
    json_free(&arr);

    const struct json_value *ledgers = json_get(out, "ledgers");
    int alarms = 0;
    size_t n = ledgers ? json_size(ledgers) : 0;
    for (size_t i = 0; i < n; i++) {
        if (json_get_bool(json_get(json_at(ledgers, i), "alarm")))
            alarms++;
    }
    json_push_kv_int(out, "alarm_count", alarms);
    json_push_kv_bool(out, "alarm", alarms > 0);
    return true;
}
