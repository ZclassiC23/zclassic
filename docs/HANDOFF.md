> **Read this file first for current live-node state.** Older revisions are
> evidence, not standing fact — recover them with
> `git log --follow -- docs/HANDOFF.md`.

# HANDOFF — current state

The typed status commands are ground truth; this page is a pointer to
evidence files, never a substitute for re-checking them:

```bash
zclassic23 status
zclassic23 dumpstate reducer_frontier
tail -5 ~/.local/state/zclassic23-slo/uptime-ledger.jsonl
```

## Current state

The canonical node is holding network tip on the self-verified (cured)
stack, per the external SLO ledger: `~/.local/state/zclassic23-slo/uptime-ledger.jsonl`
records canonical served height against `gap_vs_oracle`. Treat any
"at tip" / "holding tip" claim as unverified until that ledger's most recent
line confirms it.

## `main` is ahead of the running binary

The canonical node runs a binary built before the current `main`. Everything
below is committed and gated (`make lint`, `make test-parallel`) but **not
deployed**, and none of it is live until an owner-gated `make deploy`:

| In `main`, not running | Where |
|---|---|
| Cost-tiered onion admission (STATIC/CHEAP/EXPENSIVE) + adaptive client puzzle; one admission primitive, the duplicate retired | `lib/net/src/onion_ratelimit.c`, `lib/net/src/puzzle.c` |
| Wallet mutating commands bound to the RPC engine (address new/import/export-key, transaction send, shielded send, rescan, backup-now) and address labels | `docs/API_REFERENCE.md` marks these `ready` |
| Groth16 comb-based verify speedup behind a differential parity oracle | `make check-groth16-parity`, `make bench-groth16-comb` |
| Sapling-tree rebuild seeded from the anchor_kv frontier (the healer for the blocker below) | merged earlier as `82f11c697` |
| Fault-injection and convergence proof harnesses; sync shadow observer; prune-after-seal safety gate; time authority | test groups + `lib/util/src/time_authority.c` |
| `lib/base` — the dependency sink (logging, allocation, the result type). `util/log_macros.h` and `util/result.h` forward to it, so the 733 includers are untouched | `lib/base/` |
| `config/` no longer reachable from `lib/`: the 14 upward symbol references (5 of them bare `extern`, invisible to the include-grep gate) replaced by boot-registered ports | `lib/net/src/net_runtime_port.c`, `lib/storage/src/node_db_runtime.c` |
| Per-file test headers: `test_core.h` + facets, so a node-header edit no longer dirties ~36% of the test build | `lib/test/include/test/` |
| Two ISA paths that had never compiled into a shipped binary — AVX-512 SHA3-512 (missing permutation + a 32-byte over-read) and SHA-NI SHA-256 (now runtime-dispatched with a known-answer self-test) | `lib/crypto/src/` |
| Boot refusals that fire before the log exists now report themselves; the doc they name is guaranteed to exist by `check-error-doc-refs` | `config/src/boot_error.c`, `tools/lint/check_error_doc_refs.sh` |
| **The vault** — one read model over all six asset classes (transparent, shielded, tokens, names, market offers, swaps), each row carrying an evidence grade. Funds locked in a swap HTLC are counted for the first time; `getbalance` still cannot see them | `app/services/src/vault_read.c`, `tools/command/native_vault_command.c`, `zclassic23 vault list` |
| Wallet-wide sweeps for ZSLP balances, ZNAM names, and shielded notes — previously every one of these could only answer per-address | `app/services/src/`, bound into the vault read model |
| Three broken commands fixed, all one defect: a handler returned a bare JSON array or an in-memory-only field, and the command bridge dropped it. `app.swap.list` and the three wallet plan legs now return usable bodies | `swap_controller.c:970`, `wallet_native_handlers.c` |
| The `lib/` module set is declared once in `config/lib_module_order.def` and derived everywhere else — it had been copied into five places, and two of the cross-checks were vacuous | `Makefile:272`, `tools/lint/repo_shape.sh` |
| The code index reads the depfiles the build actually writes (it had been reading a stale flat directory), so the test skip-cache can key on a real include graph: **0 → 677 of 749 groups cacheable** | `lib/codeindex/src/codeindex_deps.c` |
| Compile-epoch re-key on toolchain+flags: a source edit now recompiles 2 TUs instead of 1199 | `Makefile`, `tools/dev/` (pushed `cf63879e8`) |
| Boot-liveness watchdog pump during block-index hydrate + LevelDB load (the long silent window that read as a stall) | `app/services/src/block_index_blocks_hydrate.c` (`2950a8111`) |
| Stale `.failed` bundle marker cleared once the bundle actually installs | `config/src/boot_auto_install_bundle.c` (`eb3e943d7`) |
| `op_return_index` names the snapshot-seed floor instead of a fake stall; `catalog_lag_exceeded` suppressed while the seed floor is the cause | `8181b9dee`, `3eb29dfed` |
| Wipe-to-tip Wedge A: genesis (nVersion=0) exempted from the contextual header-version gate; regression test | `app/jobs/src/validate_headers_validator.c` (`1aa370c7c`, `71f084afe`) |
| Plaintext wallet-key mirror deleted — one encryption-aware writer for the node.db key tables; boot-time WKS1 scrub of legacy plaintext rows, fail-loud | `lib/wallet/src/wallet_sqlite.c`, `app/models/src/wallet_key.c` (`d027c034b`) |
| Wipe-to-tip Wedge B: getheaders serve path falls back to the hash-bound node.db `blocks` row below the snapshot body floor (full Equihash solution); serve refusals no longer poison entries with `BLOCK_FAILED_VALID`; the successor walk advances instead of ending the reply | `lib/net/src/msg_headers.c`, port `storage/node_db_runtime.h` (`44bf0426f`) |
| ZCODE slice 1: signed package release envelope codec (703-byte bounded wire, frozen KAT, SPDX allowlist, low-S secp256k1) | `lib/vcs/package_release.*` (`fb37cc885`) |

