/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Shared native-command adapter for the one local sovereignty policy. */
#ifndef ZCL_TOOLS_NATIVE_ZCODE_POLICY_H
#define ZCL_TOOLS_NATIVE_ZCODE_POLICY_H

#include "vcs/zcode_sovereignty_policy.h"

#include <stdbool.h>
#include <stddef.h>

/* Load the policy bound to the running daemon's network and evaluate one
 * public object subject. No rule contents or identifiers leave this helper. */
bool zcl_native_zcode_policy_allows(
    const char *datadir, enum vcs_zcode_sovereignty_action action,
    const struct vcs_zcode_sovereignty_subject *subject,
    char *error_out, size_t error_capacity);

#endif
