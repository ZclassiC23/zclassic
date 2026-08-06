/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Canonical persistent Noise X25519 static-key loader. */

#ifndef ZCL_NET_V2_IDENTITY_H
#define ZCL_NET_V2_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Load <datadir>/v2_identity.key, creating it atomically with mode 0600 when
 * absent.  Existing malformed, short, or over-permissive files fail closed
 * and are never overwritten.  The caller owns private_out and must cleanse it. */
bool v2_identity_load_or_create(const char *datadir,
                                uint8_t private_out[32],
                                uint8_t public_out[32],
                                char *error_out, size_t error_capacity);

#endif /* ZCL_NET_V2_IDENTITY_H */