In flight on branches/worktrees at push time (`main` at `fb37cc885`):
ZCODE slice 2 staging CAS (`work/zcode-slices`), the end-to-end web UX
design system (`zclassic23-ux`), and the node.db lock-contention fixes
(`zclassic23-dblock`). Program plans: `docs/work/ZCODE_PLAN.md`,
`docs/work/UX_PLAN.md`, `docs/work/NAT_AND_ONION_TRANSPORT.md`.

Deploy policy during the hold window is unchanged: a restart resets the 72h
trailing window, so deploy when the escalator fires anyway, or after
`HOLD_PROVEN`. Do not restart the canonical node for any other reason.

## Verify the cure

| Claim | Evidence file |
|---|---|
| Anchor-refold rebuild applied, revert path kept | `~/.local/state/zclassic23-cure/verdict.jsonl`; backups under `~/.zclassic-c23-PREINSTALL-*` and `~/.zclassic-c23-PREPROMOTE-*` |
| Tip-holding, externally confirmed | `~/.local/state/zclassic23-slo/uptime-ledger.jsonl` (`gap_vs_oracle`, `ts=`) |
| 72h hold accrual toward `HOLD_PROVEN` | `~/.local/state/zclassic23-slo/hold-ledger.jsonl`; judge: `tools/scripts/slo_hold_judge.sh`; recorder: `zclassic23-hold-certifier.timer` (`make install-hold-certifier`) |

## Open blockers

- **`sapling_tree_rebuild.fail_closed`** is a known-junk blocker, not
  corruption. When the legacy sapling-tree-checkpoint copy is discarded at
  boot for a root mismatch, the deferred rebuild replays from Sapling
  activation over bodies that do not exist below the cure anchor, so its
  first appended commitment is guaranteed to mismatch. The canonical
  incremental tree (`fold_sapling`, anchor_kv) independently verifies against
  every block header's `hashFinalSaplingRoot` and is healthy. Fix: seed the
  rebuild from the anchor_kv frontier instead of replaying from genesis, and
  reclassify skip-tainted mismatches as DEPENDENCY, not PERMANENT. Until
  fixed, this blocker keeps the escalator's empty-escape set non-empty.
- **Label-splice wedge class** — a NULL `block_hash` in a stale replay
  artifact can freeze H* during live operation, not only as wedge-era
  residue. The escalator's 600-block resnapshot refold self-cures it (crude,
  always-terminating). The merged in-place healer fixes the fault at the
  source; deploy it the next time this class fires.

## Open branches — work that is finished but did not pass review

Four branches exist because an independent verifier ran the gates and found a
specific defect, not because they ran out of time. Each names what to fix.
Re-verify before trusting any of it; all four forked before the merges above
and need a reconcile pass.

