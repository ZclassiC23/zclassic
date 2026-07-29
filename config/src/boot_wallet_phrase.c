/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot_wallet_phrase — implementation. See config/boot_wallet_phrase.h. */

#include "config/boot_wallet_phrase.h"

#include "event/event.h"
#include "models/database.h"
#include "support/cleanse.h"
#include "util/log_macros.h"
#include "wallet/mnemonic.h"
#include "wallet/wallet.h"
#include "wallet/wallet_sqlite.h"

#include <stdio.h>
#include <string.h>

bool boot_wallet_mint_recovery_phrase(struct wallet *w, char *phrase_out,
                                      size_t cap)
{
    GUARD_NOT_NULL(w, "wallet_phrase", "wallet");
    GUARD_NOT_NULL(phrase_out, "wallet_phrase", "phrase_out");
    if (cap < BOOT_WALLET_PHRASE_CAP)
        LOG_FAIL("wallet_phrase", "phrase buffer too small: %zu < %d",
                 cap, (int)BOOT_WALLET_PHRASE_CAP);
    phrase_out[0] = '\0';

    if (!mnemonic_generate(MNEMONIC_12_WORDS, phrase_out, cap))
        LOG_FAIL("wallet_phrase", "could not draw a 12-word recovery phrase");

    if (!wallet_init_from_recovery_phrase(w, phrase_out)) {
        memory_cleanse(phrase_out, cap);
        phrase_out[0] = '\0';
        LOG_FAIL("wallet_phrase",
                 "could not root the wallet on the recovery phrase");
    }
    return true;
}

void boot_wallet_show_recovery_phrase_once(const char *phrase)
{
    if (!phrase || !phrase[0])
        return;

    /* stdout, never the log. node.log is copied, shipped and read by other
     * people; these words are the wallet. */
    printf("\n");
    printf("================ WRITE THESE 12 WORDS DOWN ================\n");
    printf("\n");
    printf("  %s\n", phrase);
    printf("\n");
    printf("These twelve words ARE your wallet. Anyone who has them can\n");
    printf("spend your money. Anyone who loses them, and the disk, loses\n");
    printf("the money for good.\n");
    printf("\n");
    printf("Write them on paper, in this order, and keep the paper.\n");
    printf("\n");
    printf("THIS IS THE ONLY TIME THEY WILL EVER BE SHOWN. There is no\n");
    printf("command that prints them again, and that is on purpose: the\n");
    printf("node stores only the key material the words derive, and the\n");
    printf("words cannot be worked back out of it. A command that could\n");
    printf("reprint them would turn one read of the wallet database into\n");
    printf("the loss of every coin in it.\n");
    printf("\n");
    printf("To get this wallet back on a new machine:\n");
    printf("  zclassic23 core wallet recovery restore \\\n");
    printf("      --input='{\"phrase\":\"<your 12 words>\",\"confirm\":true}'\n");
    printf("==========================================================\n");
    printf("\n");
    fflush(stdout);
}

bool boot_wallet_create_new(struct wallet *w, struct wallet_sqlite *ws,
                            struct node_db *ndb, bool plaintext_optin)
{
    GUARD_NOT_NULL(w, "wallet_phrase", "wallet");
    if (plaintext_optin)
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                    "wallet_plaintext_created_optin");

    /* Born from twelve words: draw the phrase and root BOTH key trees on
     * the seed it derives BEFORE any key is minted, so every key in the
     * keypool below is recoverable from the words alone. */
    char phrase[BOOT_WALLET_PHRASE_CAP];
    bool phrase_ok = boot_wallet_mint_recovery_phrase(w, phrase,
                                                      sizeof(phrase));
    if (!phrase_ok)
        fprintf(stderr, "WARNING: could not create a recovery phrase for "
            "this wallet; its keys are random and can only be backed up as "
            "a file (core wallet backup now).\n");

    wallet_top_up_key_pool(w, DEFAULT_KEYPOOL_SIZE);
    int64_t pool_generation = wallet_key_pool_generation_ceiling(w);

    if (ws && ws->open) {
        struct zcl_result r = wallet_sqlite_flush_r(ws, w);
        if (!r.ok) {
            fprintf(stderr,
                "\nFATAL: initial keypool flush failed.\n"
                "       code=%d message=%s\n"
                "       source=%s:%d\n"
                "       REFUSING to proceed — fresh keys would be RAM-only.\n\n",
                r.code, r.message, r.source_file ? r.source_file : "?",
                r.source_line);
            event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                        "wallet_keypool_flush_failed code=%d", r.code);
            memory_cleanse(phrase, sizeof(phrase));
            return false;
        }
    } else {
        /* RAM-only wallet: nothing was persisted, so the phrase would name
         * a wallet that will not be here next boot. Never show words the
         * user would write down for nothing. */
        phrase_ok = false;
    }
    wallet_key_pool_mark_persisted_through(w, pool_generation);
    if (ndb && ndb->open)
        node_db_wal_checkpoint(ndb);

    /* The seed is on disk now — the words are true. Show them once. */
    if (phrase_ok)
        boot_wallet_show_recovery_phrase_once(phrase);
    memory_cleanse(phrase, sizeof(phrase));
    printf("New wallet created.\n");
    return true;
}

void boot_wallet_adopt_seed_if_it_governs(struct wallet *w)
{
    if (w && w->sapling_keys.has_seed)
        (void)wallet_hd_adopt_seed(w, w->sapling_keys.seed);
}
