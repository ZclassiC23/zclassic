/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Pure standard-script-type detection. Replays from script bytes alone.
 * No clock, RNG, allocation, or I/O. Mirrors zclassicd
 * src/script/standard.cpp::Solver + ExtractDestination + GetScriptID. */

#include "domain/consensus/script_standard.h"

#include "core/hash.h"
#include "keys/pubkey.h"
#include "script/script.h"
#include "script/standard.h"      /* for struct tx_destination layout */

#include <string.h>

const char *domain_consensus_script_txn_output_type_name(
        enum domain_script_txnouttype t)
{
    switch (t) {
    case DOMAIN_SCRIPT_TX_NONSTANDARD: return "nonstandard";
    case DOMAIN_SCRIPT_TX_PUBKEY:      return "pubkey";
    case DOMAIN_SCRIPT_TX_PUBKEYHASH:  return "pubkeyhash";
    case DOMAIN_SCRIPT_TX_SCRIPTHASH:  return "scripthash";
    case DOMAIN_SCRIPT_TX_MULTISIG:    return "multisig";
    case DOMAIN_SCRIPT_TX_NULL_DATA:   return "nulldata";
    }
    return NULL;
}

struct zcl_result domain_consensus_script_id_from_script(
        const struct script *s,
        unsigned char out_hash[20])
{
    if (!s)
        return ZCL_ERR(DOMAIN_CONSENSUS_SCRIPT_STANDARD_ERR_NULL_SCRIPT,
                       "script_id_from_script: null script");
    if (!out_hash)
        return ZCL_ERR(DOMAIN_CONSENSUS_SCRIPT_STANDARD_ERR_NULL_OUT,
                       "script_id_from_script: null out_hash");
    hash160(s->data, s->size, out_hash);
    return ZCL_OK;
}

