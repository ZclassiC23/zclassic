/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * fuzz_script — libFuzzer harness for eval_script on untrusted
 * script bytes + a minimal empty stack.
 *
 * No signature checker is installed, so OP_CHECKSIG-family opcodes
 * fall through to the no-checker branch. That's deliberate: we're
 * fuzzing the script interpreter's state machine, not the crypto.
 *
 * eval_script must never read beyond the script or stack bounds
 * under ANY input. This harness is how we catch regressions that
 * might otherwise only manifest under a malicious peer.
 */

#include "script/script.h"
#include "script/script_flags.h"
#include "script/interpreter.h"
#include "script/script_error.h"
#include "chain/chainparams.h"

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Required by process_block.c / sync_controller — provided by main.c
 * in the real binary and by test.c in the test suite. The fuzzer is
 * neither, so we define the global here. */
volatile sig_atomic_t g_shutdown_requested = 0;

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    chain_params_select(CHAIN_MAIN);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0 || size > MAX_SCRIPT_SIZE)
        return 0;

    struct script script;
    script_init(&script);
    script_set(&script, data, size);

    /* Use the first byte (if available) to select verify flags —
     * this gives the fuzzer a cheap way to explore the combinatorial
     * space of CLEANSTACK / P2SH / NULLDUMMY / etc. combinations
     * without making the corpus explode. */
    unsigned int flags = 0;
    if (size > 0) {
        if (data[0] & 0x01) flags |= SCRIPT_VERIFY_P2SH;
        if (data[0] & 0x02) flags |= SCRIPT_VERIFY_STRICTENC;
        if (data[0] & 0x04) flags |= SCRIPT_VERIFY_MINIMALDATA;
        if (data[0] & 0x08) flags |= SCRIPT_VERIFY_LOW_S;
        if (data[0] & 0x10) flags |= SCRIPT_VERIFY_NULLDUMMY;
        if (data[0] & 0x20) flags |= SCRIPT_VERIFY_CLEANSTACK;
    }

    struct script_stack stack __attribute__((cleanup(stack_free))) = {0};
    if (!stack_init(&stack))
        return 0;

    ScriptError serror = SCRIPT_ERR_OK;
    (void)eval_script(&stack, &script, flags, NULL, 0, &serror);

    /* ── Stateless structural parsing on the SAME untrusted bytes ──
     * A malicious peer's scriptPubKey/scriptSig hits these parsers
     * before (and independently of) full evaluation: the opcode walk
     * (script_get_op), the CScriptNum decoder, the sigop counters, and
     * the pattern classifiers. eval_script can short-circuit on the
     * first error and never traverse the tail, so driving these
     * directly reaches bounds logic eval alone may skip. All are pure
     * reads over `script` and must never index past script.size. */

    /* Full opcode walk with a real 520-byte payload sink, mirroring the
     * interpreter's per-op GetOp loop. script_get_op returns false at a
     * truncated/oversized push; we stop there exactly like the VM. */
    {
        size_t pc = 0;
        enum opcodetype opcode;
        unsigned char pushbuf[MAX_SCRIPT_ELEMENT_SIZE];
        size_t pushlen;
        while (script_get_op(&script, &pc, &opcode, pushbuf, &pushlen)) {
            /* Interpret each pushed payload as a CScriptNum, both with
             * and without the BIP62 minimal-encoding rule and at both
             * the default (4) and extended (CLTV, 5) width bounds. This
             * is how the VM turns a stack element into a locktime /
             * count / arithmetic operand. */
            if (pushlen > 0) {
                struct script_num n;
                if (script_num_from_bytes(&n, pushbuf, pushlen,
                                          /*require_minimal=*/true,
                                          SCRIPT_NUM_DEFAULT_MAX_SIZE)) {
                    /* Round-trip a decoded number back to bytes. */
                    unsigned char rt[SCRIPT_NUM_MAX_SIZE];
                    (void)script_num_serialize(&n, rt, sizeof(rt));
                    (void)script_num_get_int(&n);
                }
                (void)script_num_from_bytes(&n, pushbuf, pushlen,
                                            /*require_minimal=*/false,
                                            /*max_size=*/5);
            }
        }
    }

    /* Also walk data-less (data==NULL), the path script_get_sig_op_count
     * uses — it traverses oversized pushes faithfully rather than
     * rejecting them, a distinct branch in script_get_op. */
    {
        size_t pc = 0;
        enum opcodetype opcode;
        while (script_get_op(&script, &pc, &opcode, NULL, NULL))
            ;
    }

    /* Sigop counters (consensus-relevant walks) + the pattern
     * classifiers. Each is a bounded scan over the raw bytes. */
    (void)script_get_sig_op_count(&script, flags, /*accurate=*/true);
    (void)script_get_sig_op_count(&script, flags, /*accurate=*/false);
    (void)script_get_sig_op_count_p2sh(&script, &script, flags);
    (void)script_is_p2sh(&script);
    (void)script_is_p2pkh(&script);
    (void)script_is_pay_to_script_hash(&script);
    (void)script_is_push_only(&script);
    (void)script_is_unspendable(&script);

    return 0;
}
