/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "services/utxo_import_pipeline.h"

#include "coins/compressor.h"
#include "core/serialize.h"
#include "models/database.h"
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int utxo_import_num_decoders(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 6)
        return 4;
    if (n > 34)
        return UTXO_IMPORT_NUM_DECODERS_MAX;
    return (int)(n - 2);
}

struct zcl_result utxo_import_writer_bind_checked(sqlite3_stmt *stmt,
                                                  const char *label,
                                                  int rc,
                                                  const struct node_db *ndb,
                                                  int row_no)
{
    if (!stmt) {
        LOG_WARN("sync", "UTXO import writer: stmt is NULL for %s",
                 label ? label : "(unknown)");
        return ZCL_ERR(-1, "import writer bind: stmt is NULL label=%s",
                       label ? label : "");
    }
    if (rc != SQLITE_OK) {
        const char *err = (ndb && ndb->db) ? sqlite3_errmsg(ndb->db)
                                          : "db unavailable";
        LOG_WARN("sync",
                 "UTXO import writer: %s failed at row %d (rc=%d): %s",
                 label ? label : "(unknown)", row_no, rc, err);
        return ZCL_ERR(-2, "import writer bind: %s failed row=%d rc=%d: %s",
                       label ? label : "", row_no, rc, err);
    }
    return ZCL_OK;
}

struct zcl_result utxo_import_writer_step_checked(sqlite3_stmt *stmt,
                                                  const struct node_db *ndb,
                                                  int row_no)
{
    if (!stmt) {
        LOG_WARN("sync", "UTXO import writer: stmt is NULL at row %d",
                 row_no);
        return ZCL_ERR(-3, "import writer step: stmt is NULL row=%d",
                       row_no);
    }
    int step_rc = AR_STEP_ROW_READONLY(stmt);
    if (step_rc != SQLITE_DONE) {
        const char *err = (ndb && ndb->db) ? sqlite3_errmsg(ndb->db)
                                          : "db unavailable";
        LOG_WARN("sync",
                 "UTXO import writer: sqlite3_step failed at row %d "
                 "(rc=%d): %s", row_no, step_rc, err);
        return ZCL_ERR(-4, "import writer step failed row=%d rc=%d: %s",
                       row_no, step_rc, err);
    }
    return ZCL_OK;
}

