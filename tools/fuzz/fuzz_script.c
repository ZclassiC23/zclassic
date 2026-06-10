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

    return 0;
}
