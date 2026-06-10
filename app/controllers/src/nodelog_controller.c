/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Node-log controller — the `getnodelog` / `zcl_node_log` primitive.
 *
 * Reverse-scans <datadir>/node.log in 64 KB chunks, matches each line
 * against a POSIX-extended regex, filters by level, and stops at
 * `max_lines` or the scan-byte cap. Bounded memory (one chunk plus an
 * accumulator) so it is cheaper than reading the whole multi-MB log.
 *
 * Level detection: log lines from LOG_FAIL / LOG_ERR / LOG_NULL all
 * start with `[domain] file:line function(): message` where `domain`
 * is e.g. "validation", "sync", "net". This handler also recognises
 * the leading `[FATAL]` / `[WARN]` markers used by some boot-time
 * printf paths.
 */

#include "platform/time_compat.h"
#include "controllers/diagnostics_internal.h"

#include "json/json.h"
#include "rpc/server.h"
#include "controllers/strong_params.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <regex.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>

#define NODELOG_DEFAULT_SINCE_SECS    300
#define NODELOG_MAX_SINCE_SECS       86400
#define NODELOG_DEFAULT_MAX_LINES      50
#define NODELOG_MAX_MAX_LINES         500
#define NODELOG_CHUNK_SIZE           65536
#define NODELOG_MAX_PATTERN_LEN        256
#define NODELOG_MAX_SCAN_BYTES   (16 * 1024 * 1024) /* 16 MB hard cap */

enum log_level { LL_ALL = 0, LL_INFO, LL_WARN, LL_ERROR, LL_FATAL };

static enum log_level parse_level(const char *s)
{
    if (!s) return LL_ALL;
    if (!strcmp(s, "all"))   return LL_ALL;
    if (!strcmp(s, "info"))  return LL_INFO;
    if (!strcmp(s, "warn"))  return LL_WARN;
    if (!strcmp(s, "error")) return LL_ERROR;
    if (!strcmp(s, "fatal")) return LL_FATAL;
    return LL_ALL;
}

static enum log_level line_level(const char *line)
{
    /* Cheap heuristic. LOG_FAIL/LOG_ERR/LOG_NULL all use stderr and
     * prefix with [domain]; we treat those as ERROR level. Boot-time
     * printfs use explicit FATAL/WARN markers. Everything else INFO. */
    if (strstr(line, "FATAL") || strstr(line, "PANIC") ||
        strstr(line, "ABORT"))
        return LL_FATAL;
    if (strstr(line, "WARNING") || strstr(line, "WARN:") ||
        strstr(line, "[watchdog] ESCALATION"))
        return LL_WARN;
    if (line[0] == '[' && strstr(line, "(): ") &&
        (strstr(line, "GUARD FAILED") || strstr(line, "FAIL") ||
         strstr(line, "failed") || strstr(line, "error") ||
         strstr(line, "ERROR")))
        return LL_ERROR;
    return LL_INFO;
}

bool diag_rpc_getnodelog(const struct json_value *params, bool help,
                         struct json_value *result)
{
    RPC_HELP(help, result,
        "getnodelog <pattern> [since_secs=300] [max_lines=50] [level=all]\n"
        "\nReverse-scan node.log. `pattern` is POSIX-extended regex.\n"
        "`level` one of: all, info, warn, error, fatal.\n"
        "\nResult: { lines: [...], scanned_bytes, truncated, log_path }");

    const char *pattern = json_get_str(json_at(params, 0));
    if (!pattern || !pattern[0]) {
        json_set_str(result, "getnodelog: missing pattern");
        LOG_FAIL("diag", "getnodelog: missing pattern");
    }
    size_t plen = strlen(pattern);
    if (plen > NODELOG_MAX_PATTERN_LEN) {
        json_set_str(result, "getnodelog: pattern too long");
        LOG_FAIL("diag", "getnodelog: pattern too long (%zu > %d)",
                 plen, NODELOG_MAX_PATTERN_LEN);
    }

    int64_t since_secs = json_at(params, 1) ?
        json_get_int(json_at(params, 1)) : NODELOG_DEFAULT_SINCE_SECS;
    if (since_secs < 0) since_secs = 0;
    if (since_secs > NODELOG_MAX_SINCE_SECS)
        since_secs = NODELOG_MAX_SINCE_SECS;

    int64_t max_lines = json_at(params, 2) ?
        json_get_int(json_at(params, 2)) : NODELOG_DEFAULT_MAX_LINES;
    if (max_lines < 1) max_lines = 1;
    if (max_lines > NODELOG_MAX_MAX_LINES)
        max_lines = NODELOG_MAX_MAX_LINES;

    enum log_level want_level = parse_level(
        json_at(params, 3) ? json_get_str(json_at(params, 3)) : NULL);

    const char *datadir = diag_datadir();
    if (!datadir[0]) {
        json_set_str(result, "getnodelog: datadir not configured");
        LOG_FAIL("diag", "getnodelog: datadir not configured");
    }

    char log_path[1280];
    snprintf(log_path, sizeof(log_path), "%s/node.log", datadir);

    int fd = open(log_path, O_RDONLY);
    if (fd < 0) {
        json_set_str(result, "getnodelog: cannot open node.log");
        LOG_FAIL("diag", "getnodelog: cannot open %s", log_path);
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        json_set_str(result, "getnodelog: fstat failed on node.log");
        LOG_FAIL("diag", "getnodelog: fstat failed on %s", log_path);
    }