struct zcl_result domain_consensus_script_solver(
        const struct script *s,
        unsigned char solutions[][65],
        size_t solution_sizes[],
        size_t solutions_cap,
        enum domain_script_txnouttype *type_out,
        size_t *num_solutions,
        bool *matched)
{
    if (!s)
        return ZCL_ERR(DOMAIN_CONSENSUS_SCRIPT_STANDARD_ERR_NULL_SCRIPT,
                       "script_solver: null script");
    if (!type_out || !num_solutions || !matched)
        return ZCL_ERR(DOMAIN_CONSENSUS_SCRIPT_STANDARD_ERR_NULL_OUT,
                       "script_solver: null out pointer");
    if (!solutions || !solution_sizes || solutions_cap == 0)
        return ZCL_ERR(DOMAIN_CONSENSUS_SCRIPT_STANDARD_ERR_NULL_SOLUTIONS,
                       "script_solver: null solutions buffer");

    *num_solutions = 0;
    *matched       = false;
    *type_out      = DOMAIN_SCRIPT_TX_NONSTANDARD;

    /* P2SH: OP_HASH160 <20> <hash> OP_EQUAL */
    if (s->size == 23 && s->data[0] == OP_HASH160 && s->data[1] == 20 &&
        s->data[22] == OP_EQUAL) {
        if (solutions_cap < 1) return ZCL_OK;  /* graceful: NONSTANDARD */
        *type_out = DOMAIN_SCRIPT_TX_SCRIPTHASH;
        memcpy(solutions[0], s->data + 2, 20);
        solution_sizes[0] = 20;
        *num_solutions = 1;
        *matched = true;
        return ZCL_OK;
    }

    /* OP_RETURN (null data) — match must consume entire script as
     * legal data pushes. */
    if (s->size >= 1 && s->data[0] == OP_RETURN) {
        size_t i = 1;
        bool ok = true;
        while (i < s->size) {
            unsigned char op = s->data[i];
            if (op <= 0x4e) {
                size_t push_len = 0;
                if (op <= 75) {
                    push_len = op;
                    i++;
                } else if (op == OP_PUSHDATA1 && i + 1 < s->size) {
                    push_len = s->data[i + 1];
                    i += 2;
                } else if (op == OP_PUSHDATA2 && i + 2 < s->size) {
                    push_len = s->data[i + 1] |
                               ((size_t)s->data[i + 2] << 8);
                    i += 3;
                } else if (op == OP_PUSHDATA4 && i + 4 < s->size) {
                    push_len = s->data[i + 1] |
                               ((size_t)s->data[i + 2] << 8) |
                               ((size_t)s->data[i + 3] << 16) |
                               ((size_t)s->data[i + 4] << 24);
                    i += 5;
                } else {
                    ok = false;
                    break;
                }
                i += push_len;
            } else {
                ok = false;
                break;
            }
        }
        if (ok) {
            *type_out = DOMAIN_SCRIPT_TX_NULL_DATA;
            *matched = true;
            return ZCL_OK;
        }
        /* fall through to other patterns */
    }

    /* P2PKH: OP_DUP OP_HASH160 <20> <hash> OP_EQUALVERIFY OP_CHECKSIG */
    if (s->size == 25 && s->data[0] == OP_DUP && s->data[1] == OP_HASH160 &&
        s->data[2] == 20 && s->data[23] == OP_EQUALVERIFY &&
        s->data[24] == OP_CHECKSIG) {
        if (solutions_cap < 1) return ZCL_OK;
        *type_out = DOMAIN_SCRIPT_TX_PUBKEYHASH;
        memcpy(solutions[0], s->data + 3, 20);
        solution_sizes[0] = 20;
        *num_solutions = 1;
        *matched = true;
        return ZCL_OK;
    }

    /* P2PK compressed: <33> <compressed pubkey> OP_CHECKSIG */
    if (s->size == 35 && s->data[0] == 33 && s->data[34] == OP_CHECKSIG &&
        (s->data[1] == 0x02 || s->data[1] == 0x03)) {
        if (solutions_cap < 1) return ZCL_OK;
        *type_out = DOMAIN_SCRIPT_TX_PUBKEY;
        memcpy(solutions[0], s->data + 1, 33);
        solution_sizes[0] = 33;
        *num_solutions = 1;
        *matched = true;
        return ZCL_OK;
    }

    /* P2PK uncompressed: <65> <04 pubkey> OP_CHECKSIG */
    if (s->size == 67 && s->data[0] == 65 && s->data[66] == OP_CHECKSIG &&
        s->data[1] == 0x04) {
        if (solutions_cap < 1) return ZCL_OK;
        *type_out = DOMAIN_SCRIPT_TX_PUBKEY;
        memcpy(solutions[0], s->data + 1, 65);
        solution_sizes[0] = 65;
        *num_solutions = 1;
        *matched = true;
        return ZCL_OK;
    }

    /* Multisig: OP_m <pubkey>{n} OP_n OP_CHECKMULTISIG */
    if (s->size >= 4 && s->data[s->size - 1] == OP_CHECKMULTISIG) {
        unsigned char last_op = s->data[s->size - 2];
        if (last_op >= OP_1 && last_op <= OP_16) {
            int n = last_op - (OP_1 - 1);
            unsigned char first_op = s->data[0];
            if (first_op >= OP_1 && first_op <= OP_16) {
                int m = first_op - (OP_1 - 1);
                if (m >= 1 && n >= 1 && m <= n) {
                    /* Need n + 2 rows (M-byte, n keys, N-byte).
                     * If the caller's buffer is too small, gracefully
                     * fall through to NONSTANDARD — never overrun. */
                    if ((size_t)(n + 2) > solutions_cap) {
                        *num_solutions = 0;
                        goto not_multisig;
                    }
                    size_t pos = 1;
                    int key_count = 0;
                    solutions[0][0] = (unsigned char)m;
                    solution_sizes[0] = 1;
                    *num_solutions = 1;

                    while (key_count < n && pos < s->size - 2) {
                        unsigned char klen = s->data[pos];
                        if (klen != 33 && klen != 65) break;
                        pos++;
                        if (pos + klen > s->size - 2) break;
                        memcpy(solutions[*num_solutions],
                               s->data + pos, klen);
                        solution_sizes[*num_solutions] = klen;
                        (*num_solutions)++;
                        pos += klen;
                        key_count++;
                    }

                    if (key_count == n && pos == s->size - 2) {
                        solutions[*num_solutions][0] = (unsigned char)n;
                        solution_sizes[*num_solutions] = 1;
                        (*num_solutions)++;
                        *type_out = DOMAIN_SCRIPT_TX_MULTISIG;
                        *matched = true;
                        return ZCL_OK;
                    }
                    *num_solutions = 0;
                }
            }
        }
    }
not_multisig:

    *type_out = DOMAIN_SCRIPT_TX_NONSTANDARD;
    *matched  = false;
    return ZCL_OK;
}