| Branch | What it does | Why it is not in `main` |
|---|---|---|
| `perf/stable-objdir-and-gold-linker` | Stops relocating all 1883 objects on every edit; per-object attestation replaces the whole-tree key. **5x less CPU per edit, independently reproduced.** | Wall time got ~11% WORSE on a 32-core box — the wasted work was hiding in spare cores, and the new per-link verifier costs ~1s. Also: editing a per-object CFLAGS line in the Makefile changes no source file, so the toolkey does not move and a stale object survives — a real regression against the guarantee it replaces. And `check-build-epoch-integrity` can now report PASS while nothing ran (its cache key omits the two new scripts; copy-proven). **Goal achieved differently 2026-07-27 (item 2 below): toolchain+flags epoch key + `BUILD_SYSTEM_ID`, no per-object verifier; all three defects closed. This branch is superseded.** |
| `lane/testcache-soundness-phase0` | Fixes three live soundness bugs in the test skip-cache: `.def` files were absent from the key, the key bound the compiler but not the flags (an `-O1` pass was honoured by the `-O3` gate), and a fresh tree silently produced a header-free key. Toolkeys now differ per profile, proven by `strings` on the binaries. | One of its own new assertions passes on the unmodified code too, so it pins nothing. It also mutates `ZCL_STRESS_TESTS` without restoring it, which suppresses coverage in the sequential runner, and its Makefile-scanning helper reads only the first 256 KB of a 327 KB file. |
| `lane/module-linkgraph-enforcement` | Measures the module graph from the linker (`nm`), declares the rank order in a `config/lib_module_order.def` that exists only on that branch, and adds two gates. Also fixes a real bug: the code index skipped `epochs/`, which is where 100% of live depfiles are, so its include graph was empty. | `check-no-cross-layer-extern` false-fails on a clean tree whenever the only warm object tree is a test tree — proven end-to-end with no env override. The gate docstrings also claim a full `make` arms them; it does not (a full `make` produces zero `.o` files). | <!-- doc-path-ok: config/lib_module_order.def exists only on that unmerged branch -->
| `wf/measure` | Block-fold pipeline instrumentation. | Never reviewed: it wraps every stage, so it needs a deliberate overhead measurement before landing. Its profiling datadir copy (`~/.zclassic-c23-COPY-20260725-001046-fold-profile`, 17 GB) is intentionally left in place — delete it if this lane is abandoned. |

`archive/stash-refold-driver-wip` is older experimental work preserved from a
stash, not a live lane.

## Picked up next — known open items

Ordered by how much they cost the next person. Everything here was found with
evidence and left deliberately, not forgotten.

1. **`core.wallet.utxo.list`: the envelope defect is CLOSED; a different one
   was underneath it.** The `{schema, utxos, count}` wrapper landed in
   `wallet_native_read_bodies.c` and `test_command_registry_catalog.c` drives it
   through the registry on rendered bytes, so that instance of the bare-array
   class is done. The `TOOL_ERROR "RPC listunspent returned an unparseable
   body"` still seen afterwards was **not** that defect: `node_rpc_call`
   returned the *truncated* reply when its deadline fired mid-body (the node
   writes HTTP headers before its handler blocks), and every caller then
   complained about the body's shape instead of about a busy node. Fixed in
   `rpc_client.c` — a timed-out read that does not parse is now named as a
   timeout; pinned by `test_native_api_contract.c` against a real socket that
   sends only headers. This affected **every** native read body, not just this
   leaf.

   The 10 s latency on that command was a **third defect — CLEARED by the
   2026-07-27 05:36 process restart, root cause still unknown.** The previous
   process ran poisoned from ≥2026-07-24 09:00 until the restart: every
   catchup `COMMIT` failed *"cannot commit transaction - SQL statements in
   progress"* (`catchup: initial COMMIT failed — aborting`, 13k+ consecutive
   failures, every ~13–19 s) while other writers logged `database is locked`
   in bursts. Since the restart: zero recurrences, catchup completes 1-block
   passes, `getbalance`/`listunspent` answer in ~0.02 s (first call 3 s cold).

   What is established: that exact error string is emitted only from the
   `db->nVdbeWrite>0` COMMIT guard (`vendor/sqlite3.c:97501`) — i.e. a write <!-- doc-path-ok: vendored sqlite3 amalgamation exists on disk but is git-ignored -->
   VM (INSERT/UPDATE/DELETE, incl. `…RETURNING`) was left in `VDBE_RUN_STATE`
   on the shared node.db handle of the old process. It is **runtime-only
   state**: a fresh boot on a crash copy of the same datadir
   (`ZCL_DB_TXN_TRACE=1` probe, 170 s, isolated ports) showed zero busy
   statements and a clean catchup; the poison is not in the datadir. A
   100 %-persistent RUN write VM points to a thread parked mid-`sqlite3_step`
   (or abandoned between step and reset) on the main handle — not found by
   static audit; the only `RETURNING` writer (`db_app_event_save`,
   `app/models/src/app_event.c:206`) finalizes correctly.

   If it recurs on the CURRENT live binary: the process is poisoned until
   restart, and **`ZCL_DB_TXN_TRACE=1` names the culprit** — a RUN write VM forces
   `txn_state=WRITE`, so the tracer's busy-stmt walk fires every 3 s and logs
   the exact SQL (`lib/util/src/db_txn_trace.c`). Enable it at the next
   owner-gated deploy. On branch `work/db-lock-fixes` (committed there, NOT
   on the live binary) the always-on seatbelt lands instead: a COMMIT
   failing with this exact error class walks the handle's
   `sqlite3_next_stmt` list, logs the offending SQL, and resets the
   abandoned VM, so the caller's ROLLBACK and the next BEGIN/COMMIT work
   without a process restart (`node_db_commit`,
   `app/models/src/database.c`). The same branch caps the catchup batch at
   2000 blocks / 5 s per COMMIT (was one 100k-block transaction holding
   the WAL write lock for minutes), opens every catchup transaction with
   BEGIN IMMEDIATE, restarts a `SQLITE_BUSY_SNAPSHOT` pass with a bound of
   3 whole-walk retries, and parks the catchup worker behind the named
   blocker `node_db_catchup.abort_storm` after 8 consecutive aborts
   instead of re-running forever. The reset unpoisons the handle but does
   not find the bug that abandoned the VM — the recovery's logged SQL (or
   `ZCL_DB_TXN_TRACE=1`) still names it. **Live gdb is unblocked (2026-07-27):** commit
   `f191bbf9c` was rebuilt byte-identical in
   `~/github/zclassic23-dbg-f191bbf9c` (sha256
   `63c8206c…df70d49d`, build-id match) and the matching
   `zclassic23.debug` (CRC `0x5b202eb1`, verified against the live binary's
   `.gnu_debuglink`) is installed at `~/.local/bin/zclassic23.debug` —
   `gdb -p <live-pid>` now resolves full DWARF. Repro note for future
   two-builder proofs: `make worktree-prime` copies `vendor/lib` but NOT
   `vendor/include`; the 135 generated OpenSSL headers + `zconf.h`/`zlib.h`
   must be copied too or `source-identity.sh` mints a different
   `ZCL_BUILD_SOURCE_ID` (84-byte binary diff). Live binary facts: real Tor
   (`vendor/tor` @ `b65701fe`), no `ZCL_WITH_RUST`, GCC 15.2.1.
