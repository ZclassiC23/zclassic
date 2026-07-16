/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * See lib/util/include/util/log_level.h for the contract. */

#include "util/log_level.h"

#include <stdatomic.h>
#include <string.h>

static atomic_int g_zcl_log_level = ZCL_LOG_ALL;

void zcl_log_level_set(enum zcl_log_level level)
{
    atomic_store_explicit(&g_zcl_log_level, (int)level, memory_order_relaxed);
}

enum zcl_log_level zcl_log_level_get(void)
{
    return (enum zcl_log_level)
        atomic_load_explicit(&g_zcl_log_level, memory_order_relaxed);
}

bool zcl_log_level_from_string(const char *s, enum zcl_log_level *out)
{
    if (!s || !out)
        return false;

    if (!strcmp(s, "all"))   { *out = ZCL_LOG_ALL;   return true; }
    if (!strcmp(s, "info"))  { *out = ZCL_LOG_INFO;  return true; }
    if (!strcmp(s, "warn"))  { *out = ZCL_LOG_WARN;  return true; }
    if (!strcmp(s, "error")) { *out = ZCL_LOG_ERROR; return true; }
    if (!strcmp(s, "fatal")) { *out = ZCL_LOG_FATAL; return true; }
    if (!strcmp(s, "off"))   { *out = ZCL_LOG_OFF;   return true; }

    return false;
}
