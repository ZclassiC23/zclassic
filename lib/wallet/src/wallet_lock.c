/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Wallet lock/unlock — implementation.  See wallet_lock.h for the model. */

#include "wallet/wallet_lock.h"
#include "wallet/wallet.h"
#include "wallet/wallet_sqlite.h"
#include "wallet/keystore.h"

#include "support/cleanse.h"
#include "util/log_macros.h"
#include "json/json.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Bound the cached passphrase so it lives in a fixed, cleansable buffer
 * (no heap copy of the secret to chase). 512 bytes is far past any real
 * passphrase and still fits the WKS PBKDF2 input. */
#define WLK_MAX_PASS 512

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

/* All fields guarded by g_mu. */
static char  g_runtime_pass[WLK_MAX_PASS + 1];
static bool  g_have_runtime_pass;   /* an unlock cached a passphrase */
static bool  g_force_locked;        /* an explicit lock — wins over env */
static bool  g_encrypted_at_rest;   /* the wallet uses WKS1 at-rest wrapping */

/* Resolve the effective passphrase under g_mu already held. Returns a
 * pointer into g_runtime_pass, the env value, or NULL. */
static const char *effective_pass_locked(void)
{
    if (g_force_locked)
        return NULL;
    if (g_have_runtime_pass)
        return g_runtime_pass;
    const char *env = getenv("ZCL_WALLET_PASSPHRASE");
    return (env && *env) ? env : NULL;
}

const char *wallet_lock_effective_passphrase(void)
{
    pthread_mutex_lock(&g_mu);
    const char *p = effective_pass_locked();
    pthread_mutex_unlock(&g_mu);
    return p;
}

void wallet_lock_note_encrypted_at_rest(void)
{
    pthread_mutex_lock(&g_mu);
    g_encrypted_at_rest = true;
    pthread_mutex_unlock(&g_mu);
}

bool wallet_lock_encrypted_at_rest(void)
{
    pthread_mutex_lock(&g_mu);
    bool v = g_encrypted_at_rest;
    pthread_mutex_unlock(&g_mu);
    return v;
}

bool wallet_lock_is_unlocked(void)
{
    pthread_mutex_lock(&g_mu);
    /* A plaintext wallet has nothing to unlock — always "unlocked". An
     * encrypted wallet is unlocked only while an effective passphrase is
     * available. */
    bool unlocked = !g_encrypted_at_rest || (effective_pass_locked() != NULL);
    pthread_mutex_unlock(&g_mu);
    return unlocked;
}

struct zcl_result wallet_lock_spend_guard(void)
{
    if (wallet_lock_is_unlocked())
        return ZCL_OK;
    return ZCL_ERR(WLK_LOCKED,
        "wallet is locked — private keys are encrypted at rest and no "
        "passphrase is loaded; unlock before spending");
}

struct zcl_result wallet_lock_unlock(struct wallet *w, struct wallet_sqlite *ws,
                                     const char *passphrase)
{
    if (!passphrase)
        return ZCL_ERR(WLK_NULL_ARG, "unlock: passphrase is NULL");
    size_t plen = strlen(passphrase);
    if (plen == 0)
        return ZCL_ERR(WLK_EMPTY_PASS, "unlock: passphrase is empty");
    if (plen > WLK_MAX_PASS)
        return ZCL_ERR(WLK_PASS_TOO_LONG,
                       "unlock: passphrase exceeds %d bytes", WLK_MAX_PASS);

    /* Snapshot prior state so a wrong passphrase leaves NO trace. */
    pthread_mutex_lock(&g_mu);
    bool  prev_have = g_have_runtime_pass;
    bool  prev_force = g_force_locked;
    char  prev_pass[WLK_MAX_PASS + 1];
    memcpy(prev_pass, g_runtime_pass, sizeof(prev_pass));

    memcpy(g_runtime_pass, passphrase, plen);
    g_runtime_pass[plen] = '\0';
    g_have_runtime_pass = true;
    g_force_locked = false;
    pthread_mutex_unlock(&g_mu);

    /* Register-only unlock (no keystore wired / unit test): accept. */
    if (!w || !ws) {
        memory_cleanse(prev_pass, sizeof(prev_pass));
        return ZCL_OK;
    }

    /* Reload transparent + Sapling keys from disk under the new passphrase.
     * read_keys_r decrypts each WKS1 blob via wallet_lock_effective_passphrase
     * (now the just-cached value); a wrong passphrase drops every encrypted
     * row and loads zero keys. */
    struct wallet_sqlite_health before = wallet_sqlite_get_health(ws, 0);
    int rows = before.row_count;

    keystore_wipe_private_keys(&w->keystore);
    struct zcl_result rr = wallet_sqlite_read_keys_r(ws, w);
    (void)wallet_sqlite_read_sapling_keys(ws, w);

    int loaded = (int)w->keystore.num_keys;

    /* Wrong passphrase: rows exist on disk but none decrypted. Roll the
     * whole subsystem back to its pre-unlock state and scrub. */
    if (!rr.ok || (rows > 0 && loaded == 0)) {
        keystore_wipe_private_keys(&w->keystore);
        pthread_mutex_lock(&g_mu);
        memory_cleanse(g_runtime_pass, sizeof(g_runtime_pass));
        memcpy(g_runtime_pass, prev_pass, sizeof(prev_pass));
        g_have_runtime_pass = prev_have;
        g_force_locked = prev_force;
        pthread_mutex_unlock(&g_mu);
        memory_cleanse(prev_pass, sizeof(prev_pass));
        return ZCL_ERR(WLK_WRONG_PASS,
            "unlock: passphrase did not decrypt any of %d on-disk key row(s)",
            rows);
    }

    memory_cleanse(prev_pass, sizeof(prev_pass));
    return ZCL_OK;
}

void wallet_lock_lock(struct wallet *w)
{
    pthread_mutex_lock(&g_mu);
    memory_cleanse(g_runtime_pass, sizeof(g_runtime_pass));
    g_have_runtime_pass = false;
    g_force_locked = true;
    pthread_mutex_unlock(&g_mu);

    /* Make decrypted spend keys non-resident so even a code path that
     * bypasses the spend guard cannot sign. */
    if (w)
        keystore_wipe_private_keys(&w->keystore);
}

void wallet_lock_status_json(struct json_value *out)
{
    if (!out) return;
    pthread_mutex_lock(&g_mu);
    bool encrypted = g_encrypted_at_rest;
    bool unlocked = !g_encrypted_at_rest || (effective_pass_locked() != NULL);
    const char *source;
    if (!encrypted)                 source = "plaintext";
    else if (g_force_locked)        source = "locked";
    else if (g_have_runtime_pass)   source = "runtime";
    else if (effective_pass_locked()) source = "env";
    else                            source = "locked";
    pthread_mutex_unlock(&g_mu);

    json_push_kv_bool(out, "encrypted_at_rest", encrypted);
    json_push_kv_bool(out, "unlocked", unlocked);
    json_push_kv_bool(out, "locked", !unlocked);
    json_push_kv_str(out, "source", source);
}

void wallet_lock_reset_for_test(void)
{
    pthread_mutex_lock(&g_mu);
    memory_cleanse(g_runtime_pass, sizeof(g_runtime_pass));
    g_have_runtime_pass = false;
    g_force_locked = false;
    g_encrypted_at_rest = false;
    pthread_mutex_unlock(&g_mu);
}