2. **DONE 2026-07-27 — the compile epoch is re-keyed on toolchain+flags; incremental rebuilds are incremental again.** `zcl_compile_epoch` (`Makefile`, `define zcl_compile_epoch`) no longer binds the whole-tree source id/mutation. The epoch key is now: compiler/toolchain fingerprint + profile name + effective compile flags + effective link flags + `BUILD_SYSTEM_ID` (new `build-epoch-key.sh build-system-id` mode: the root Makefile — every flag variable and per-object override — plus the four epoch driver scripts). A source edit recompiles only make's stale TUs in the STABLE epoch; a Makefile/flags/toolchain edit busts every epoch. Identity freshness is unchanged: every profile's `clientversion.o` depends on `$(BUILD_IDENTITY_STAMP)`, so a source-identity move rebuilds it and relinks every binary (proven: after a one-line edit the dev binary's baked `ZCL_BUILD_SOURCE_ID` updates in the stable epoch dir). Measured (`docs/BENCHMARKS_LOG.md` 2026-07-27): one-line `.c` edit 1199→2 compile invocations, header edit 1199→4, ~35s→~7s CPU per edit; wall ~7s→~6.3s (the floor is parse-time source capture, not compilation). The three defects that sank `perf/stable-objdir-and-gold-linker` are closed: per-object CFLAGS edits move `BUILD_SYSTEM_ID` (proven: dev epoch re-keys, all 1207 dev TUs scheduled); the `check-build-epoch-integrity` cache key now includes the cache driver itself plus every script the probes read (proven: editing an input forces a real ~12.7s rerun, no stale PASS); no per-object verifier was added — the win is make's normal timestamp+depfile incrementality. Known residual vs the old design: the ABA quarantine-by-namespace is gone (a compile racing an edit in the same seconds window can leave a newer-than-source object; publish-time verify-record still refuses the binary). `build-epoch-selftest.sh` was rewritten for the new semantics (stable namespace across A→B→A, all five key inputs bound, retired 7-arg derivation fail-closed, source-bound publish preserved).
3. **A passing test can still be hollow.** The wallet plan bug survived because
   its test asserted on the in-memory reply struct, which was correct, while
   the serializer that the caller actually reads never emitted the field. The
   rewritten test (`lib/test/src/test_native_api_contract.c`) executes through
   `zcl_command_registry_execute_json` and asserts on rendered bytes. Any new
   command test should do the same. **Sibling sweep DONE 2026-07-27:** all 9
   other test files that call `zcl_native_handle_*` directly were checked.
   The architecture limits the blast radius — `serialize_reply`
   (`lib/kernel/src/command_registry.c:1625`) copies `reply.data` wholesale,
   so a per-field drop like the wallet bug is impossible by construction;
   what a never-rendering test CANNOT see is whole-envelope failure
   (`write_bounded_json` budget overflow, `next`-array self-name rejection —
   the actual shipped bug). Narrow hole (never renders):
   `test_offline_datadir_query.c` (top rewrite candidate — rows payload can
   realistically exceed `ZCL_COMMAND_RESULT_BUDGET` → production
   `EXECUTE_FAILED` while the test stays green), `test_code_capsule.c`
   (largest payloads), `test_operator_ux.c` (5 success paths, zero
   rendering), `test_code_impact.c` (minor — already budget-fit renders),
   `test_vault_session.c` / `test_vault_dispatch.c` (low value — asserted
   fields live in generically-serialized portions). Already render-through:
   `test_code_merkle.c`, `test_code_emitter.c`. False alarm:
   `test_rom_fetch.c` (error-only paths; `push_error` is generic).
4. **RESOLVED 2026-07-27 — the `MISSING_ADDRESS` ERROR line is the handler's
   own error-path logging, working as designed.** `test_native_api_contract`
   step 5 deliberately drives `zcl_native_handle_wallet_transaction_send`
   with an empty input object (`test_native_api_contract.c:911-924`) to
   prove the missing-key path fails closed; the handler's `wnh_fail`
   (`app/controllers/src/wallet_native_handlers.c:183`) calls
   `LOG_ERROR(WNH_TAG, "%s: %s (%s)", code, …)` on *every* failure —
   including this intentional one. The pre-rewrite test asserted on an
   in-memory struct without executing the real handler, so it never
   triggered the log. One ERROR line per negative test of an error path is
   the defensive-coding contract doing its job, not a defect. (If the noise
   ever bothers anyone, the fix would be a client-input-vs-internal
   severity split in `wnh_fail` — a style decision, not a bug.)
5. **ZNAM and market write verbs are unrouted.** They are registered `PLANNED`
   with NULL handlers, so the vault can report those classes but cannot act on
   them. The market class is additionally the one vault row graded
   `heuristic_payment_address_match`, because `file_offers` carries no
   ownership marker at all — that column has to exist before the row can be
   exact.
6. **The package library (P1) is not started.** The design is written and
   decided: `docs/P2P_SOURCE_HOSTING.md` items 3–12, plus a publish-time SPDX
   allowlist gate. Two of twelve pieces exist and are tested
   (`lib/vcs/package_manifest.*`, `lib/vcs/package_swarm.*`); the codec has
   never been given a socket. Owner decisions on record: hash-only package
   identity, license enforced mechanically at publish, free distribution by
   default.
7. **The ontology work is paused mid-design**, at `ba4ded785` + `0343defc3`
   (`config/onto/extractors.def` + `tools/lint/onto_shape.sh` are landed and
   green). Three lane designs were returned and saved under
   `~/.claude/plans/`; worktrees `zclassic23-onto-{a,b,c}` exist with no
   commits. Nothing depends on it.
8. **Boot crash-loop on corrupt block-index flat file — root-caused, fix
   landed (ungated, uncommitted).** Evidence: 2026-07-27 04:19 the poisoned
   2d13h process (item 1) was cleanly stopped; every boot for the next hour
   then died to the 2-min systemd watchdog (8 SIGABRTs in `crash_log.txt`,
   each a fresh PID in `main`→`sleep`). Mechanism: the flat file's tip hash
   mapped to height −1 vs the SQLite store
   (`Block index flat: tip hash maps to wrong height (-1 vs SQLite …)`), so
   `rung_taint_load` (`config/src/boot_blkidx_ladder.c:115`) dropped `loaded`
   and set `flat_union_tainted`, forcing the multi-minute blocks-table
   hydrate / LevelDB reload — which pumped NO boot-liveness marker, so the
   watchdog killed the boot at 2 min; the deliberate "healed map persisted
   only at shutdown" guard then re-infected the next boot, sustaining the
   loop until a warm-page-cache boot finally finished in time (~05:34, the
   current healthy process). Fix: `boot_progress_note()` every 4096 rows in
   the hydrate validate/insert/link passes
   (`app/services/src/block_index_blocks_hydrate.c`) and in the LevelDB guts
   walk (`lib/storage/src/block_index_db.c`) — the same primitive the flat
   loader already pumps. The taint guard itself is untouched (persisting
   the union mid-boot would launder the poison record). Regression test:
   `test_block_index_loader.c` §15b asserts >4096 hydrate rows advance
   `boot_progress_marker()`; a `test_chain.c` sibling asserts the same for
   the LevelDB guts walk. Remaining: full
   `make lint && make test-parallel` gate, then owner-gated deploy. If the
   loop ever recurs pre-deploy, the escape is what happened on its own:
   repeat boots until one completes inside the window.

9. **C6 soak can never accrue on the current datadir — the blocker chain is
   structural, not a flag to clear.** Diagnosed read-only 2026-07-27.
   `mvp_gate.sh` hard-gates soak accrual on `security_posture_ok`
   (`tools/mvp_gate.sh:339`): `review_required` → `NOT_MET` regardless of
   uptime. The live posture is `review_required_bootstrap_trust` with
   `next_action=finish_sovereign_refold_and_full_history_validation`
   (`app/controllers/src/agent_security_posture.c:338`), i.e.
   `trusted_state_present || !full_history_validation_complete`. Live
   evidence (`dumpstate chain_evidence_controller`): `snapshot_anchor=-1`,
   `snapshot_evidence.source_class="unknown"`, every verify flag false;
   `dumpstate bg_validation`: `verified_height=-1`, idle, zero sigs/proofs.
   Root cause: the datadir is snapshot-seeded
   (`reducer_trusted_base_height=3195552` in `operatorsnapshot`) — block
   bodies below the seed floor were never downloaded, so genesis-to-tip
   full-history validation is impossible *on this datadir*, which is also
   why the `address_index.below_snapshot_seed` / `txindex` blockers are
   permanent there ("backfill cannot fold at height 0 … no source"). The
   only clearing path is the sovereign cure itself (self-verified-tip-plan:
   fold real bodies from genesis/anchor → CEC `FULLY_VALIDATED` with origin
   `GENESIS_HISTORY` → `trusted_state_present` flips false). Consequences:
   (a) C6's 168h clock has not started and cannot start pre-cure — do not
   read uptime as soak progress; (b) the cure must run copy-proven on a
   datadir copy per the runbook, then the soak window restarts on the
   post-cure datadir; (c) any "just clear the flag" change is a
   gate-weakening — refuse it.
10. **First genuine wipe→tip stopwatch run: STALLED-NAMED at h=0 for the
    full 606s (2026-07-27).** `make mvp-coldstart-to-tip-stopwatch` (empty
    `/tmp` datadir, isolated `$HOME`, `-nolegacyimport`, no bundle/snapshot
    flags, one synced peer) never made progress: H\*=0 against
    network_tip=3195667 in every sample; exit 4. Artifact:
    `build/c3-stopwatch/20260727T080317Z-3413759/`. Named blockers:
    `bootstrap.no_state_source` ("the node is doing full from-genesis IBD"
    — but it didn't), `proof_validate.stale_upstream_hash` (fired 312×:
    holds until script_validate publishes a receipt for the selected
    branch), `sync_rate_below_floor`, and two sticky-escalator rows.
    **DIAGNOSED 2026-07-27 — two independent wedges, either alone pins
    H\*=0 forever (both confirmed in code AND artifacts):**
    **Wedge A (reducer-side):** the synthetic genesis `block_index` entry
    is inserted with `nVersion=0` (never populated,
    `app/services/src/block_index_loader.c:783-793`), so
    `validate_headers_default_validator` terminal-fails height 0
    (`version-too-low`, `validate_headers_validator.c:479-483`;
    `stage-validate_headers.json`: `failed_total=1 first_failed_height=0`).
    `body_fetch` skips h0, `script_validate` publishes `upstream_failed`,
    and `proof_validate` dead-waits on a receipt that can never exist —
    the `stale_upstream_hash` ×312. Fires even with a perfect network.
    Fix: populate the real genesis header fields (the real ZCL genesis IS
    v4 — the live node.db `blocks` row confirms version=4 at h=0), or
    exempt the genesis hash from the version gate (mirroring existing
    exemptions in `check_block.c:301`, `accept_block_header.c:221,319`).
    **Wedge B (network-side, hits every snapshot-seeded SERVER):** the
    hydrated in-memory index carries no Equihash `nSolution`
    (`block_index_blocks_hydrate.c:441-478`), so
    `headers_index_header_servable` (`lib/net/src/msg_headers.c:304-342`)
    builds a header with `nSolutionSize=0` → `invalid-solution` → refuse;
    the node.db `blocks` row HAS the full 1344-byte solution but the serve
    path never consults it. Confirmed in the LIVE peer's own log at
    08:03:26Z: `getheaders: refusing to serve header 0004b371… h=1
    reason=invalid-solution`. Companions: the successor walk re-queries
    the same failed h=1 entry 64× → 0-header reply
    (`msg_headers.c:344-365`), and `msg_headers.c:334-335` sets
    `BLOCK_FAILED_VALID` for what is a data-AVAILABILITY failure (watch
    the persisted-FAILED reconcile on the live peer's next flat-cache
    save). Fix: node.db-blocks fallback in the serve path (pattern:
    `header_from_node_db_block`), no FAILED_VALID for availability, and a
    successor walk that actually advances. The getheaders LOCATOR was NOT
    the bug — `peer_start_h=3195667` is only the peer's advertised
    starting_height for interval math; the wire locator was genesis
    (`msg_headers.c:1213`). The `catchup: final commit missing tip hash`
    loop is a downstream symptom (zero indexed blocks → NULL
    last_indexed_tip), not a cause. **Measurement trap that
    cost one wrong read:** `zclassic23 dumpstate` IGNORES the
    `ZCL_DATADIR`/`ZCL_RPCPORT` env vars — a probe aimed at the stopwatch
    node silently read the live node instead (reported H\*=tip, the live
    node's value). Aim dumpstate probes with explicit flags, or read the
    harness's own samples. The working form (verified against the live
    node): `zclassic23 -datadir=DIR -rpcport=PORT dumpstate <sub>` — the
    RPC cookie is per-datadir, so BOTH flags are needed; env vars only
    affect the separate `zcl-rpc` binary.
    **RESOLVED 2026-07-27 — re-run PASS, 402s wiped-datadir-to-tip.**
    Same harness, same machine, peer=`zclassicd` behind the harness relay
    (`tools/scripts/c3_stopwatch_run_and_record.sh --peer=127.0.0.1:8034
    --budget=1800`, run dir `build/c3-stopwatch/20260727T102606Z-2420093/`,
    verdict pass, exit 0): wiped datadir reached network_tip=3195777 in
    402s wall across 2 boots (one self-respawn by the tip watchdog),
    provable sample to tip, readback clean. Path taken: bundle candidate
    discovered from the peer (h=3056758), installed, projections folded,
    then ~139k bodies ingested from zclassicd at ~800 blk/s. Wedge A
    (genesis exemption) + Wedge B (getheaders node.db fallback) are what
    moved this from STALLED-NAMED h=0 to PASS. Residual observed, not
    blocking: near-tip the node adopted a short fork header branch,
    briefly flipped at_tip→connecting, got 35 `notfound` replies for
    fork-only hashes (neither byte order exists in the live node's
    `blocks` table — verified via `core storage query`), and recovered
    without operator input; one `no-header-solution-backfill-required`
    on-demand validate refusal at the bundle checkpoint h=3056758 fired
    during boot 1 and cleared on the watchdog respawn.
11. **DONE 2026-07-27 — stale `.failed` bundle markers now cleared on
    successful install.** Found in the canonical datadir: a watchdog-killed
    boot (the item-8 crash-loop morning) marked
    `bundles/consensus-state-bundle-3056758.sqlite.failed` at 04:28; a later
    boot installed the bundle successfully (05:37), but nothing cleared the
    marker — and `boot_autodetect_consensus_bundle` skips marked bundles by
    design, so every future autodetect scan (e.g. on a copy-prove datadir
    copy, the recovery doctrine) would silently ignore the GOOD bundle. Fix:
    `boot_auto_install_clear_failed_marker()` (new, exported per the
    `boot_post_install_fold_span_check` testability precedent,
    `config/src/boot_auto_install_bundle.c`) called at both install-success
    branches (autodetect + install-on-next-boot request). Regression test:
    `case_failed_marker_cleared_on_success` in
    `test_consensus_state_install_runtime.c` (marked → skipped, cleared →
    detectable, idempotent/NULL-safe). The live datadir's stale marker can
    be removed by hand at the next owner-gated maintenance window
    (`rm ~/.zclassic-c23/bundles/*.failed` — the bundle beside it is the
    good 05:37 install).
12. **`catalog.op_return_index.lag_exceeded` on the live node is the item-9
    structural class wearing a misleading name.** The blocker says "cursor
    frozen at -1 … backfill service is stalled and must resume" (87 fires,
    age ~2.8h), but the mechanism is the same snapshot-seed floor as
    `address_index.below_snapshot_seed` / `txindex.below_snapshot_seed`:
    `op_return_backfill_run_once` (`app/services/src/
    op_return_backfill_service.c:123`) walks from cursor+1=0 and dies on the
    first height — `active_chain_at(0)` has no `BLOCK_HAVE_DATA` because
    bodies below 3195552 were never downloaded. It can NEVER resume on this
    datadir. Two honest options, not yet picked: (a) teach the backfill to
    start at `reducer_trusted_base_height` on seeded datadirs — but the
    index folds a running digest from genesis, so a mid-chain start
    diverges from any from-genesis peer's digest (the fold is
    order-committed; see the oversize-block comment at
    op_return_backfill_service.c:150-155) — the digest contract must be
    amended deliberately, not hacked; (b) give op_return_index the same
    `below_snapshot_seed` typed blocker the other two indexes have, so the
    live blocker board stops claiming a resumable stall. Until the
    sovereign cure lands, (b) is the truth-preserving move.

