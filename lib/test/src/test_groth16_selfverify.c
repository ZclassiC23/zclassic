/* Diagnostic: bisect the Sapling output prover->verifier round-trip.
 *
 * Answers: does groth16_prove emit an (A,B,C) that satisfies the
 * Groth16 pairing relation against the prover's OWN VK and the
 * circuit's OWN public inputs (witness[1..5]) — WITHOUT the
 * serialize/deserialize step and WITHOUT the consensus verifier's
 * public-input reconstruction from cv/cm/epk?
 *
 * If self-verify PASSES  -> bug is at the prover->consensus boundary
 *                            (serialization or public-input derivation
 *                             or a VK difference).
 * If self-verify FAILS   -> the proof math / circuit / params
 *                            consumption is broken (QAP mismatch).
 */

#include "test/test_helpers.h"
#include "sapling/sapling.h"
#include "sapling/params_init.h"
#include "sapling/sapling_circuit.h"
#include "sapling/groth16_prover.h"
#include "sapling/bls12_381.h"
#include "sapling/fr.h"
#include "util/safe_alloc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static uint8_t *slurp(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = zcl_malloc((size_t)sz, "selfverify_params");
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) { free(buf); return NULL; }
    *out_len = (size_t)sz;
    return buf;
}

static bool find_div(uint8_t d[11])
{
    memset(d, 0, 11);
    for (int i = 0; i < 256; i++) {
        d[0] = (uint8_t)i;
        if (sapling_check_diversifier(d)) return true;
    }
    return false;
}

static void fr_raw(uint64_t raw[4], const struct fr *a)
{
    uint8_t b[32];
    fr_to_bytes(b, a);
    memcpy(raw, b, 32);
}

int test_groth16_selfverify(void);
int test_groth16_selfverify(void)
{
    printf("\n=== groth16 self-verify bisection (Sapling output) ===\n");
    int failures = 0;

    const char *home = getenv("HOME");
    char params_dir[512], out_path[600];
    snprintf(params_dir, sizeof(params_dir), "%s/.zcash-params",
             (home && *home) ? home : ".");
    snprintf(out_path, sizeof(out_path), "%s/sapling-output.params", params_dir);

    if (!sapling_init_params(params_dir)) {
        printf("  params init failed — SKIP\n");
        return 0;
    }

    size_t pk_len = 0;
    uint8_t *pk_data = slurp(out_path, &pk_len);
    if (!pk_data) { printf("  slurp params failed — SKIP\n"); return 0; }

    struct groth16_pk pk;
    if (!groth16_pk_read(&pk, pk_data, pk_len)) {
        printf("  groth16_pk_read FAILED\n"); free(pk_data); return 1;
    }
    printf("  pk: h=%zu l=%zu a=%zu b=%zu inputs=%zu ic_len=%zu\n",
           pk.h_len, pk.l_len, pk.a_len, pk.b_len, pk.num_inputs, pk.vk.ic_len);

    /* Build a valid output witness (mirrors test_snark_kat KAT B). */
    uint8_t div[11];
    bool div_ok = find_div(div);
    uint8_t esk[32], pk_d[32], rcm[32];
    sapling_generate_r(esk);
    sapling_generate_r(rcm);
    /* derive a valid pk_d */
    uint8_t ask[32], nsk[32], ak[32], nk[32], ivk[32];
    sapling_generate_r(ask);
    sapling_generate_r(nsk);
    sapling_ask_to_ak(ask, ak);
    sapling_nsk_to_nk(nsk, nk);
    sapling_crh_ivk(ak, nk, ivk);
    bool pkd_ok = div_ok && sapling_ivk_to_pkd(ivk, div, pk_d);
    printf("  div_ok=%d pkd_ok=%d\n", div_ok, pkd_ok);

    struct sapling_output_witness wit;
    memset(&wit, 0, sizeof(wit));
    wit.value = 12345;
    memcpy(wit.diversifier, div, 11);
    memcpy(wit.pk_d, pk_d, 32);
    memcpy(wit.rcm, rcm, 32);
    memcpy(wit.esk, esk, 32);
    sapling_generate_r(wit.rcv);

    struct sapling_output_inputs pub;
    memset(&pub, 0, sizeof(pub));
    /* cv from value/rcv */
    sapling_value_commit(wit.value, wit.rcv, pub.cv);
    sapling_ka_derivepublic(div, esk, pub.epk);
    sapling_compute_cm(div, pk_d, wit.value, rcm, pub.cm);

    struct constraint_system cs;
    cs_init(&cs);
    if (!sapling_output_synthesize(&cs, &wit, &pub)) {
        printf("  synthesize FAILED\n"); cs_free(&cs);
        groth16_pk_free(&pk); free(pk_data); return 1;
    }
    printf("  circuit: num_vars=%zu num_inputs=%zu num_constraints=%zu\n",
           cs.num_vars, cs.num_inputs, cs.num_constraints);
    printf("  num_aux(circuit)=%zu ; pk l_len(num_aux setup)=%zu ; a_len=%zu b_len=%zu\n",
           cs.num_vars - cs.num_inputs - 1, pk.l_len, pk.a_len, pk.b_len);

    struct groth16_proof proof;
    if (!groth16_prove(&pk, &cs, &proof)) {
        printf("  groth16_prove FAILED\n"); cs_free(&cs);
        groth16_pk_free(&pk); free(pk_data); return 1;
    }

    /* Public inputs straight from the circuit witness (vars 1..num_inputs). */
    size_t ni = cs.num_inputs;
    uint64_t (*pin)[4] = zcl_calloc(ni, sizeof(uint64_t[4]), "pin");
    for (size_t i = 0; i < ni; i++)
        fr_raw(pin[i], &cs.witness[1 + i]);

    printf("  --- SELF-VERIFY (pk.vk, circuit public inputs, NO serialize) ---\n");
    bool self_ok = groth16_verify(&pk.vk, &proof, pin, ni);
    printf("  self_verify = %s\n", self_ok ? "TRUE" : "FALSE");

    free(pin);
    cs_free(&cs);
    groth16_pk_free(&pk);
    free(pk_data);

    printf("=== self-verify bisection: %s ===\n",
           self_ok ? "PROOF SELF-CONSISTENT (boundary bug)"
                   : "PROOF NOT SELF-CONSISTENT (math/circuit/params bug)");
    (void)failures;
    return 0; /* diagnostic, never gates */
}
