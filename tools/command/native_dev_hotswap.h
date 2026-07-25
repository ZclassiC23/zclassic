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
struct hotswap_publish_hooks;

/* Register the resident-node RPC method `dev_hotswap_native` on `table`.
 * DEV-ONLY, and a successful no-op on a release build or a non-dev-lane
 * datadir (returns true without registering).
 * Called once at boot from config/src/boot_services.c. Returns false only if a
 * required registration could not be completed. */
bool register_dev_native_hotswap_rpc(struct rpc_table *table,
                                     const char *datadir, int rpc_port);

#ifdef ZCL_DEV_BUILD
/* Fill `out` with THE canonical publish hooks for the command registry in this
 * process: the all-or-nothing batch commit, the probe-before-publish dispatch
 * (public spec lookup + input validation + declared-output-schema check), and
 * optionally the retired-snapshot quiesce poll that gates dlclose.
 *
 * `with_quiesce` is false for a one-shot CLI (nothing to reclaim; the process
 * exits) and true in the resident node. Shared by the resident RPC, the CLI
 * `dev hotswap probe`, and the ZCL_HOTSWAP_PRELOAD path in native_command.c so
 * exactly one implementation of "how a candidate is validated and published"
 * exists. DEV-ONLY, like every other symbol on this path. */
void zcl_native_hotswap_publish_hooks(struct hotswap_publish_hooks *out,
                                      bool with_quiesce);
#endif /* ZCL_DEV_BUILD */

#ifdef __cplusplus
}
#endif

#endif /* ZCL_TOOLS_NATIVE_DEV_HOTSWAP_H */
