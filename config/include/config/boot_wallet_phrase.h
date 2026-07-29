/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot_wallet_phrase — the recovery phrase a brand-new wallet is born with.
 *
 * Before this existed, the only backup this node could offer was a file:
 * lose the disk and the backup together and the money was gone. A wallet
 * created now is born from twelve words instead. The words are shown once,
 * at creation, and never again — the node keeps only the 32-byte seed they
 * derive, and a seed cannot be turned back into words. That is the whole
 * point: if the phrase could be reprinted, every read of node.db would be a
 * total compromise of the wallet.
 *
 * Split out of boot.c so the boot function stays readable and so the
 * once-only print has exactly one implementation.
 */

#ifndef ZCL_CONFIG_BOOT_WALLET_PHRASE_H
#define ZCL_CONFIG_BOOT_WALLET_PHRASE_H

#include <stdbool.h>
#include <stddef.h>

struct wallet;
struct wallet_sqlite;
struct node_db;

/* Buffer a caller must provide for a phrase. */
#include "wallet/mnemonic.h"
#define BOOT_WALLET_PHRASE_CAP MNEMONIC_MAX_PHRASE_SIZE

/* Draw a fresh 12-word BIP39 phrase and make `w` descend from it: the
 * Sapling (ZIP32) and transparent (BIP32) trees are both re-rooted on the
 * seed the phrase derives, so every key minted afterwards is recoverable
 * from the words alone. Call on a wallet with NO keys yet.
 *
 * On success the phrase is written to `phrase_out` — the caller owns it,
 * must show it once, and must memory_cleanse() it. On failure `phrase_out`
 * is set to the empty string and the wallet is left untouched (it will mint
 * legacy random keys, which is exactly the old behaviour).
 *
 * The phrase is never logged and never written to disk by this call. */
bool boot_wallet_mint_recovery_phrase(struct wallet *w, char *phrase_out,
                                      size_t cap);

/* Print the once-only phrase block to stdout: the words, what they are for,
 * and the fact that no command can ever show them again. Call ONLY after
 * the wallet's seed has been durably flushed — showing a phrase for a
 * wallet that did not persist would be a lie the user acts on. */
void boot_wallet_show_recovery_phrase_once(const char *phrase);

/* Create the first-run wallet: a fresh recovery phrase, both key trees
 * rooted on the seed it derives, the standard keypool minted from that
 * seed, one durable flush, and then — only once the seed is provably on
 * disk — the words printed once for the user to write down.
 *
 * `plaintext_optin` is true when the operator opted into an unencrypted
 * at-rest wallet; it is recorded as a boot event exactly as before.
 *
 * Returns false when the keypool could not be made durable, having already
 * printed the FATAL diagnostic and emitted the boot event; the caller must
 * refuse to continue, because the alternative is a node running on keys
 * that exist only in RAM. Never shows a phrase on that path. */
bool boot_wallet_create_new(struct wallet *w, struct wallet_sqlite *ws,
                            struct node_db *ndb, bool plaintext_optin);

/* On an EXISTING wallet, re-point the transparent HD chain at the seed
 * just loaded from disk — but only when that seed provably grew the keys
 * already in the keystore (wallet_hd_adopt_seed makes that call by
 * derivation). A wallet created before recovery phrases is left exactly as
 * it was found: legacy random key generation, unchanged derivation. */
void boot_wallet_adopt_seed_if_it_governs(struct wallet *w);

#endif /* ZCL_CONFIG_BOOT_WALLET_PHRASE_H */