Standing pattern behind items 1, 5, and much of the above: **when one fact has
two writable copies, fix it by deleting a copy, never by adding a
reconciliation guard.** Five instances were found this session; four were real
and were collapsed. The fifth (`zslp_balances`) looks exactly like the disease
and is not — it is load-bearing, and `docs/AGENT_TRAPS.md` now says so. Read
that entry before deleting anything that looks obviously dead.

## Where the developer loop actually spends its time

Measured, recorded in [`BENCHMARKS_LOG.md`](BENCHMARKS_LOG.md). Read that before
optimising anything here — two confident hypotheses died against these numbers.

- The bare link over 1883 objects is **0.90s** (`ld.bfd`) / **0.58s** (`ld.gold`).
  It is not the bottleneck, despite `ZCL_DEV_LINKER` resolving to empty on this
  host and the gating lane never referencing it. Both true, both worth ~0.3s.
- ~~The one-file rebuild cost is the compile-epoch churn~~ **FIXED 2026-07-27**
  (item 2 above): the epoch no longer re-keys on source edits, so one edit
  recompiles only the stale TUs. The remaining per-edit wall floor is the
  parse-time source-identity capture + session acquire (~6s), not compilation.
- The remaining large lever is the ~36% of test objects that a node-header edit
  used to dirty; the header split took a real edit from 680 recompiles to 9.

