/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_FRAMEWORK_APP_PLATFORM_H
#define ZCL_FRAMEWORK_APP_PLATFORM_H

#include "zclassic23/app.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Core-side fail-closed validator for an app generation. `why` is always
 * populated on failure and never contains app-controlled format strings. */
bool zcl_app_manifest_v1_validate(const struct zcl_app_manifest_v1 *manifest,
                                  uint64_t host_capabilities,
                                  const char *expected_build_identity,
                                  char *why,
                                  size_t why_sz);

const char *zcl_app_capability_name(uint64_t one_capability);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_FRAMEWORK_APP_PLATFORM_H */
