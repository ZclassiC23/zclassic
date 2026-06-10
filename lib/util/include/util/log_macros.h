/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Error return macros that force logging at every failure site.
 *
 * An agent that writes `return -1;` in an MCP handler or `return false;`
 * in a service function provides zero diagnostic info. These macros
 * make the logged version SHORTER than the silent version, so the
 * path of least resistance is the correct one.
 *
 * Usage:
 *   // Instead of: return false;
 *   LOG_FAIL("wallet", "key not found: pubkey_hash=%s", hex);
 *
 *   // Instead of: return -1;
 *   LOG_ERR("mcp", "rpc backend unreachable: method=%s", method);
 *
 *   // Instead of: return NULL;
 *   LOG_NULL("sync", "malloc failed for %d headers", count);
 */

#ifndef ZCL_LOG_MACROS_H
#define ZCL_LOG_MACROS_H

#include <stdio.h>

/* ── Core: log context + return ──────────────────────────────────── */

/* Log error and return false. Use in functions returning bool. */
#define LOG_FAIL(domain, fmt, ...) do { \
    fprintf(stderr, "[%s] %s:%d %s(): " fmt "\n", \
            (domain), __FILE__, __LINE__, __func__, ##__VA_ARGS__); \
    return false; \
} while (0)

/* Log error and return -1. Use in MCP handlers / int-returning funcs. */
#define LOG_ERR(domain, fmt, ...) do { \
    fprintf(stderr, "[%s] %s:%d %s(): " fmt "\n", \
            (domain), __FILE__, __LINE__, __func__, ##__VA_ARGS__); \
    return -1; \
} while (0)

/* Log error and return NULL. Use in pointer-returning functions. */
#define LOG_NULL(domain, fmt, ...) do { \
    fprintf(stderr, "[%s] %s:%d %s(): " fmt "\n", \
            (domain), __FILE__, __LINE__, __func__, ##__VA_ARGS__); \
    return NULL; \
} while (0)

/* Log error and return a custom value. */
#define LOG_RETURN(val, domain, fmt, ...) do { \
    fprintf(stderr, "[%s] %s:%d %s(): " fmt "\n", \
            (domain), __FILE__, __LINE__, __func__, ##__VA_ARGS__); \
    return (val); \
} while (0)

/* ── Non-returning level logs ────────────────────────────────────
 *
 * The macros above LOG-and-RETURN (for error sites). These LOG and
 * CONTINUE — for the common case of a diagnostic that must NOT abort
 * the caller: progress, best-effort cleanup, and warnings on a
 * log-and-continue path. They replace raw `fprintf(stderr, ...)` (often
 * marked `// obs-ok:`) so node.log is uniform and `zcl_node_log` can
 * filter by level.
 *
 * Same `[domain] LEVEL file:line func():` prefix the returning macros
 * use; the level token matches nodelog_controller's filter
 * (`WARN:` → warn; bare → info; the returning macros cover error/fatal).
 *
 *   // Instead of: fprintf(stderr, "[net] short write peer=%d\n", id);
 *   LOG_WARN("net", "short write peer=%d", id);
 */

/* Log a warning and continue (no return). */
#define LOG_WARN(domain, fmt, ...) do { \
    fprintf(stderr, "[%s] WARN: %s:%d %s(): " fmt "\n", \
            (domain), __FILE__, __LINE__, __func__, ##__VA_ARGS__); \
} while (0)

/* Log an informational line and continue (no return). */
#define LOG_INFO(domain, fmt, ...) do { \
    fprintf(stderr, "[%s] INFO %s:%d %s(): " fmt "\n", \
            (domain), __FILE__, __LINE__, __func__, ##__VA_ARGS__); \
} while (0)

/* ── Guards: check condition, log + return on failure ─────────── */

/* Guard: if condition is false, log and return false. */
#define GUARD(cond, domain, fmt, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "[%s] %s:%d %s(): GUARD FAILED: " fmt "\n", \
                (domain), __FILE__, __LINE__, __func__, ##__VA_ARGS__); \
        return false; \
    } \
} while (0)

/* Guard: if pointer is NULL, log and return false. */
#define GUARD_NOT_NULL(ptr, domain, label) do { \
    if (!(ptr)) { \
        fprintf(stderr, "[%s] %s:%d %s(): %s is NULL\n", \
                (domain), __FILE__, __LINE__, __func__, (label)); \
        return false; \
    } \
} while (0)

/* Guard: if pointer is NULL, log and return NULL. */
#define GUARD_NOT_NULL_RET_NULL(ptr, domain, label) do { \
    if (!(ptr)) { \
        fprintf(stderr, "[%s] %s:%d %s(): %s is NULL\n", \
                (domain), __FILE__, __LINE__, __func__, (label)); \
        return NULL; \
    } \
} while (0)

/* Guard: if pointer is NULL, log and return -1. */
#define GUARD_NOT_NULL_ERR(ptr, domain, label) do { \
    if (!(ptr)) { \
        fprintf(stderr, "[%s] %s:%d %s(): %s is NULL\n", \
                (domain), __FILE__, __LINE__, __func__, (label)); \
        return -1; \
    } \
} while (0)

#endif /* ZCL_LOG_MACROS_H */
