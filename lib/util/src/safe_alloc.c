/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "util/safe_alloc.h"

#include <stdatomic.h>
#include <string.h>

static _Atomic(const char *) g_alloc_fault_site = NULL;

void zcl_alloc_fault_fail_next(const char *label)
{
    atomic_store(&g_alloc_fault_site, label && *label ? label : NULL);
}

void zcl_alloc_fault_clear(void)
{
    atomic_store(&g_alloc_fault_site, NULL);
}

const char *zcl_alloc_fault_armed_label(void)
{
    return atomic_load(&g_alloc_fault_site);
}

bool zcl_alloc_fault_should_fail(const char *label)
{
    if (!label || !*label) return false;
    const char *armed = atomic_load(&g_alloc_fault_site);
    if (!armed || strcmp(armed, label) != 0) return false;
    return atomic_compare_exchange_strong(&g_alloc_fault_site, &armed, NULL);
}
