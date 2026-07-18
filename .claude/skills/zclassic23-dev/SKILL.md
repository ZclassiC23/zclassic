---
name: zclassic23-dev
description: Use when developing on the ZClassic23 codebase (this repo) — onboarding, understanding the architecture, or making any code change. Covers the verify-only native dev loop, runtime-publication containment, typed-commands-over-bash, workflows of tiered subagents, the push traps, the node's state-machine model, the eight code shapes / where things live, the inviolable rules (consensus parity, copy-prove before live, defensive-coding gates), build/test/deploy, and the don't-re-chase traps. Invoke for "how does this codebase work", "how do I develop efficiently here", "how do I add or change X here", "be a zclassic23 developer", or before editing zclassic23 source.
---

# Being a ZClassic23 developer

ZClassic23 is one ~15 MB C23 binary that is a full ZClassic node (Equihash 200,9 PoW,
Sapling shielded txs) plus wallet, explorer, embedded Tor, and more. Its native
command registry is the sole agent interface. It must stay **bit-for-bit
consensus-compatible with `zclassicd`**.

The codebase looks big; the idea underneath is small. This skill is the compressed
operating manual. The **canonical, verified docs** are the source of truth — read them,
don't trust this page's specifics blindly (code moves; docs rot):

- `docs/HOW_THE_NODE_WORKS.md` — the one-page mental model (read FIRST if it feels complex).
- `docs/CODEBASE_MAP.md` — where things live + "I want to X → go here" + commands.
- `docs/AGENT_TRAPS.md` — things that look broken but are intentional/already-done. Read before "fixing."
- `docs/FRAMEWORK.md` — the Prime Directive, the Ten Laws, the eight shapes (the *why*).
- `docs/HANDOFF.md` — current live state (what's fixed, what's in flight). Read before acting.
- `docs/DEFENSIVE_CODING.md` — the mandatory coding gates.
- `docs/CONSENSUS_PARITY_DOCTRINE.md` — the inviolable parity rule.
- `docs/NATIVE_COMMAND_INTERFACE.md` — the native command registry (the dev + agent interface).

## Understand fast — query the code index, don't grep (this saves the most tokens)

Before you Read a file or `grep`, ask the in-tree **code navigator** (`lib/codeindex/`, served as `zclassic23 code …`). One indexed lookup ≈ 150–200 tokens; grepping then reading a 1,000-line file to learn the same fact is *thousands*. **Navigator-first — `grep`/`Read` is the fallback for what the index can't answer (prose, comments, non-symbol text).**

