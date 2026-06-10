/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Private helpers shared by utxo_apply delta/unwind translation units. */

#ifndef ZCL_JOBS_UTXO_APPLY_DELTA_INTERNAL_H
#define ZCL_JOBS_UTXO_APPLY_DELTA_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

struct sqlite3;

bool utxo_apply_emit_inverse_delta(struct sqlite3 *db, int height);
bool utxo_apply_delete_rows_above(struct sqlite3 *db, int first_h,
                                  int last_h);
bool utxo_apply_unwind_write_cursor(struct sqlite3 *db, uint64_t value);

#endif /* ZCL_JOBS_UTXO_APPLY_DELTA_INTERNAL_H */
