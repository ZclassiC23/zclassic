/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * reducer_drive_guard — see util/reducer_drive_guard.h. */

#include "util/reducer_drive_guard.h"
#include <stdatomic.h>

/* Number of synchronous reducer drives currently in progress (normally 0 or 1;
 * a counter rather than a bool so a future nested drive cannot clear the flag
 * early). */
static _Atomic int g_reducer_drive_depth = 0;

void reducer_drive_enter(void)
{
    atomic_fetch_add_explicit(&g_reducer_drive_depth, 1, memory_order_acq_rel);
}

void reducer_drive_exit(void)
{
    atomic_fetch_sub_explicit(&g_reducer_drive_depth, 1, memory_order_acq_rel);
}

bool reducer_drive_active(void)
{
    return atomic_load_explicit(&g_reducer_drive_depth, memory_order_acquire) > 0;
}