int domain_consensus_script_sig_args_expected(
        enum domain_script_txnouttype t,
        const unsigned char solutions[][65],
        const size_t solution_sizes[],
        size_t num_solutions)
{
    (void)solutions;
    switch (t) {
    case DOMAIN_SCRIPT_TX_NONSTANDARD:
    case DOMAIN_SCRIPT_TX_NULL_DATA:
        return -1;
    case DOMAIN_SCRIPT_TX_PUBKEY:
        return 1;
    case DOMAIN_SCRIPT_TX_PUBKEYHASH:
        return 2;
    case DOMAIN_SCRIPT_TX_MULTISIG:
        if (!solutions || !solution_sizes ||
            num_solutions < 1 || solution_sizes[0] < 1)
            return -1;
        return solutions[0][0] + 1;
    case DOMAIN_SCRIPT_TX_SCRIPTHASH:
        return 1;
    }
    return -1;
}

struct zcl_result domain_consensus_script_extract_destination(
        const struct script *s,
        struct tx_destination *dest_out,
        bool *matched)
{
    if (!s)
        return ZCL_ERR(DOMAIN_CONSENSUS_SCRIPT_STANDARD_ERR_NULL_SCRIPT,
                       "extract_destination: null script");
    if (!dest_out || !matched)
        return ZCL_ERR(DOMAIN_CONSENSUS_SCRIPT_STANDARD_ERR_NULL_OUT,
                       "extract_destination: null out pointer");

    *matched = false;
    dest_out->type = DEST_NONE;

    enum domain_script_txnouttype type;
    unsigned char solutions[20][65];
    size_t solution_sizes[20];
    size_t num_solutions = 0;
    bool solver_matched = false;

    struct zcl_result r = domain_consensus_script_solver(
            s, solutions, solution_sizes, 20,
            &type, &num_solutions, &solver_matched);
    if (!r.ok) return r;
    if (!solver_matched) return ZCL_OK;

    if (type == DOMAIN_SCRIPT_TX_PUBKEY) {
        struct pubkey pk;
        pubkey_set(&pk, solutions[0], solution_sizes[0]);
        if (!pubkey_is_valid(&pk))
            return ZCL_OK;
        dest_out->type = DEST_KEY_ID;
        dest_out->id.key = pubkey_get_id(&pk);
        *matched = true;
        return ZCL_OK;
    }
    if (type == DOMAIN_SCRIPT_TX_PUBKEYHASH) {
        dest_out->type = DEST_KEY_ID;
        memcpy(dest_out->id.key.id.data, solutions[0], 20);
        *matched = true;
        return ZCL_OK;
    }
    if (type == DOMAIN_SCRIPT_TX_SCRIPTHASH) {
        dest_out->type = DEST_SCRIPT_ID;
        memcpy(dest_out->id.script.hash.data, solutions[0], 20);
        *matched = true;
        return ZCL_OK;
    }
    return ZCL_OK;
}