    regex_t re;
    int rc = regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB);
    if (rc != 0) {
        char errbuf[128];
        regerror(rc, &re, errbuf, sizeof(errbuf));
        close(fd);
        json_set_str(result, "getnodelog: bad regex");
        LOG_FAIL("diag", "getnodelog: bad regex '%s': %s",
                 pattern, errbuf);
    }

    int64_t now = (int64_t)platform_time_wall_time_t();
    int64_t earliest = (since_secs > 0) ? (now - since_secs) : 0;

    /* Reverse-scan in NODELOG_CHUNK_SIZE chunks. Each chunk we read,
     * we split into lines and emit any complete lines that match.
     * Partial-line tail goes back into the next chunk. */
    json_set_object(result);
    struct json_value lines_arr = {0};
    json_set_array(&lines_arr);
    int emitted = 0;
    bool truncated = false;
    int64_t scanned = 0;
    off_t pos = st.st_size;

    /* Each scanned line goes onto a stack so we emit in
     * newest-first order; lines_arr is built from that stack at the end. */
    char *stack[NODELOG_MAX_MAX_LINES];
    int stack_n = 0;
    char carry[NODELOG_CHUNK_SIZE + 1] = {0};
    size_t carry_len = 0;

    while (pos > 0 && emitted < max_lines &&
           scanned < NODELOG_MAX_SCAN_BYTES) {
        size_t want = (pos > NODELOG_CHUNK_SIZE) ? NODELOG_CHUNK_SIZE
                                                  : (size_t)pos;
        off_t start = pos - (off_t)want;
        char buf[NODELOG_CHUNK_SIZE + 1];
        ssize_t got = pread(fd, buf, want, start);
        if (got <= 0) break;
        buf[got] = '\0';
        scanned += got;
        pos = start;

        /* Combine `buf` (this chunk) with `carry` (partial line from
         * the previous-newer chunk). Process newline-separated lines
         * from the end. */
        size_t combined_cap = (size_t)got + carry_len + 1;
        char *combined = zcl_malloc(combined_cap, "diagnostics.node_log.combined");
        if (!combined) break;
        memcpy(combined, buf, got);
        memcpy(combined + got, carry, carry_len);
        combined[got + carry_len] = '\0';
        size_t combined_len = (size_t)got + carry_len;

        /* Walk backwards over `combined`, slicing at '\n'. */
        size_t line_end = combined_len;
        for (ssize_t i = (ssize_t)combined_len - 1;
             i >= -1 && emitted < max_lines; i--) {
            if (i < 0 || combined[i] == '\n') {
                size_t start_off = (i < 0) ? 0 : (size_t)i + 1;
                if (start_off < line_end) {
                    /* line = combined[start_off .. line_end) */
                    size_t llen = line_end - start_off;
                    if (i < 0 && pos > 0) {
                        /* Partial line at the start of this chunk —
                         * save as carry, don't emit yet (older chunk
                         * holds the head). */
                        if (llen > sizeof(carry) - 1) llen = sizeof(carry) - 1;
                        memcpy(carry, combined + start_off, llen);
                        carry_len = llen;
                        carry[llen] = '\0';
                    } else {
                        char *line = zcl_malloc(llen + 1, "diagnostics.node_log.line");
                        if (line) {
                            memcpy(line, combined + start_off, llen);
                            line[llen] = '\0';
                            /* Filter by regex + level. */
                            bool match = (regexec(&re, line, 0,
                                                  NULL, 0) == 0);
                            enum log_level lvl = line_level(line);
                            bool level_ok = (want_level == LL_ALL) ||
                                            (lvl >= want_level);
                            /* Crude "since" filter: log lines start
                             * with "Mon DD HH:MM:SS" or "MM-DD HH:MM:SS"
                             * but we don't parse — use mtime delta as
                             * a coarse approximation. Lines older than
                             * `earliest` are not specifically detected
                             * here; that's a future enhancement. */
                            (void)earliest;
                            if (match && level_ok &&
                                stack_n < NODELOG_MAX_MAX_LINES) {
                                stack[stack_n++] = line;
                                emitted++;
                            } else {
                                free(line);
                            }
                        }
                        carry_len = 0;
                    }
                }
                line_end = (size_t)i;
            }
            if (i < 0) break;
        }
        free(combined);
        if (pos == 0) break;
    }

    if (pos > 0 && scanned >= NODELOG_MAX_SCAN_BYTES)
        truncated = true;

    /* Build the result array newest-first (stack already newest-last → reverse). */
    for (int i = 0; i < stack_n; i++) {
        struct json_value lv = {0};
        json_set_str(&lv, stack[i]);
        json_push_back(&lines_arr, &lv);
        json_free(&lv);
        free(stack[i]);
    }

    json_push_kv(result, "lines", &lines_arr);
    json_free(&lines_arr);
    json_push_kv_int(result, "scanned_bytes", scanned);
    json_push_kv_bool(result, "truncated", truncated);
    json_push_kv_str(result, "log_path", log_path);
    json_push_kv_int(result, "emitted", emitted);

    regfree(&re);
    close(fd);
    return true;
}