## MVP status

MRS and per-criterion evidence live in [`docs/MVP.md`](MVP.md) and
`zclassic23 milestone` (REST `GET /api/v1/milestone`). Only a run-passing
`make mvp-verify` member moves a ◐ to a ✅ — never hand-bump the count.

## Operational notes

- `-import-complete-shielded` requires the source chainstate's best block to
  equal exactly the target coins-island root — a bind guard, not a bug.
- A `-bootstrapserve` zclassicd pins its on-disk chainstate at the serve
  anchor.
- `chainstate_legacy_reader` reads LevelDB SSTs only; it does not replay a
  non-empty WAL. A non-empty WAL must refuse loudly rather than silently
  drop data.
- `zclassicd invalidateblock` does not persist across restarts in this fork.

## Lanes

| Lane | Datadir | Deploy | Purpose |
|---|---|---|---|
| **live** | `$HOME/.zclassic-c23` | `make deploy` (owner-gated) | Public daily-driver node; restart only for a vetted live deploy. |
| **dev** | `$HOME/.zclassic-c23-dev` | verify/probe only | Isolated build/test lane; public tooling cannot restart or publish to it. |
| **soak** | `$HOME/.zclassic-c23-soak` | deliberate re-baseline | Long-uptime / weekly evidence lane; do not churn during development. |

