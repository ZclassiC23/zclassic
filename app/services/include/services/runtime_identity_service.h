/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_SERVICES_RUNTIME_IDENTITY_SERVICE_H
#define ZCL_SERVICES_RUNTIME_IDENTITY_SERVICE_H

#include <stdint.h>

#define RUNTIME_INSTANCE_ID_MAX 160

struct runtime_identity_snapshot {
    int64_t process_id;
    int64_t initialized_at_unix_us;
    int64_t initialized_at_monotonic_us;
    char instance_id[RUNTIME_INSTANCE_ID_MAX];
};

/* Stable for the process lifetime. Thread-safe and initialized before the
 * caller's first capture window starts. The random nonce prevents PID reuse
 * from aliasing a restarted node. */
void runtime_identity_get(struct runtime_identity_snapshot *out);

#endif
