/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "platform/time_compat.h"
#include "util/log_macros.h"
#include "models/onion_announcement.h"
#include "models/model_text.h"
#include "storage/small_projections.h"
#include <string.h>
#include <time.h>

DEFINE_MODEL_CALLBACKS(onion_announcement)

static bool onion_announcement_before_save(void *record, void *ctx)
{
    struct db_onion_announcement *ann = record;

    (void)ctx;
    if (!ann)
        return false;
    model_trim_ascii(ann->onion_address);
    model_trim_ascii(ann->script_hex);
    model_ascii_downcase(ann->onion_address);
    model_ascii_downcase(ann->script_hex);
    return true;
}

/* Projection emit runs as a registered after_save callback (the
 * wallet_key pattern) so it fires through the AR lifecycle on a successful
 * save instead of as inline post-save code. Best-effort: a failed emit is
 * logged but does not fail the save. */
static void onion_announcement_after_save(void *record, void *ctx)
{
    (void)ctx;
    const struct db_onion_announcement *a = record;
    if (a->announced_at < 0 || a->announced_at > UINT32_MAX ||
        !onion_ann_projection_emit(a->onion_address,
                                   (uint32_t)a->announced_at,
                                   a->script_hex)) {
        LOG_WARN("model", "onion announcement projection emit failed for save");
    }
}

/* Registers onion_announcement's before_save + after_save hooks once on the
 * callback singleton (db_onion_announcement_callbacks returns a static
 * struct). */
static struct ar_callbacks *onion_announcement_callbacks_ready(void)
{
    struct ar_callbacks *cbs = db_onion_announcement_callbacks();
    static bool hooks_done = false;
    if (!hooks_done) {
        ar_register_before_save(cbs, onion_announcement_before_save);
        ar_register_after_save(cbs, onion_announcement_after_save);
        hooks_done = true;
    }
    return cbs;
}

bool db_onion_announcement_validate(const struct db_onion_announcement *a,
                                    struct ar_errors *errors)
{
    size_t alen;

    ar_errors_clear(errors);
    validates_string_present(errors, a->onion_address, "onion_address");
    validates_non_negative(errors, a, announced_at);
    validates_custom(errors,
        strlen(a->onion_address) <= ONION_ADDRESS_MAX,
        "onion_address", "exceeds max length 127");
    validates_custom(errors,
        strlen(a->script_hex) <= ONION_SCRIPT_HEX_MAX,
        "script_hex", "exceeds max length 511");
    validates_custom(errors,
        model_string_is_printable(a->onion_address),
        "onion_address", "contains non-printable characters");
    validates_custom(errors,
        model_string_is_printable(a->script_hex) || a->script_hex[0] == '\0',
        "script_hex", "contains non-printable characters");
    alen = strlen(a->onion_address);
    validates_custom(errors,
        alen >= 7 &&
        strcmp(a->onion_address + alen - 6, ".onion") == 0,
        "onion_address", "must end with .onion");
    return !ar_errors_any(errors);
}

bool db_onion_announcement_save(struct node_db *ndb,
                                const struct db_onion_announcement *a)
{
    sqlite3_stmt *s = NULL;
    struct ar_callbacks *cbs;

    if (!ndb || !ndb->open || !a)
        return false;
    if (a->announced_at == 0)
        ((struct db_onion_announcement *)a)->announced_at = (int64_t)platform_time_wall_time_t();

    cbs = onion_announcement_callbacks_ready();
    if (!ar_run_before_save(cbs, (void *)a)) {
        fprintf(stderr, "onion_announcement save vetoed by before_save\n");
        return false;
    }
    AR_VALIDATE_RECORD(cbs, "onion_announcement", a,
                       db_onion_announcement_validate);

    if (sqlite3_prepare_v2(ndb->db,
            "INSERT OR REPLACE INTO onion_announcements "
            "(onion_address,announced_at,script_hex) VALUES (?,?,?)",
            -1, &s, NULL) != SQLITE_OK || !s)
        return false;

    AR_BIND_TEXT(s, 1, a->onion_address);
    AR_BIND_INT(s, 2, a->announced_at);
    AR_BIND_TEXT(s, 3, a->script_hex);
    if (!AR_STEP_DONE(s)) {
        fprintf(stderr, "onion_announcement save failed: %s\n",
                sqlite3_errmsg(ndb->db));
        AR_FINALIZE(s);
        return false;
    }
    AR_FINALIZE(s);
    ar_run_after_save(cbs, (void *)a);
    return true;
}

bool db_onion_announcement_exists(struct node_db *ndb,
                                  const char *onion_address)
{
    sqlite3_stmt *s = NULL;
    bool exists = false;

    if (!ndb || !ndb->open || !onion_address)
        return false;

    if (sqlite3_prepare_v2(ndb->db,
            "SELECT 1 FROM onion_announcements WHERE onion_address=?",
            -1, &s, NULL) != SQLITE_OK || !s)
        return false;

    AR_BIND_TEXT(s, 1, onion_address);
    exists = AR_STEP_ROW(s);
    AR_FINALIZE(s);
    return exists;
}

int db_onion_announcement_recent(struct node_db *ndb,
                                 struct db_onion_announcement *out,
                                 size_t max)
{
    sqlite3_stmt *s = NULL;
    int count = 0;

    if (!ndb || !ndb->open || !out || max == 0)
        return 0;
    AR_PREPARE_RET(ndb, s,
            "SELECT onion_address,announced_at,script_hex "
            "FROM onion_announcements "
            "ORDER BY announced_at DESC, onion_address ASC LIMIT ?",
            0);

    AR_BIND_INT(s, 1, (int)max);
    AR_LIST_ROWS(s, out, max,
        AR_READ_STR(s, 0, out[count].onion_address,
                    sizeof(out[count].onion_address));
        out[count].announced_at = AR_COL_INT(s, 1);
        AR_READ_STR(s, 2, out[count].script_hex, sizeof(out[count].script_hex)));
    AR_FINALIZE(s);
    return count;
}
