/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native Tier-1 hot-swap command glue. The resident activation path is kept
 * separate from the CLI probe so release builds link only a contained stub.
 *
 * dev.hotswap.probe and dev.hotswap.apply are hard-contained until a
 * disposable probe worker, pre-load ELF admission, immutable artifacts, and
 * the complete source/proof/rollback transaction exist. This file declares
 * the resident typed-refusal RPC registration; CLI handlers are declared in
 * command/native_command.h under ZCL_DEV_BUILD. */

#ifndef ZCL_TOOLS_NATIVE_DEV_HOTSWAP_H
#define ZCL_TOOLS_NATIVE_DEV_HOTSWAP_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rpc_table;

/* Register the resident-node RPC method `dev_hotswap_native` on `table`.
 * DEV-ONLY, and a successful no-op on a release build or a non-dev-lane
 * datadir (returns true without registering).
 * Called once at boot from config/src/boot_services.c. Returns false only if a
 * required registration could not be completed. */
bool register_dev_native_hotswap_rpc(struct rpc_table *table,
                                     const char *datadir, int rpc_port);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_TOOLS_NATIVE_DEV_HOTSWAP_H */
