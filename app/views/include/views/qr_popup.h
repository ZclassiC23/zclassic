/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: local native GTK presentation of bounded QR payloads. */

#ifndef ZCL_VIEWS_QR_POPUP_H
#define ZCL_VIEWS_QR_POPUP_H

#include <stdbool.h>
#include <stddef.h>

/* Opens one local modal-independent desktop window and runs its GTK loop until
 * the user closes it. No node, wallet, or network state is touched. */
bool qr_popup_show(const char *payload, const char *title,
                   char *error, size_t error_cap);

#endif
