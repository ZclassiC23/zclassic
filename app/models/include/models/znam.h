/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_DB_MODEL_ZNAM_H
#define ZCL_DB_MODEL_ZNAM_H

#include "models/database.h"
#include "models/activerecord.h"
#include "znam/znam.h"
#include <stdbool.h>

/* ActiveRecord models for the ZCL Names (ZNAM) registry.
 *
 * Three tables, three record types:
 *   znam_names         → struct znam_entry
 *   znam_text_records  → struct znam_text_record
 *   znam_addr_records  → struct znam_addr_record
 *
 * The validators enforce the format constraints from the on-chain
 * ZNAM protocol (lokad ID "ZNAM"). Names that fail validation should
 * never have been accepted from OP_RETURN parsing in the first place;
 * the validator here is the last line of defense against corrupted
 * blocks reaching at-rest storage. */

struct znam_entry {
    char name[ZNAM_NAME_MAX + 1];
    char owner_address[64];
    uint8_t target_type;
    char target_value[ZNAM_VALUE_MAX + 1];
    uint8_t reg_txid[32];
    int32_t reg_height;
    uint8_t last_update_txid[32];
};

/* Text record (ENS TextResolver equivalent) */
struct znam_text_record {
    char name[ZNAM_NAME_MAX + 1];
    char key[ZNAM_TEXT_KEY_MAX + 1];
    char value[ZNAM_TEXT_VAL_MAX + 1];
};

/* Additional address record (ENS AddrResolver equivalent) */
struct znam_addr_record {
    char name[ZNAM_NAME_MAX + 1];
    uint8_t coin_type;    /* ZNAM_TYPE_BTC, etc. */
    char address[ZNAM_VALUE_MAX + 1];
};

struct ar_callbacks *db_znam_entry_callbacks(void);
struct ar_callbacks *db_znam_text_callbacks(void);
struct ar_callbacks *db_znam_addr_callbacks(void);

bool db_znam_entry_validate(const struct znam_entry *entry,
                            struct ar_errors *errors);
bool db_znam_text_validate(const struct znam_text_record *rec,
                           struct ar_errors *errors);
bool db_znam_addr_validate(const struct znam_addr_record *rec,
                           struct ar_errors *errors);

bool db_znam_save(struct node_db *ndb, const struct znam_entry *entry);
bool db_znam_find(struct node_db *ndb, const char *name,
                  struct znam_entry *out);
int db_znam_list(struct node_db *ndb, struct znam_entry *out, size_t max);
int db_znam_list_by_owner(struct node_db *ndb, const char *owner,
                          struct znam_entry *out, size_t max);

/* Text records */
bool db_znam_text_save(struct node_db *ndb, const char *name,
                       const char *key, const char *value);
bool db_znam_text_get(struct node_db *ndb, const char *name,
                      const char *key, char *value_out, size_t max);
int db_znam_text_list(struct node_db *ndb, const char *name,
                      struct znam_text_record *out, size_t max);

/* Multi-coin address records */
bool db_znam_addr_save(struct node_db *ndb, const char *name,
                       uint8_t coin_type, const char *address);
bool db_znam_addr_get(struct node_db *ndb, const char *name,
                      uint8_t coin_type, char *addr_out, size_t max);
int db_znam_addr_list(struct node_db *ndb, const char *name,
                      struct znam_addr_record *out, size_t max);

#endif
