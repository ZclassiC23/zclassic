/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Capture and verify the canonical current source epoch for dev tools. */

#include "devloop.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static bool process_ok(const struct zcl_devloop_process_result *result)
{
    return result && !result->timed_out && result->term_signal == 0 &&
           result->exit_code == 0;
}

static bool lower_hex64(const char *input, char out[65])
{
    if (!input || strlen(input) != 64)
        return false;
    for (size_t i = 0; i < 64; i++) {
        if (!isxdigit((unsigned char)input[i]))
            return false;
        out[i] = (char)tolower((unsigned char)input[i]);
    }
    out[64] = '\0';
    return true;
}

static bool parse_source_record(const struct zcl_devloop_process_result *result,
                                struct dev_source_record *out,
                                char *why, size_t why_len)
{
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!result || !process_ok(result) || result->output_truncated) {
        (void)snprintf(why, why_len, "source_identity_command_failed");
        return false;
    }
    size_t len = result->output_len;
    while (len > 0 && (result->output[len - 1] == '\n' ||
                       result->output[len - 1] == '\r'))
        len--;
    if (len == 0 || len >= 160) {
        (void)snprintf(why, why_len, "source_identity_output_invalid");
        return false;
    }
    char body[160], source[65], complete[8], mutation[65], extra[2];
    memcpy(body, result->output, len);
    body[len] = '\0';
    int fields = sscanf(body, "%64s %7s %64s %1s", source, complete,
                        mutation, extra);
    if (fields != 3 || strcmp(complete, "1") != 0 ||
        !lower_hex64(source, out->source_id) ||
        !lower_hex64(mutation, out->mutation_id)) {
        (void)snprintf(why, why_len, "source_identity_output_invalid");
        return false;
    }
    return true;
}

bool zcl_dev_source_identity_capture(const char *repo_root,
                                     struct dev_source_record *out,
                                     char *why, size_t why_len)
{
    char tool[PATH_MAX];
    int n = snprintf(tool, sizeof(tool), "%s/tools/dev/source-identity.sh",
                     repo_root ? repo_root : "");
    if (n <= 0 || (size_t)n >= sizeof(tool)) {
        (void)snprintf(why, why_len, "source_identity_tool_path_invalid");
        return false;
    }
    struct zcl_devloop_process_result result = {0};
    const char *argv[] = { tool, "capture-record", NULL };
    if (!zcl_devloop_process_run(repo_root, argv, 30000, &result)) {
        (void)snprintf(why, why_len, "source_identity_capture_failed");
        return false;
    }
    if (parse_source_record(&result, out, why, why_len))
        return true;
    if (result.output_len > 0 && why && why_len > 0) {
        size_t copy = result.output_len < why_len - 1 ? result.output_len
                                                       : why_len - 1;
        memcpy(why, result.output, copy);
        why[copy] = '\0';
    }
    return false;
}

bool zcl_dev_source_identity_verify(const char *repo_root,
                                    const struct dev_source_record *expected,
                                    char *why, size_t why_len)
{
    if (!expected) {
        (void)snprintf(why, why_len, "source_identity_expected_missing");
        return false;
    }
    char tool[PATH_MAX];
    int n = snprintf(tool, sizeof(tool), "%s/tools/dev/source-identity.sh",
                     repo_root ? repo_root : "");
    if (n <= 0 || (size_t)n >= sizeof(tool)) {
        (void)snprintf(why, why_len, "source_identity_tool_path_invalid");
        return false;
    }
    struct zcl_devloop_process_result result = {0};
    const char *argv[] = { tool, "verify-record", expected->source_id, "1",
                           expected->mutation_id, NULL };
    if (!zcl_devloop_process_run(repo_root, argv, 30000, &result) ||
        !process_ok(&result)) {
        (void)snprintf(why, why_len, "source_epoch_superseded");
        return false;
    }
    struct dev_source_record actual;
    return parse_source_record(&result, &actual, why, why_len) &&
           strcmp(actual.source_id, expected->source_id) == 0 &&
           strcmp(actual.mutation_id, expected->mutation_id) == 0;
}

bool zcl_dev_source_mutation_verify(const char *repo_root,
                                    const struct dev_source_record *expected,
                                    char *why, size_t why_len)
{
    if (!expected) {
        (void)snprintf(why, why_len, "source_identity_expected_missing");
        return false;
    }
    char tool[PATH_MAX];
    int n = snprintf(tool, sizeof(tool), "%s/tools/dev/source-identity.sh",
                     repo_root ? repo_root : "");
    if (n <= 0 || (size_t)n >= sizeof(tool)) {
        (void)snprintf(why, why_len, "source_identity_tool_path_invalid");
        return false;
    }
    struct zcl_devloop_process_result result = {0};
    const char *argv[] = {tool, "verify-mutation", expected->mutation_id,
                          NULL};
    if (!zcl_devloop_process_run(repo_root, argv, 30000, &result) ||
        !process_ok(&result) || result.output_truncated) {
        (void)snprintf(why, why_len, "source_epoch_superseded");
        return false;
    }
    size_t len = result.output_len;
    while (len > 0 && (result.output[len - 1] == '\n' ||
                       result.output[len - 1] == '\r'))
        len--;
    if (len != 64) {
        (void)snprintf(why, why_len, "source_mutation_output_invalid");
        return false;
    }
    char raw[65], actual[65];
    memcpy(raw, result.output, 64);
    raw[64] = '\0';
    if (!lower_hex64(raw, actual) ||
        strcmp(actual, expected->mutation_id) != 0) {
        (void)snprintf(why, why_len, "source_epoch_superseded");
        return false;
    }
    return true;
}

bool zcl_dev_executable_source_id(const char *cwd, int executable_fd,
                                  const char *display_path, char out[65])
{
    if (!cwd || executable_fd < 0 || !display_path || !out)
        return false;
    struct zcl_devloop_process_result result = {0};
    const char *argv[] = {display_path, "--source-id", NULL};
    if (!zcl_devloop_process_run_fd(cwd, executable_fd, argv, 5000,
                                    &result) ||
        !process_ok(&result) || result.output_truncated)
        return false;
    size_t len = result.output_len;
    while (len > 0 && (result.output[len - 1] == '\n' ||
                       result.output[len - 1] == '\r'))
        len--;
    if (len != 64)
        return false;
    char raw[65];
    memcpy(raw, result.output, 64);
    raw[64] = '\0';
    return lower_hex64(raw, out);
}
