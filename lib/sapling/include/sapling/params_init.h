/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Load Zcash zkSNARK verification keys from params files. */

#ifndef ZCL_SAPLING_PARAMS_INIT_H
#define ZCL_SAPLING_PARAMS_INIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Load all Sapling and Sprout Groth16 verification keys.
 * Reads sapling-spend.params, sapling-output.params, and sprout-groth16.params
 * from the given directory path. Sets global VKs for verification.
 * Returns false if any file cannot be read or parsed. */
bool sapling_init_params(const char *params_dir);

/* True only after all verification keys have loaded and been installed.
 * Safe to read from reducer/validation threads while the boot loader thread is
 * still parsing the large params files. */
bool sapling_params_loaded(void);

/* Get raw proving key data for Sapling output/spend proofs.
 * Returns pointer to mmap'd file data. NULL if not loaded. */
const uint8_t *sapling_get_output_pk(size_t *len);
const uint8_t *sapling_get_spend_pk(size_t *len);

/* Free all loaded verification keys. */
void sapling_free_params(void);

#endif
