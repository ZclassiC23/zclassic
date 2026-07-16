/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Declare the durable dev failure receipt and lookup API. */

#ifndef ZCL_TOOLS_DEV_FAILURE_STORE_H
#define ZCL_TOOLS_DEV_FAILURE_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZCL_DEV_FAILURE_HEX_LEN 64
#define ZCL_DEV_FAILURE_PHASE_MAX 64
#define ZCL_DEV_FAILURE_ERROR_MAX 512
#define ZCL_DEV_FAILURE_CAPSULE_MAX 1024
#define ZCL_DEV_FAILURE_RETRY_MAX 160

/* Immutable base record plus its SHA3-sealed, atomically replaced observation
 * counter. `repeat_count` includes the original observed failure, so it is
 * always at least one for a successfully read record. */
struct zcl_dev_failure_record {
    char failure_id[ZCL_DEV_FAILURE_HEX_LEN + 1];
    char workspace_id[ZCL_DEV_FAILURE_HEX_LEN + 1];
    char source_id[ZCL_DEV_FAILURE_HEX_LEN + 1];
    char first_source_mutation[ZCL_DEV_FAILURE_HEX_LEN + 1];
    char first_execution_id[ZCL_DEV_FAILURE_HEX_LEN + 1];
    char record_digest[ZCL_DEV_FAILURE_HEX_LEN + 1];
    char phase[ZCL_DEV_FAILURE_PHASE_MAX];
    char first_error[ZCL_DEV_FAILURE_ERROR_MAX];
    char capsule[ZCL_DEV_FAILURE_CAPSULE_MAX];
    char retry_command[ZCL_DEV_FAILURE_RETRY_MAX];
    int64_t first_seen_unix_ms;
    uint64_t repeat_count;
};

/* Read APIs distinguish an ordinary miss from corrupt/untrusted state.  A
 * caller may report ABSENT as an honest empty result; INVALID must never be
 * rendered as a successful "not found" response. */
enum zcl_dev_failure_lookup {
    ZCL_DEV_FAILURE_LOOKUP_INVALID = -1,
    ZCL_DEV_FAILURE_LOOKUP_ABSENT = 0,
    ZCL_DEV_FAILURE_LOOKUP_FOUND = 1
};

/* Collapse whitespace/control noise into one bounded, stable line. */
bool zcl_dev_failure_normalize_error(const char *input,
                                     char out[ZCL_DEV_FAILURE_ERROR_MAX]);

/* SHA3-256 over labeled NUL-delimited fields, exactly:
 * domain\0zcl.dev_failure_id.v1\0source_id_sha256\0<source>\0
 * phase\0<phase>\0first_error\0<normalized-error>\0. */
bool zcl_dev_failure_compute_id(
    const char source_id[ZCL_DEV_FAILURE_HEX_LEN + 1],
    const char *phase, const char *first_error,
    char out[ZCL_DEV_FAILURE_HEX_LEN + 1]);

/* Record one actually executed deterministic failure.  The immutable base is
 * created with no-replace publication; an identical already-present failure
 * advances its bounded sealed observation counter. The workspace is derived from the
 * canonical repo_root, so parallel worktrees never share `latest`. */
bool zcl_dev_failure_record_failure(
    const char *repo_root,
    const char source_id[ZCL_DEV_FAILURE_HEX_LEN + 1],
    const char source_mutation[ZCL_DEV_FAILURE_HEX_LEN + 1],
    const char execution_id[ZCL_DEV_FAILURE_HEX_LEN + 1],
    const char *phase, const char *first_error, const char *capsule,
    const char *retry_command, struct zcl_dev_failure_record *out,
    char *why, size_t why_len);

/* Return true only when the workspace's atomic latest pointer resolves to an
 * intact record whose source, ABA mutation token, execution/toolchain epoch,
 * and phase all match exactly.  A mismatch/miss is ordinary false with an
 * empty `why`; malformed/tampered storage is false with a diagnostic. */
bool zcl_dev_failure_match_latest(
    const char *repo_root,
    const char source_id[ZCL_DEV_FAILURE_HEX_LEN + 1],
    const char source_mutation[ZCL_DEV_FAILURE_HEX_LEN + 1],
    const char execution_id[ZCL_DEV_FAILURE_HEX_LEN + 1],
    const char *phase, struct zcl_dev_failure_record *out,
    char *why, size_t why_len);

/* Note that a matching failure was deliberately coalesced without executing
 * the compiler/test process, then return the new durable sealed count. */
bool zcl_dev_failure_note_coalesced(
    const char *repo_root,
    const char failure_id[ZCL_DEV_FAILURE_HEX_LEN + 1],
    const char source_id[ZCL_DEV_FAILURE_HEX_LEN + 1],
    const char source_mutation[ZCL_DEV_FAILURE_HEX_LEN + 1],
    const char execution_id[ZCL_DEV_FAILURE_HEX_LEN + 1],
    const char *phase,
    struct zcl_dev_failure_record *out, char *why, size_t why_len);

/* Read one immutable failure by durable ID and verify its SHA3 seal. */
enum zcl_dev_failure_lookup zcl_dev_failure_read(
    const char *repo_root,
    const char failure_id[ZCL_DEV_FAILURE_HEX_LEN + 1],
    struct zcl_dev_failure_record *out, char *why, size_t why_len);

/* Resolve the workspace-scoped atomic latest pointer, then perform the same
 * sealed-record verification as zcl_dev_failure_read(). */
enum zcl_dev_failure_lookup zcl_dev_failure_read_latest(
    const char *repo_root, struct zcl_dev_failure_record *out,
    char *why, size_t why_len);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_TOOLS_DEV_FAILURE_STORE_H */
