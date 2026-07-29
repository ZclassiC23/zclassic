/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Wallet Recovery Service — rebuild a wallet from its recovery phrase alone.
 *
 * What it is for
 * --------------
 * The disaster this answers is the total one: the machine is gone, the
 * backup file is gone with it, and all the user has is the twelve words
 * they wrote on paper when the wallet was created. Those words determine
 * the wallet's single 32-byte seed (mnemonic_to_wallet_seed), and that seed
 * determines every key the wallet ever mints — the transparent chain
 * through BIP32/BIP44 and the shielded chain through ZIP32. So the words
 * are enough, and this service is what turns them back into a datadir.
 *
 * It runs OFFLINE, in-process, against a datadir on disk. Asking the user
 * to start a node first would ask them to boot the very wallet they are
 * trying to put back.
 *
 * The three things it refuses to do
 * ---------------------------------
 * 1. Touch a datadir a node is holding — the same <datadir>/zclassic23.pid
 *    single-writer proof wallet_restore_datadir_free() applies.
 * 2. Recover into a datadir that already holds a wallet. Keys already there
 *    were minted from some other seed; installing a second seed over them
 *    would leave a wallet whose keys half descend from the phrase and half
 *    do not, and whose owner has no way to tell which. Recover into an
 *    empty datadir.
 * 3. Store, log, echo or return the phrase. The report carries only public
 *    addresses. The phrase is cleansed out of every buffer this service
 *    touches before it returns.
 *
 * What comes back, and what does not
 * ----------------------------------
 * The keys come back — the same addresses, the same spending authority.
 * The HISTORY does not: balances, transactions and shielded note witnesses
 * are chain state, and the node rebuilds them by scanning the chain after
 * the wallet is in place. next_steps in the command output names that.
 */

#ifndef ZCL_SERVICES_WALLET_RECOVERY_SERVICE_H
#define ZCL_SERVICES_WALLET_RECOVERY_SERVICE_H

#include "util/result.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct node_db;

struct wallet_recovery_request {
    const char *phrase;    /* BIP39 recovery phrase; required. Never stored. */
    const char *datadir;   /* target datadir; required */
    bool        dry_run;   /* derive and report, write nothing */
};

struct wallet_recovery_report {
    char    datadir[1024];
    char    target_db[1024];
    bool    dry_run;
    bool    datadir_created;      /* the directory did not exist */
    bool    target_created;       /* node.db did not exist */
    bool    phrase_valid;
    int     word_count;
    int64_t keys_before;          /* wallet_keys rows in the target */
    int64_t keys_after;
    bool    seed_present_before;  /* target already had a wallet_seed row */
    bool    seed_installed;       /* the phrase's seed is now on disk */
    /* PUBLIC addresses the phrase reproduces — safe to print, and the one
     * thing a user can check against what they remember. */
    char    first_address[128];
    char    first_shielded_address[160];
    int     keys_minted;
};

/* Recover a wallet from `req->phrase` into `req->datadir`.
 *
 * A dry run derives the addresses and reports every refusal it would hit,
 * writing nothing — so the plan a caller shows is exactly what the commit
 * will do. A committed run mints the standard keypool from the phrase's
 * seed and flushes it through the wallet's single writer.
 *
 * Fills `out` on every path, including failures, so a refusal is still
 * informative. Returns ZCL_OK on success; on refusal the code is
 *   -60 invalid or missing phrase        -63 cannot open the target db
 *   -61 datadir held by a running node   -64 key derivation failed
 *   -62 datadir already holds a wallet   -65 flush failed
 */
struct zcl_result wallet_recovery_run(const struct wallet_recovery_request *req,
                                      struct wallet_recovery_report *out);

/* ── status: a strictly READ-ONLY question, in two halves ───────────
 *
 * `core.wallet.recovery.status` is declared a READ leaf, and its `datadir`
 * defaults to the operator's LIVE one when the caller names none. So the
 * answer has to come out of node.db without the file being able to change:
 * the caller opens the database READ-ONLY itself
 * (zcl_native_node_db_open_readonly, tools/command/native_node_db_ro.c —
 * SQLITE_OPEN_READONLY + PRAGMA query_only=ON, no CREATE, no migrate, no
 * quarantine) and hands the handle in. This service no longer opens
 * anything, which is what keeps the boot ceremony — and its
 * node.db.corrupt-<ts> rename — off a read path for good.
 *
 * Splitting the call in two is not decoration: the preflight has to run
 * BEFORE anything opens the datadir, and the open belongs to the caller. */

/* Half one — refuse before the datadir is opened at all. Resets `out` and
 * fills its datadir/target_db paths, then proves no node holds the datadir
 * (-61 if one does). Kept even though the read is now read-only: a node
 * mid-migration is rewriting the very wallet tables this answer is read
 * out of, and "your wallet cannot be rebuilt from words" is not a sentence
 * to derive from a half-written schema. */
struct zcl_result wallet_recovery_status_preflight(
    const char *datadir, struct wallet_recovery_report *out);

/* Half two — answer from an ALREADY-OPEN, read-only node.db. `ndb` must be
 * open; this call writes nothing to it and does not close it.
 *
 * This is the honest answer to "can I restore this wallet from words?" — a
 * wallet created before recovery phrases has independently random
 * transparent keys and the answer is no, use a file backup.
 *
 * Fills `out`; `phrase_valid` is unused here.
 * `seed_installed` reports whether the wallet's keys descend from its seed,
 * i.e. whether a recovery phrase would bring them back.
 *
 * Codes: -63 no usable handle / unreadable wallet tables. */
struct zcl_result wallet_recovery_status(const char *datadir,
                                         struct node_db *ndb,
                                         struct wallet_recovery_report *out);

#endif /* ZCL_SERVICES_WALLET_RECOVERY_SERVICE_H */
