/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: bounded, reviewed native desktop presentation process boundary. */

#ifndef ZCL_VIEWS_UI_PRESENT_H
#define ZCL_VIEWS_UI_PRESENT_H

#include "base/result.h"
#include "encoding/qr.h"

#include <stdbool.h>
#include <stddef.h>

#define UI_PRESENT_REQUEST_MAX 4096u
#define UI_PRESENT_TITLE_MAX 80u

struct ui_present_qr_request {
    char payload[ZCL_QR_MAX_PAYLOAD + 1u];
    char title[UI_PRESENT_TITLE_MAX + 1u];
};

/* Parse and revalidate the private stdin document used by the presentation
 * child. Exposed so malformed, oversized, and valid requests are testable
 * without opening a desktop window. */
bool ui_present_qr_request_parse(const char *raw, size_t raw_len,
                                 struct ui_present_qr_request *out,
                                 char *error, size_t error_cap);

/* Launch one detached copy of this exact executable. The QR payload is sent
 * through stdin, never argv or the environment. Success means exec and stdin
 * delivery succeeded; the window owns its lifecycle after that hand-off. */
struct zcl_result ui_present_qr_launch(const char *payload,
                                        const char *title);

/* Internal, exact-flag entry point used only by src/main.c. `kind` is an
 * allowlisted presentation type, never an executable or shell command. */
int ui_present_child_main(const char *kind);

#endif /* ZCL_VIEWS_UI_PRESENT_H */
