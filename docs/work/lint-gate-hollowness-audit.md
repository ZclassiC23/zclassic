# Lint-gate hollowness audit (2026-06-19)

**Systemic finding:** many `make lint` gates can report **"clean" / exit 0 while
a real violation is present** — a *fail-silent* (hollow) gate. A hollow gate is
worse than no gate: it gives false "all green" confidence, the exact
self-deception this project fights.

This was found after fixing a real fail-silent bug in
`gate_no_csr_lock_on_finalize_drive.sh` (an awk regex went *fatal* on this gawk,
its output was swallowed by `|| true`, so the scan ran over empty input and
passed an injected violation). An audit of all 33 gate scripts (workflow
`wf_04e65ef5-a98`, 22 agents, each finding empirically reproduced in an isolated
worktree) found the same class is widespread.

## Root cause (one pattern, many gates)

A gate derives its scan set from a `find` / glob / `grep -rl` / hardcoded list,
then **never asserts the scan set is non-empty**, and/or **swallows `grep` exit
≥2 (a real error) as "no match"** via `2>/dev/null || true`. When the producer
silently empties (a renamed symbol/marker/dir, a non-GNU grep, a moved file),
the loop runs zero times, `fail` stays 0, and the gate prints "clean" exit 0.
`set -euo pipefail` does NOT save the `find`/`grep` inside a `< <(...)` process
substitution — its nonzero exit does not abort the parent shell.

## The fix patterns

1. **Conservative preflight (zero-broadening, SAFE — no false-positive risk):**
   assert the scan target exists and the scan set is non-empty (or meets a known
   floor), else `exit 2` LOUD; and check `grep`'s exit explicitly (0=match,
   1=no-match, **≥2=error→fail**) instead of `2>/dev/null || true`. This closes
   the "producer silently emptied → pass" hole without changing what is scanned.
   **This is the default fix.**
2. **Broaden the surface (higher-value, HIGHER-RISK — needs clean-tree proof):**
   e.g. add `domain/` to a `find` root, or content-discover stores instead of a
   hardcoded list. CAUTION: broadening can surface grandfathered matches and
   false-positive the clean tree. Example: a naive content-discovery for
   `gate_stage_log_reorg_unsafe_ratchet` (files with `height INTEGER PRIMARY KEY`
   + `INSERT OR REPLACE`) finds **12** occurrences across `app/`+`lib/storage/`,
   not the baseline 8 — so it must key on a naming convention (`*_log_store.c` /
   `*_log.c`) or a checked-in manifest, not a bare glob. Verify the clean tree
   stays green before shipping any broaden-surface fix.

## Status

### FIXED (inject-verified: clean passes, real violation fires, breakage fails loud)
- **`check_consensus_parity.sh` (E13, the most sacred gate).** Fail-loud preflight
  (every `PATHS` dir must exist → `exit 2`) + explicit `grep` exit handling (≥2 →
  `exit 2`). Zero-broadening; a NEW consensus dir still must be added to `PATHS`
  deliberately (now documented in the gate). Commit `6c4e08a6b`.
- **`check_long_functions.sh`.** End-anchor `/^\}$/` → `/^\}[[:space:]]*(\/\/.*|\/\*.*)?$/`
  so a `}`-with-trailing-whitespace/comment close (`}\t`, `} // end`) no longer
  evades the length check; still rejects `};` / `} while`. Commit `b5ae38dc4`.
- **`check_honest_witness.sh` (Law 7, never-lie).** Producer + awk anchor made
  qualifier-tolerant (`static (inline )?bool witness_`) so an inline witness is
  scanned (and its body extracted, not emptied) + a fail-loud "scanned 0 → exit 2"
  floor. Commit `b5ae38dc4`.
- **`check_no_silent_ready.sh` (E8, never-lie).** Fail-loud preflight: the READY
  authority discovery must be non-empty AND contain the known authority
  `chain_activation_service.c` (a setter/enum rename now fails loud, not silent);
  fixed the stale comment that named the defunct `chain_activation_controller.c`.

### TODO — hollow under a GREEN build (genuine; fix carefully, inject-verify each)
These go hollow via a refactor that *keeps the build compiling*, so CI would be
green while the gate is blind. Highest priority.
- `check_one_write_path.sh` — `find app lib config tools` omits `domain/`, which
  holds consensus code; a chain-state writer there escapes. Fix: add `domain` to
  the find roots — *verify clean tree stays green first (broaden-surface).*
- `check_no_secret_printf.sh` — non-GNU grep (busybox) rejects `--exclude-dir` /
  `\s`/`\b`, exits 2, swallowed → pass. Fix: treat grep ≥2 as fatal + a
  preflight self-test on an inline known-positive/negative fixture. (Local CI
  uses GNU grep, so lower live-likelihood, but cheap to harden.)
- `gate_stage_log_reorg_unsafe_ratchet.sh` — hardcoded 8-file list; a NEW 9th
  store isn't scanned (defeats its stated "catch the pattern spreading" purpose).
  Fix: discover by **naming convention** (`*_log_store.c`/`*_log.c`) or a
  manifest — NOT a bare content glob (finds 12≠8, see caveat above).
- `check_projections_pure.sh` — keys on `*_projection.c` suffix; rename a
  projection file (build stays green via glob) → unscanned. Fix: scan all
  `lib/storage/src/*.c` for the AR-save/app-include violations, or a manifest +
  non-empty floor.
- `check_supervisor_domain.sh` / `check_supervisor_registration.sh` — hardcoded
  worker list / `app/services/src/*.c` glob; a renamed/relocated TU (build green
  via Makefile glob) silently drops out. Fix: non-empty/`nullglob` floor +
  derive the worker list by grepping spawn tokens.
- `check_stage_advances_or_blocks.sh` — `is_job_step_file` filter keys on
  `stage_create(`/`_step`; a registration/convention rename drops the file. Fix:
  pin expected step-file count (≥8) + broaden the signature match.

### TODO — hollow ONLY under a core-dir rename (degenerate; build breaks → CI red anyway)
Lower priority (the precondition can't coexist with a green build), but the
conservative "scanned 0 files → exit 2" preflight is cheap and defensive:
- `check_one_result_type.sh`, `framework_shape_check.sh`,
  `check_file_size_ceiling.sh`, `check_no_new_repair_rung.sh`,
  `check_coins_lookup_nullcheck.sh` (also has a symbol-rename green-build vector).

## Recommendation

Add a shared `tools/lint/gate_lib.sh` providing `gate_require_scanned <count>
<name>` (fail-loud if the scan set is empty/below a floor) and a
`gate_grep` wrapper that fails on grep exit ≥2, then retrofit the gates above.
Fix the GREEN-build-hollow set first; inject-verify each (clean passes, real
violation fires, producer-breakage fails loud) before greening. Do NOT ship a
broaden-surface fix without confirming the clean tree stays green.

Full per-gate evidence (the exact injection + commands + exit codes that proved
each one hollow): workflow `wf_04e65ef5-a98` result, archived under the session
tasks dir.