- `zclassic23 code sym --input='{"name":"<symbol>"}'` → definition `path:line` + full signature + group. Answers "where is X defined?"
- `zclassic23 code refs --input='{"name":"<symbol>"}'` → every call site as `file:line`. Answers "who calls X? / what breaks if I change it?" (e.g. it flags a caller a delete must handle before you've opened a single file).
- `zclassic23 code find` / `code file` / `code group` → text search / a file's symbol surface / a directory's surface. Run `zclassic23 discover schema <leaf>` for the exact input keys.

The index is derived and read-only. The current efficient loop is **`code sym`/`code refs` to understand (cheap) → `Edit` the `.c` → the watcher builds+tests in verify-only mode → `dev status` to confirm → preserve the candidate evidence for review.** Runtime publication is contained; a green verdict does not change a running generation.

**Editor/agent LSP (clangd):** `make compdb` regenerates the gitignored root `compile_commands.json` from the exact dev-object recipes (~25 s). The root `.clangd` adds the C23 fallback flags for out-of-database files (`src/cli.c`, `examples/`, fuzz, `vendor/`), suppresses GCC-only `-Wno-*` noise, and keeps the background index on disk (`.cache/clangd/`). It complements the navigator — navigator for cheap exact symbol/refs queries, clangd for editor hover/rename/diagnostics. Sanity-check one TU with `clangd --check=<file> --compile-commands-dir=.`.

## Develop fast — the native dev loop (this is how you stay efficient)

The platform exists so you **drop in C and let the machine classify, build, and test it.** Do not hand-run every step or drop to bash to inspect — that is the slow path the platform was built to remove.

1. **Persistent watcher (default loop):** `zclassic23-dev dev loop ensure` (or `make dev-watch`) once. Then just **Edit `.c`** — it runs classify→prove→build in verify-only mode and never changes the running generation; read the verdict with `zclassic23-dev dev status` (`dev.status`) or block on `dev loop wait`. Publication watcher modes and direct generation-application commands are containment probes: they refuse before compilation, loader activity, service control, or generation relinking.
2. **Hot-swap tiers for the fast inner loop — three of them, pick by surface:**
   - **Swappable read-only leaf (the seconds-scale loop):** the six leaves on `config/hotswap_swappable.def` (`core.status`, `core.network.peers.incidents`, `ops.metrics`, `core.wallet.address.list`, `core.consensus.utxo.audit`, `app.names.list`). Edit the owning TU, then `make hotswap-try HANDLER=<leaf> ARGS="<command>"` — builds a single-TU module `.so` and runs the command with `ZCL_HOTSWAP_PRELOAD`, so the freshly compiled body renders live dev-lane data in one shot, no restart. `make hotswap-apply HANDLER=<leaf>` instead commits the override in the RUNNING dev node (armed with `-hotswap-activate` + `ZCL_HOTSWAP_ACTIVATE=1` on `zcl23-dev.service`); `dev hotswap probe` verifies a module without committing. Limits: modules link `-Wl,-Bsymbolic`, so edits confined to the TU just work; an edit that calls a function the resident binary does NOT export fails dlopen with a typed error — refresh the dev node (`make fast-rebuild`, then a MANUAL generation flip: `cp`/`ln -sfn` the new generation under `~/.local/lib/zclassic23-dev/` and `systemctl --user restart zcl23-dev`; there is no scripted flip path) and retry. See `docs/work/HOTSWAP.md`.
   - **Everything else — two verification tiers, know which proof you need:** eligible stateless handler changes may build a candidate shared object and run `dev.hotswap.probe` without changing the resident registry. All other changes use the mapped compile/test proof lane and produce a reload candidate only. Neither tier publishes or restarts a process.

   The Make/dev-loop control plane captures one exact source record and reuses it across nested Makes. Exact single-profile goals load only their depfiles; mixed, unknown, and default goals load every profile. Use `make ff`, `make t-fast ONLY=<group>`, and `make fast-compile` for iteration, then run the strict gates below. Full-suite success is summary-only; focused runs and failures retain diagnostics, and `--verbose` requests the transcript. **Never fabricate or manually pass `BUILD_SOURCE_RECORD` / `ZCL_FAST_BUILD_SOURCE_RECORD`**—the parent Make or watcher owns capture, and every artifact session verifies it.

   The watcher coalesces only an exact, deterministic compiler diagnostic. Source bytes, ABA mutation token, execution/toolchain epoch, flags, and phase must all match; any change forces execution. Tests, lint, timeouts, signals, locks, infrastructure failures, and malformed receipts always execute. The current cycle verdict's `failure_id` is authoritative; `dev.diagnose.latest` is only the most recently recorded compiler failure and can be stale after an edit or green cycle. Inspect the returned ID with `zclassic23-dev dev diagnose show <failure_id>`; use `--view=full` only for the bounded capsule. `zclassic23-dev dev ff` deliberately reruns the current checkout without coalescing—it is not historical replay. Cycle and failure state are worktree-scoped and SHA3-sealed. Never edit or delete their files to influence a verdict.
3. **Typed commands over bash — always.** `zclassic23 status` (compact status), `ops state --subsystem=<name>`, `ops logs`, `core storage query`, `discover help|search <q>`, `dev status` — instead of `ss`/`ps`/`tail`/`grep`. **Every reach for bash to inspect the node is a missing typed command — add it.** The registry is the only agent interface.
4. **Big refactor/test campaigns → workflows of tiered subagents.** Author a `Workflow` (Opus for hard lanes, Sonnet for scoped, to save tokens); each lane runs in an isolated worktree (`isolation:'worktree'`), self-gates (build + focused test + `make lint`), and commits its green work to a `wf/<name>` branch. You then merge the green branches to main and push. Orchestrate + review; the fleet does the volume.
5. **Push flow + its two traps:** `make lint && make build-only`, run the mapped focused tests, then `git push` (hook runs `make pre-push-ci`). **Trap A (impact-rules):** every changed `.c` must map to a focused group in `app/controllers/include/controllers/agent_impact_rules.def` or the push is BLOCKED ("no focused test mapping") — add the mapping. **Trap B (pre-push SIGPIPE):** git may not drain the hook's stdout, so a GREEN `make pre-push-ci` can die with `make[2]: write error: stdout` and spuriously block — confirm green out-of-band (`make pre-push-ci >log 2>&1; echo $?` → 0) then `git push --no-verify` (verified, not skipped).
6. **ZVCS:** each green cycle may anchor candidate source/artifact evidence. Source revert is available only with generation relinking disabled; relinking remains contained. Sealed-core changes require the owner unseal ritual (`check-core-seal`).

**Phase 3 reopening gate (future, not current authority):** publication may return only as one durable transaction that resolves an immutable source epoch, derives the complete dependency/proof plan, enforces signed seal authority, records proof receipts, validates and behaviorally probes the candidate, compare-and-swaps the expected resident epoch, persists prepared provenance, quiesces and atomically publishes the complete generation, probes through the public registry, and then durably accepts or restores the exact prior generation. Until every step and rollback proof exists, verify-only is the platform contract.

## The model in four lines

1. One durable append-only log of facts on disk (`progress.kv`). It is the only consensus authority.
2. One kind of worker — a **reducer stage**. Each reads the height its upstream finished, then
   **advances its cursor by one (one log row) OR names a typed blocker**. Eight stages, fixed line:
   `header_admit → validate_headers → body_fetch → body_persist → script_validate → proof_validate → utxo_apply → tip_finalize`.
3. Everything else (wallet, explorer, peers, UTXO set) is a **projection** — a read-only view folded
   from the log, rebuildable, never authoritative.
4. Health is one number: `network_tip − log_head`. A stall is always a named blocker at a known
   height — a silent halt is unrepresentable. `getblockcount` serves `H*` (the provable tip).

## Where things live — the eight shapes

Every `.c` under `app/` is exactly one shape (lint-enforced). Open the folder, know the shape:
`controllers/` (parse→authorize→call one service), `services/` (orchestrate, return `zcl_result`),
`models/` (the only readers/writers of state; AR lifecycle), `jobs/` (the reducer stages,
cursor-stamped, advance-or-block), `supervisors/` (liveness trees), `conditions/`
(`{detect,remedy,witness}` healers), `events/` (reserved-empty), `views/` (explorer templates).
Pure consensus core: `domain/` (no clock/RNG/IO). Primitives: `lib/`. Hexagonal write seam:
`ports/` + `adapters/`. Boot: `config/src/`. Command tooling and lint: `tools/`.
Full map + "how to add a model / healer / native command / reducer stage /
lint gate" is in `docs/CODEBASE_MAP.md`.

## The inviolable rules (violating these causes real damage)

1. **Consensus parity is absolute.** Never ship a consensus change (Equihash params, activation
   heights, block/tx validity) to zclassic23 first — not even opt-in. Enforced by `check-consensus-parity`
   (E13) + `test_consensus_parity`. **Validate against the real CHAIN, not the zclassicd source text**
   (the chain contains a 125,811-byte tx at h=478544 the text-copied cap would false-reject). Any
   tightening of a bounded predicate requires a full-history replay first.
2. **Copy-prove before live; never live surgery.** Copy the datadir, reproduce on the copy, prove the
   fix FIRES on the copy, then deploy. **Gate on H\* CLIMB**, not "booted without FATAL." `test_parallel`
   green is a regression floor, not a liveness proof.
3. **Every write goes through the AR lifecycle** (`AR_BEGIN_SAVE`/`AR_FINISH_SAVE` or `AR_ADHOC_SAVE`).
   Raw `sqlite3_step()` in app code is lint-rejected. **Every malloc** uses `zcl_malloc(size,"label")`.
   **Every error return logs context** (`LOG_FAIL`/`LOG_ERR`/`LOG_NULL`). **Every native command
   handler sets an error body** — never a bare `return -1`. `make lint` enforces these.
4. **Less is more.** Prefer deleting/unifying over adding. A new abstraction is a last resort.
5. **Profile-first for performance.** No unmeasured perf claims. Don't optimize cold paths. Use
   `zclassic23 ops debug profile` / `zclassic23 core mining benchmark` / the measured bottleneck docs.
6. **Status reporting is plain and technical** — exact height/table/function/file:line. No metaphor.

## Before you change anything

1. Detect your worktree: `pwd` (`main` = orchestrator; `~/github/zclassic23-2` = wt2; `~/github/zclassic23-3` = wt3).
2. Read `docs/HANDOFF.md` (live state) and skim `docs/AGENT_TRAPS.md` (don't re-chase a fixed thing or
   re-propose a shipped optimization or "fix" an intentional parity decision).
3. Check the live node before trusting any doc: `zclassic23 status`, then
   `zclassic23 dumpstate reducer_frontier`.
   A doc can be stale; the node cannot.

## Build / test / deploy

- `make build-only` — fast parallel compile-check (inner loop).
- `make -j$(nproc)` — full build (`zclassic23`, `test_zcl`, `zclassic-cli`).
- `make test` / `make test-parallel` — the canonical test runner. **Use this, not `test_zcl`.**
- `make lint` — all gates; must pass before tests. `make ci` — lint + build + tests + checks.
- `make deploy` is owner-gated live deployment. All public dev-lane publication,
  stage, relink, and recovery-apply paths currently hard-refuse — the gated
  swappable-leaf hot-swap loop above (`hotswap-try`/`hotswap-apply`) is the one
  live exception; source identities and environment variables grant no
  activation authority.
  `make deploy` rm's the stale binary first
  (a stale binary was a real multi-day outage) and verifies `build_commit`.
- Gate every change: `make` + `make lint` + `make test-parallel` (read the `N passed, M failed` line).

## The agent surface — native command registry

The interface is the native registry: `zclassic23 <path>` under `core.*` / `app.*` / `ops.*` / `dev.*` /
`discover.*`. Start with `zclassic23 status`. Three diagnostic primitives cover most questions:
`ops state --subsystem=<name>` (generic state dump — ~56 subsystems incl. the 8 stage names + `blocker`,
`reducer_frontier`, `condition_engine`), `ops logs`, and `core storage query` for SELECT-only SQL. Discover everything with
`discover help` / `discover search <q>`. Add introspection by registering one `*_dump_state_json` in
`app/controllers/src/diagnostics_registry.c` — no new command needed.

Postmortem fast path: `zclassic23 ops debug bundle` writes ONE JSON (every dumper + build identity +
supervisor stalls) to `<datadir>/debug-bundle-<utc>.json` — also auto-written, rate-limited, when the
supervisor detects a stall. Don't scroll 100+KB of JSON:
`python3 tools/scripts/debug_bundle_triage.py <datadir|bundle.json>` prints the one-screen triage
(H*/floor/gap, coins vs H*, top blockers + repair owners, stalled supervisor children, likely story).

## Hosting & recovering the clearnet block explorer

The node **is its own HTTPS server** (`lib/net/src/https_server.c`) — no nginx/proxy.
Full runbook + troubleshooting: `docs/BLOCK_EXPLORER_HOSTING.md`. The two things that
silently take a public explorer (e.g. `https://zclnet.net/`) down:

1. **No TLS cert in the active datadir → onion-only.** The node binds clearnet 8443 only
   if `<datadir>/ssl/fullchain.pem` + `privkey.pem` exist at boot, else logs
   `HTTPS: no cert … not on clearnet`. The certbot deploy-hook
   (`/etc/letsencrypt/renewal-hooks/deploy/zclassic23-explorer.sh`) refreshes them **only
   on renewal**, so a datadir rebuild/re-seed between renewals drops the cert until restored.
   Diagnose: `zclassic23 dumpstate explorer` (one call: https_started, cert_present, onion);
   or by hand `ss -ltn | grep 8443` + `grep -aE 'HTTPS|cert' <datadir>/node.log`. Public 443
   reaches 8443 via the capped linger forwarder `~/.local/bin/zcl-portfwd` (one-time
   `setcap`, managed with `systemctl --user`, never sudo). Recover (no sudo): copy a valid
   cert into `<datadir>/ssl/` (verify the pair with a **pubkey** compare — LE keys are ECDSA,
   `openssl rsa -modulus` fails: `diff <(openssl x509 -in f.pem -noout -pubkey) <(openssl pkey
   -in k.pem -pubout)`), then `systemctl --user restart zclassic23`. HTTPS defers during
   IBD/refold and auto-starts near tip, so on a snapshot-loader node the site returns a few
   minutes after restart, not instantly.
2. **Snapshot-loader node shows empty token/history pages.** `/explorer/tokens` blank,
   `zslp_listtokens` → `[]`, only 1-2 `blk*.dat` files = the node loaded a UTXO snapshot
   (`-load-snapshot-at-own-height`) and never folded historical bodies, so body-derived
   projections (ZSLP tokens, tx/address history, ZNAM below the seed) are empty. Correct
   by design, not a bug. A **public explorer must run full-history** (P2P from genesis or
   two-step `--importblockindex` of a `zclassicd` archive); the snapshot loader is for
   fast/robust consensus+wallet nodes. See `docs/BLOCK_EXPLORER_HOSTING.md` §E.

The live node is the detached `~/.local/bin/zclassic23-live` binary, but `systemctl --user
restart zclassic23` correctly relaunches it (a `stopgap-loader.conf` drop-in overrides ExecStart).

## Parallel-worktree workflow

Main repo orchestrates; `wt2`/`wt3` are workers (`cp -a` vendor static libs from main first — fresh
worktrees can't link without the gitignored `vendor/lib/*.a`). When fanning out work across lanes, give
each lane a disjoint file set, prove each on its own datadir copy, and merge in dependency order. See
`docs/work/README.md` + `docs/work/agent-protocol.md`.

## The discipline that matters most

The node's whole reason to exist is one property: **derive every fact by folding a replayable log and
check it against its own cryptographic checkpoints — never serve an unproven value, never halt without
naming the exact block + reason.** Every change should make that more true (more self-derived, fewer
borrowed/cached authorities), or it's off-mission. The current work to finish it is
`docs/work/self-verified-tip-plan.md`.

**Verify fresh. The live code is the only authority; this skill and every doc it links can be stale.**
