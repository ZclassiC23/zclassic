/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: explained full-text queries over the rebuildable code index. */

#include "codeindex_priv.h"

#include "util/log_macros.h"

#include <sqlite3.h>

#include <string.h>

int ci_store_search_text(struct ci_store *s, const char *q,
                         struct ci_search_hit *out, int cap)
{
    if (!s || !q || !q[0] || !out || cap <= 0)
        LOG_ERR("codeindex", "bad arg to search_text");
    ci_store_lock(s);
    sqlite3_stmt *stmt = NULL;
    static const char sql[] =
        "SELECT " CI_SYM_COLS ","
        " instr(lower(name),lower(?1))>0 AS in_name,"
        " instr(lower(signature),lower(?1))>0 AS in_signature,"
        " (instr(lower(def_path),lower(?1))>0 OR"
        "  instr(lower(decl_path),lower(?1))>0) AS in_path,"
        " instr(lower(doc),lower(?1))>0 AS in_doc,"
        " CASE WHEN lower(name)=lower(?1) THEN 0"
        "      WHEN instr(lower(name),lower(?1))=1 THEN 1"
        "      WHEN instr(lower(name),lower(?1))>0 THEN 2"
        "      WHEN instr(lower(signature),lower(?1))>0 THEN 3"
        "      WHEN instr(lower(def_path),lower(?1))>0 OR"
        "           instr(lower(decl_path),lower(?1))>0 THEN 4"
        "      ELSE 5 END AS rank"
        " FROM symbols"
        " WHERE instr(lower(name),lower(?1))>0"
        "    OR instr(lower(signature),lower(?1))>0"
        "    OR instr(lower(def_path),lower(?1))>0"
        "    OR instr(lower(decl_path),lower(?1))>0"
        "    OR instr(lower(doc),lower(?1))>0"
        " ORDER BY rank ASC,name ASC,(def_path='') ASC,def_path ASC,def_line ASC";
    sqlite3 *db = ci_store_db(s);
    if (!db || sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        ci_store_unlock(s);
        LOG_ERR("codeindex", "prepare search_text");
    }
    sqlite3_bind_text(stmt, 1, q, -1, SQLITE_TRANSIENT);
    int n = 0;
    int rc = SQLITE_DONE;
    while (n < cap && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {  // raw-sql-ok:codeindex-derived
        struct ci_search_hit hit;
        memset(&hit, 0, sizeof(hit));
        if (!ci_store_fill_symbol(stmt, &hit.symbol))
            continue;
        if (sqlite3_column_int(stmt, 12))
            hit.match_mask |= CI_SEARCH_MATCH_NAME;
        if (sqlite3_column_int(stmt, 13))
            hit.match_mask |= CI_SEARCH_MATCH_SIGNATURE;
        if (sqlite3_column_int(stmt, 14))
            hit.match_mask |= CI_SEARCH_MATCH_PATH;
        if (sqlite3_column_int(stmt, 15))
            hit.match_mask |= CI_SEARCH_MATCH_DOC;
        hit.score = 1000 - sqlite3_column_int(stmt, 16) * 100;
        out[n++] = hit;
    }
    bool ok = rc == SQLITE_ROW || rc == SQLITE_DONE;
    sqlite3_finalize(stmt);
    ci_store_unlock(s);
    if (!ok) LOG_ERR("codeindex", "step search_text");
    return n;
}