`zclassic23 agentlanes` / REST `/api/v1/agent` report each lane's
`operator_lane` (`zcl.operator_lane.v1`) and restart policy; prefer that
contract over parsing systemd names. The units declare the same intent with
`-operator-lane=canonical|dev|soak`. `zclassicd` (the C++ reference) runs
co-located — never stop it.

`make deploy-dev`, `make deploy-dev-fast`, and `make agent-deploy-fast` are
Phase-0 contained: every public invocation refuses before service, datadir, or
generation mutation. Build, source verification, simulation, and hermetic
fixture probes stay available.

`make lane-health` is the read-only three-lane status check. It reports systemd
state, RPC reachability, listener state, height, lag from the live lane, peer
count, restart count, memory pressure, role readiness, soak-evidence
eligibility, any `-reindex-chainstate` flag, and the binary-owned
`bootstrapstatus.snapshot_loader` posture (snapshot seed height, active loader
path, `recovery_hint`). `role_ready` answers whether a lane serves its assigned
purpose; the dev lane is not role-ready when its lag exceeds the lane
threshold, even with RPC and listeners up. `soak_eligible=false` means the soak
lane is alive but not earning clean MVP-C6 evidence. It is an observability
check, not automatic failover.

`make lane-recover LANE=dev|soak` is a read-only bounded recovery planner
emitting `zcl.lane_recovery_plan.v1`. Public `--apply` and
`ZCL_LANE_RECOVERY_APPLY=1` refuse before any mutation; `live`, `canonical`,
and `main` are refused outright. Use it after `make lane-health` reports a
recovery hint — it is a planner, never an apply path.

