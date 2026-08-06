/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: local native presentation of bounded QR payloads. */

#ifndef ZCL_VIEWS_QR_POPUP_H
#define ZCL_VIEWS_QR_POPUP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZCL_QR_POPUP_CARD_WIDTH 440u
#define ZCL_QR_POPUP_CARD_HEIGHT 660u
#define ZCL_QR_POPUP_CARD_BYTES \
    (ZCL_QR_POPUP_CARD_WIDTH * ZCL_QR_POPUP_CARD_HEIGHT * 3u)

struct qr_popup_card {
    uint8_t *pixels;
    uint32_t width;
    uint32_t height;
    bool is_deposit;
    char address[192];
    char amount[64];
};

/* Compose the exact pixels shown by qr_popup_show(). The caller owns pixels
 * and releases them with qr_popup_card_free(). This separated compositor is
 * what keeps visual iteration and deterministic tests out of the window loop. */
bool qr_popup_card_render(const char *payload, const char *title,
                          struct qr_popup_card *out,
                          char *error, size_t error_cap);
void qr_popup_card_free(struct qr_popup_card *card);

/* Opens one detached-process desktop window through lib/presentation and runs
 * until the user closes it. No node, wallet, or network state is touched. */
bool qr_popup_show(const char *payload, const char *title,
                   char *error, size_t error_cap);

#endif
