/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * bundle_exporter — the STANDING live consensus-state bundle exporter (lane C1
 * of the Instant-Sync program). On a qualified serving node it holds a producer
 * receipt session open (so the live reducer keeps stamping script_validate /
 * proof_validate rows with THIS binary's source epoch) and runs a supervised
 * periodic job that, every N durable blocks, monotonic-re-finalizes the source
 * receipt at the current durable tip and exports a fresh, fully fail-closed
 * zcl.consensus_state_bundle.v1 into <datadir>/bundles/, keeping K generations.
 *
 * The export runs against a PRIVATE read-only WAL snapshot of progress.kv
 * (consensus_state_snapshot_export_from_progress_snapshot) so the reducer is
 * never stalled for the proof + copy.
 *
 * WHAT THIS PROVES: a bundle from a SELF-FOLDED, single-binary generation —
 * genesis..H* all validated FULL by THIS running binary with the session held
 * open the whole time. It does NOT (yet) produce bundles from a bundle-INSTALLED
 * node: the A3 activate install clears the script/proof validate logs below the
 * install height, so those rows are absent and the genesis..H* stage-row proof
 * cannot be reconstructed without base-evidence composition (a documented
 * follow-up that needs coordination with the A3 install to record the installed
 * bundle's proof digest as base evidence).
 *
 * FAIL-SAFE: nothing here can fail a boot. When provenance does not qualify, the
 * session is not opened and the reason is a dumpstate-visible named degradation
 * (zcl_state subsystem=bundle_exporter). */

#ifndef ZCL_CONFIG_BUNDLE_EXPORTER_H
#define ZCL_CONFIG_BUNDLE_EXPORTER_H

#include <stdbool.h>

#include <sqlite3.h>

/* Open the producer session if this serving node's provenance qualifies, then
 * register the supervised rolling re-finalize+export job. Idempotent; returns
 * true when the job was registered (whether or not the session opened) and
 * false only on a hard wiring error — the boot caller ignores the result so a
 * degraded exporter never blocks boot. `datadir` must be the live datadir
 * (bundles are written under <datadir>/bundles/). `pdb` is the owned
 * progress.kv handle. */
bool bundle_exporter_start(sqlite3 *pdb, const char *datadir);

/* Stop ticking the supervised job (the durable session in progress.kv is left
 * intact). Safe to call repeatedly and from shutdown. */
void bundle_exporter_stop(void);

/* Register the exporter as an optional maintenance service on `kernel`
 * (ctx = the DATADIR string, like disk_monitor — NOT a DB handle; the
 * exporter writes <datadir>/bundles and reads the owned progress.kv).
 * Fail-safe: the service start records a dumpstate degradation and still
 * arms rather than failing, so a degraded exporter never blocks boot. */
struct zcl_service_kernel;
bool bundle_exporter_register_service(struct zcl_service_kernel *kernel,
                                      const char *datadir);

/* zcl_state subsystem=bundle_exporter (CLAUDE.md "Adding state introspection").
 * `out` is json_set_object'd by the caller; this also does it defensively.
 * `key` is unused. */
struct json_value;
bool bundle_exporter_dump_state_json(struct json_value *out, const char *key);

#endif /* ZCL_CONFIG_BUNDLE_EXPORTER_H */