Copy-prove every recovery path on a datadir COPY before live; never live
surgery. Gate on **H\* CLIMB**, never "booted without FATAL." Never weaken a
safety/operator gate. Gate every change with
`tools/scripts/gate-and-report.sh <lintlog> <testlog>` (lint → full link build
→ `test-parallel`, keyed on the `SUITE VERDICT … groups_ran=N groups_failed=N`
line and rejecting a cached run) — not the pipe exit, and not a bare
`grep "ALL TESTS PASSED"`, which also matches `ALL TESTS PASSED (CACHED)`.
Replay any consensus-predicate tightening against real chain history first —
see `docs/CONSENSUS_PARITY_DOCTRINE.md`.

## Pointers

- [`docs/work/FORWARD_PLAN.md`](work/FORWARD_PLAN.md) — the ordered plan.
- [`docs/work/self-verified-tip-plan.md`](work/self-verified-tip-plan.md) — the sovereign-cure design.
- [`docs/work/sovereign-cutover-runbook.md`](work/sovereign-cutover-runbook.md) — owner-gated cutover + revert.
- [`docs/AGENT_TRAPS.md`](AGENT_TRAPS.md) — read before "fixing" anything.
- [`docs/MVP.md`](MVP.md) — the v1 acceptance bar (8 criteria).
- [`docs/FRAMEWORK.md`](FRAMEWORK.md) — canonical architecture reference (§9 is the open-item debt board), off the v1 path.

A map, not the territory: trust the code you read this minute over this
file.
