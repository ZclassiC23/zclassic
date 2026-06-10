/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ChaCha20-Poly1305 AEAD (RFC 7539) — pure C23 implementation.
 * Replaces libsodium crypto_aead_chacha20poly1305_ietf_encrypt/decrypt. */

#ifndef ZCL_CRYPTO_CHACHA20POLY1305_H
#define ZCL_CRYPTO_CHACHA20POLY1305_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define CHACHA20_KEY_SIZE 32
#define CHACHA20_NONCE_SIZE 12
#define POLY1305_TAG_SIZE 16

void chacha20_block(const uint8_t key[32], uint32_t counter,
                     const uint8_t nonce[12], uint8_t out[64]);

void chacha20_encrypt(const uint8_t key[32], uint32_t counter,
                       const uint8_t nonce[12],
                       const uint8_t *plaintext, size_t len,
                       uint8_t *ciphertext);

void poly1305_mac(const uint8_t *message, size_t len,
                   const uint8_t key[32], uint8_t tag[16]);

bool chacha20poly1305_encrypt(const uint8_t *plaintext, size_t plen,
                                const uint8_t *aad, size_t aad_len,
                                const uint8_t nonce[12],
                                const uint8_t key[32],
                                uint8_t *ciphertext);

bool chacha20poly1305_decrypt(const uint8_t *ciphertext, size_t clen,
                                const uint8_t *aad, size_t aad_len,
                                const uint8_t nonce[12],
                                const uint8_t key[32],
                                uint8_t *plaintext);

#endif
