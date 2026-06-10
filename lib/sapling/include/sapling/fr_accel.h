/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Hardware-accelerated field arithmetic dispatch.
 * Compile with -march=native to enable BMI2+ADX and AVX-512 IFMA paths. */

#ifndef ZCL_SAPLING_FR_ACCEL_H
#define ZCL_SAPLING_FR_ACCEL_H

#include <stdint.h>
#include "sapling/fr.h"

/* BMI2+ADX accelerated Montgomery multiply (single) */
#if defined(__BMI2__) && defined(__ADX__)
void fr_mont_mul_asm(uint64_t r[4], const uint64_t a[4], const uint64_t b[4]);
void fp_mont_mul_asm(uint64_t r[6], const uint64_t a[6], const uint64_t b[6]);
#endif

/* AVX-512 IFMA batch multiply (8 independent Fr multiplies) */
#ifdef __AVX512IFMA__
void fr_mul_batch8(struct fr r[8], const struct fr a[8], const struct fr b[8]);
#endif

/* Parallel FFT (pthread-based). Returns false on non-pow-2 n. */
bool fr_fft_parallel(struct fr *coeffs, size_t n, bool inverse, int num_threads);

/* Forward declarations for point types */
struct g1_point;
struct g2_point;

#endif
