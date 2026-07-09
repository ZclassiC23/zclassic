/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * _shim.c — the one process-global symbol every example needs but none of
 * them defines: `g_shutdown_requested`.
 *
 * In the real node it's defined once in src/main.c (and, for the test
 * harness, once in lib/test/src/test_parallel.c / lib/test/src/test.c) and
 * declared `extern` by a handful of library TUs that check it during a
 * graceful-shutdown-aware loop (see lib/validation/src/process_block_internal.h,
 * lib/net/src/msg_headers.c, app/services/src/node_db_catchup_service.c,
 * etc). The examples link against the library object tree but deliberately
 * exclude lib/test/src/test_parallel.o (it owns its own main(), which would
 * collide with each example's main()) — so this shim provides the same
 * definition, in the same never-set state (0 == "no shutdown requested"),
 * for every example binary. This file is examples-only; it is not part of
 * the node or test build.
 */

#include <signal.h>

volatile sig_atomic_t g_shutdown_requested = 0;
