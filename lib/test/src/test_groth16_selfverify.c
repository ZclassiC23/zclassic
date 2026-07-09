/* Positive Sapling prover capability gate.
 *
 * This test used to print FALSE for the native prover's self-verification and
 * deliberately return success. That made a broken prover indistinguishable
 * from a healthy one. The production parameter loader now runs a complete
 * Spend + Output + binding-signature bundle through the independent C23
 * consensus verifier before enabling proving. This test makes that result a
 * hard assertion and independently exercises the public Output API.
 */

#include "test/test_helpers.h"

#include "sapling/params_init.h"
#include "sapling/sapling.h"
#include "sapling/sapling_prover.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROVER_CHECK(name, expr) do {          \
    printf("  %s... ", (name));                \
    if ((expr)) printf("OK\n");                \
    else { printf("FAIL\n"); failures++; }     \
} while (0)

static bool find_diversifier(uint8_t d[11])
{
    memset(d, 0, 11);
    for (unsigned int i = 0; i < 256; i++) {
        d[0] = (uint8_t)i;
        if (sapling_check_diversifier(d))
            return true;
    }
    return false;
}

int test_groth16_selfverify(void);
int test_groth16_selfverify(void)
{
    printf("\n=== Sapling prover -> consensus verifier capability ===\n");
    int failures = 0;

    const char *home = getenv("HOME");
    char params_dir[512];
    char output_path[640];
    snprintf(params_dir, sizeof(params_dir), "%s/.zcash-params",
             (home && *home) ? home : ".");
    snprintf(output_path, sizeof(output_path),
             "%s/sapling-output.params", params_dir);

    FILE *probe = fopen(output_path, "rb");
    if (!probe) {
        printf("  params absent — SKIP\n");
        return 0;
    }
    fclose(probe);

    const bool initialized = sapling_init_params(params_dir);
    PROVER_CHECK("parameter loader completed", initialized);
    PROVER_CHECK("backend provenance is pinned librustzcash",
                 strcmp(zclassic_sapling_prover_backend(),
                        "librustzcash-06da3b9ac8f2") == 0);
    PROVER_CHECK("full Spend+Output+binding self-test returned true",
                 initialized && zclassic_sapling_prover_run_self_test());
    PROVER_CHECK("proving capability is READY",
                 zclassic_sapling_prover_is_ready() &&
                 strcmp(zclassic_sapling_prover_status(), "ready") == 0);

    if (zclassic_sapling_prover_is_ready()) {
        uint8_t diversifier[11];
        uint8_t ask[32], nsk[32], ovk[32];
        uint8_t ak[32], nk[32], ivk[32], pk_d[32];
        bool keys_ok = find_diversifier(diversifier) &&
                       sapling_generate_r(ask) &&
                       sapling_generate_r(nsk) &&
                       sapling_generate_r(ovk);
        if (keys_ok) {
            sapling_ask_to_ak(ask, ak);
            sapling_nsk_to_nk(nsk, nk);
            sapling_crh_ivk(ak, nk, ivk);
            keys_ok = sapling_ivk_to_pkd(ivk, diversifier, pk_d);
        }
        PROVER_CHECK("independent output recipient constructed", keys_ok);

        void *pctx = keys_ok ? zclassic_sapling_proving_ctx_init() : NULL;
        uint8_t cv[32], cm[32], epk[32], proof[192];
        uint8_t enc[580], out[80];
        bool built = pctx && sapling_build_output_with_ctx(
            pctx, ovk, diversifier, pk_d, UINT64_C(54321), NULL,
            cv, cm, epk, enc, out, proof);
        PROVER_CHECK("public proving API produced an output proof", built);

        if (built) {
            struct sapling_verification_ctx vctx;
            sapling_verification_ctx_init(&vctx);
            PROVER_CHECK("independent C23 consensus verifier accepts proof",
                         sapling_check_output(&vctx, cv, cm, epk, proof));

            uint8_t bad_proof[192];
            memcpy(bad_proof, proof, sizeof(bad_proof));
            bad_proof[191] ^= 1;
            sapling_verification_ctx_init(&vctx);
            PROVER_CHECK("tampered proof is rejected",
                         !sapling_check_output(
                             &vctx, cv, cm, epk, bad_proof));
        }
        if (pctx)
            zclassic_sapling_proving_ctx_free(pctx);
    }

    printf("Sapling prover capability: %s (%d failures)\n",
           failures == 0 ? "OK" : "FAIL", failures);
    return failures;
}