int utxo_import_decode_entry(const struct utxo_import_raw_entry *raw,
                             struct utxo_import_row *out,
                             int max_rows)
{
    if (!raw || !out || max_rows <= 0)
        return 0;

    struct byte_stream s;
    stream_init_from_data(&s, raw->value, raw->value_len);

    uint64_t nVersion = 0;
    if (!stream_read_varint(&s, &nVersion)) {
        stream_free(&s);
        return 0;
    }

    uint64_t nCode = 0;
    if (!stream_read_varint(&s, &nCode)) {
        stream_free(&s);
        return 0;
    }

    bool is_coinbase = (nCode & 1) != 0;
    bool vout0_present = (nCode & 2) != 0;
    bool vout1_present = (nCode & 4) != 0;
    unsigned int nMaskCode = (unsigned int)(nCode / 8) +
        ((vout0_present || vout1_present) ? 0 : 1);

    if (nMaskCode > 10000) {
        stream_free(&s);
        return 0;
    }

    size_t num_avail = 2;
    bool avail[4096];
    memset(avail, 0, sizeof(avail));
    avail[0] = vout0_present;
    avail[1] = vout1_present;

    unsigned int mask_remaining = nMaskCode;
    while (mask_remaining > 0) {
        unsigned char ch = 0;
        if (!stream_read_bytes(&s, &ch, 1))
            break;
        for (unsigned int p = 0; p < 8 && num_avail < 4096; p++)
            avail[num_avail++] = (ch & (1 << p)) != 0;
        if (ch != 0)
            mask_remaining--;
    }

    int nrows = 0;
    for (size_t vi = 0; vi < num_avail && nrows < max_rows; vi++) {
        if (!avail[vi])
            continue;

        uint64_t comp_amount = 0;
        if (!stream_read_varint(&s, &comp_amount))
            break;
        int64_t value = (int64_t)decompress_amount(comp_amount);

        uint64_t nSize = 0;
        if (!stream_read_varint(&s, &nSize))
            break;

        size_t raw_script_len = 0;
        if (nSize == 0 || nSize == 1)
            raw_script_len = 20;
        else if (nSize >= 2 && nSize <= 5)
            raw_script_len = 32;
        else
            raw_script_len = (size_t)(nSize - 6);

        uint8_t raw_script[10240];
        if (raw_script_len > sizeof(raw_script))
            raw_script_len = sizeof(raw_script);
        if (!stream_read_bytes(&s, raw_script, raw_script_len))
            break;

        struct utxo_import_row *r = &out[nrows];
        memcpy(r->txid, raw->txid, 32);
        r->vout = (uint32_t)vi;
        r->value = value;
        r->is_coinbase = is_coinbase;
        r->height = 0;
        r->script_overflow = NULL;
        r->has_address = 0;
        r->script_type = 0;

        if (nSize == 0) {
            r->script_len = 25;
            r->script[0] = 0x76;
            r->script[1] = 0xa9;
            r->script[2] = 0x14;
            memcpy(r->script + 3, raw_script, 20);
            r->script[23] = 0x88;
            r->script[24] = 0xac;
            memcpy(r->address_hash, raw_script, 20);
            r->has_address = 1;
            r->script_type = 1;
        } else if (nSize == 1) {
            r->script_len = 23;
            r->script[0] = 0xa9;
            r->script[1] = 0x14;
            memcpy(r->script + 2, raw_script, 20);
            r->script[22] = 0x87;
            memcpy(r->address_hash, raw_script, 20);
            r->has_address = 1;
            r->script_type = 2;
        } else if (nSize >= 2 && nSize <= 5) {
            uint8_t prefix = (nSize == 2 || nSize == 4) ? 0x02 : 0x03;
            r->script_len = 35;
            r->script[0] = 0x21;
            r->script[1] = prefix;
            memcpy(r->script + 2, raw_script, 32);
            r->script[34] = 0xac;
        } else {
            uint16_t slen = (uint16_t)raw_script_len;
            r->script_len = slen;
            if (slen <= sizeof(r->script)) {
                memcpy(r->script, raw_script, slen);
            } else {
                r->script_overflow = zcl_malloc(slen, "script overflow");
                if (r->script_overflow) {
                    memcpy(r->script_overflow, raw_script, slen);
                } else {
                    r->script_len = (uint16_t)sizeof(r->script);
                    memcpy(r->script, raw_script, sizeof(r->script));
                }
            }
            const uint8_t *sc = r->script_overflow ? r->script_overflow
                                                   : r->script;
            if (slen == 25 && sc[0] == 0x76 && sc[1] == 0xa9 &&
                sc[2] == 0x14 && sc[23] == 0x88 && sc[24] == 0xac) {
                memcpy(r->address_hash, sc + 3, 20);
                r->has_address = 1;
                r->script_type = 1;
            } else if (slen == 23 && sc[0] == 0xa9 && sc[1] == 0x14 &&
                       sc[22] == 0x87) {
                memcpy(r->address_hash, sc + 2, 20);
                r->has_address = 1;
                r->script_type = 2;
            } else if (slen > 0 && sc[0] == 0x6a) {
                r->script_type = 3;
            }
        }
        nrows++;
    }

    uint64_t height = 0;
    if (stream_read_varint(&s, &height) && height <= 10000000) {
        for (int i = 0; i < nrows; i++)
            out[i].height = (int32_t)height;
    }

    stream_free(&s);
    return nrows;
}
