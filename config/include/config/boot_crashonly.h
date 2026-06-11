/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot_crashonly — crash-only recovery decisions for boot's post-restore
 * integrity gate, factored out of the boot orchestrator (boot.c) so it stays
 * lean. When a derived tip is installed above the validated on-disk index
 * extent, the cure is to RE-DERIVE the UTXO set via -reindex-chainstate (rewind
 * to the consistent target + replay from blocks/), not to FATAL or surgically
 * repair. See storage/boot_auto_reindex.h for the durable, bounded request.
 */

#ifndef ZCL_CONFIG_BOOT_CRASHONLY_H
#define ZCL_CONFIG_BOOT_CRASHONLY_H

#include <stdbool.h>

/* Consume a pending self-rebuild request (logs when it does). Returns true if
 * the next boot should run -reindex-chainstate. */
bool boot_crashonly_consume_reindex_request(const char *datadir);

/* Clear the self-rebuild budget once the boot reaches a clean integrity state. */
void boot_crashonly_clear(const char *datadir);

/* Handle a post-restore CHAIN_INTEGRITY_UNRECOVERABLE verdict. If it is the
 * reindex-recoverable shape (zero_nbits==0: holes only ABOVE the validated
 * index extent), the verb is executable (reindex_executable: the caller
 * verified blocks/ can serve the replay start), and the bounded budget
 * allows, request a self-rebuild and log it.
 * Returns true  => the caller must exit boot (reindex requested, budget
 *                  exhausted page, or structural-corruption FATAL).
 * Returns false => replay-from-blocks/ is not executable on this datadir
 *                  (cold-import window): no sentinel was written, the
 *                  cold_import_reseed_required page fired, and the caller
 *                  must KEEP SERVING degraded instead of crash-looping
 *                  into an impossible rebuild (defect #6). */
bool boot_crashonly_handle_unrecoverable(const char *datadir, int tip_h,
                                         int zero_nbits, int mismatches,
                                         int first_mismatch_h,
                                         bool reindex_executable);

#endif /* ZCL_CONFIG_BOOT_CRASHONLY_H */
