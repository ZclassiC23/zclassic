/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * evidence_ledger_row — the ONE reader for the flock-appended JSONL evidence
 * ledgers this repo's out-of-process recorders write under
 * ~/.local/state/zclassic23-*, so the node can describe evidence that exists
 * on disk without a second parser per ledger.
 *
 * WHY IT IS SHARED, not copied. Two in-node readers now exist over this same
 * file shape (services/stopwatch_skip_watch.h over the stopwatch history,
 * services/tip_agreement_watch.h over the off-host tip-hash agreement
 * ledger), and the subtle part is not the field extraction — it is the
 * BOUNDED TAIL READ:
 *
 *   - after a mid-file seek the first line is a FRAGMENT and must be dropped,
 *     never folded as a row (folding it invents a sample);
 *   - a row longer than the row buffer must have its tail CONSUMED, not
 *     folded as a second phantom row;
 *   - a missing or unreadable ledger is DATA (nothing scanned), never an
 *     error a caller branches on — a host that never installed a recorder
 *     looks exactly like that.
 *
 * Getting any of those three wrong fabricates evidence, so there is one
 * implementation and both readers call it.
 *
 * THE ROW SHAPE this supports, deliberately narrow: flat, single-line,
 * unnested JSON objects with no duplicate keys — the same assumption the
 * shell judges' fld_num()/fld_str() already make. It is NOT a JSON parser
 * and must not grow into one; a nested field belongs in json/json.h.
 *
 * Pure: no allocation, no clock, no globals, no threads. Reentrant-safe. */

#ifndef ZCL_SERVICES_EVIDENCE_LEDGER_ROW_H
#define ZCL_SERVICES_EVIDENCE_LEDGER_ROW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One ledger row is a flat JSON object well under 1 KiB; 4 KiB is generous.
 * A row longer than this is not an evidence row — see the overlong note in
 * evidence_ledger_scan_tail(). */
#define EVIDENCE_ROW_MAX 4096

/* Bounded, NUL-terminating copy of `len` bytes. A NULL/zero-cap dst is a
 * no-op; a NULL src writes the empty string. */
void evidence_copy_bounded(char *dst, size_t cap, const char *src, size_t len);

/* First occurrence of NUL-terminated `needle` inside [hay, hay+len), or NULL.
 * Substring search over a non-NUL-terminated span — that is why it is here
 * and not strstr(). */
const char *evidence_find_sub(const char *hay, size_t len, const char *needle);

/* Copy a string-valued field into dst (always NUL-terminated when cap > 0).
 * Returns true when the FIELD EXISTS AS A JSON STRING, even when empty:
 * callers must be able to tell "the recorder wrote an empty value" from
 * "this row predates the field". */
bool evidence_row_str(const char *row, size_t len, const char *key,
                      char *dst, size_t cap);

/* Read an integer-valued field. Returns false when the field is absent or
 * not a number — JSON `null` included, because every recorder here writes
 * null for "I could not measure this", and null is not a zero. */
bool evidence_row_int(const char *row, size_t len, const char *key,
                      int64_t *out);

/* Per-row callback. `row` is NOT NUL-terminated; use `len`. */
typedef void (*evidence_row_fn)(const char *row, size_t len, void *ctx);

/* Split an in-memory ledger (newline-separated rows) and hand each row to
 * `fn`. Pure: no IO, no clock. Returns false only on bad arguments. */
bool evidence_ledger_scan_text(const char *text, size_t len,
                              evidence_row_fn fn, void *ctx);

/* Read the trailing `tail_bytes` of `path` and hand each COMPLETE row to
 * `fn`, streaming so peak memory is one row rather than one tail.
 *
 * A missing / empty / unreadable file is NOT a failure: `fn` is simply never
 * called and the call returns true. Returns false only on bad arguments.
 *
 * `out_overlong` (may be NULL) is INCREMENTED once per row that did not fit
 * EVIDENCE_ROW_MAX. Such a row is dropped whole — its continuation bytes are
 * consumed rather than folded as a second row. Callers count it as malformed.
 *
 * The first line after a mid-file seek is a fragment and is dropped. */
bool evidence_ledger_scan_tail(const char *path, size_t tail_bytes,
                              evidence_row_fn fn, void *ctx,
                              unsigned *out_overlong);

/* Resolve "<dir>/<file>" where dir is $<dir_env> when set and non-empty,
 * else "$HOME/<home_rel_dir>". Returns false (and empties `out`) when there
 * is no env override and no HOME, or when the result would not fit. Shared so
 * a node-side reader and the recorder script read the SAME env var and the
 * SAME default — a reader pointed at a different path than the writer reports
 * "no evidence" forever. */
bool evidence_ledger_resolve_path(const char *dir_env, const char *home_rel_dir,
                                 const char *file, char *out, size_t cap);

#endif /* ZCL_SERVICES_EVIDENCE_LEDGER_ROW_H */
