# Being a ZClassic23 developer

This is the developer operating manual: onboarding, the fast dev loop, where
things live, the inviolable rules, and build/test/deploy. It is also the body of
the `zclassic23-dev` Claude Code skill, which is a short stub at
[`.claude/skills/zclassic23-dev/SKILL.md`](../.claude/skills/zclassic23-dev/SKILL.md)
that imports this file — this page is the single copy, edit it here.

ZClassic23 is one self-contained C23 binary that is a full ZClassic node
(Equihash 200,9 PoW, Sapling shielded txs) plus wallet, explorer, embedded Tor,
and more (`tools/scripts/binary_size.sh` prints the current size; don't hand-pin
one). Its native command registry is the sole agent interface. It must stay
**bit-for-bit consensus-compatible with `zclassicd`**.

The codebase looks big; the idea underneath is small. This page is the compressed
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
- `zclassic23 code find` / `code file` / `code group` → text search / a file's symbol surface / a directory's surface. `code find` takes `{"text":"<needle>","limit":<n>}` (no `name`/`query` key). Run `zclassic23 discover schema <leaf>` for the exact input keys of the others.
- `zclassic23 code capsule --input='{"name":"<symbol>"}'` → one bounded document composing identity (decl/def, linkage-aware id, signature, group) + direct callers/callees + in-tree includes + the command paths whose handler lives in that file — what `code sym`+`code refs`+`code file` would otherwise take four calls to assemble (the semantic-impact closure in one shot). Self-shrinking under budget (drops includes, then callees, then callers — never identity/def/route) and names what it cut in `dropped_sections`, so it's honest about what didn't fit rather than silently truncating.

When the navigator can't answer and you fall back to a raw-text search, scope it with **`git grep`** (or `git ls-files | xargs grep`) — never `grep -r` / `find .` from the repo root. `.claude/worktrees/` (every lane's full checkout) and `test-tmp/` (per-run test scratch) live under the repo root and are gitignored/untracked, so an unscoped recursive scan walks tens of GB of duplicate/scratch content instead of the tracked source tree.

The index is derived and read-only. The current efficient loop is **`code sym`/`code refs` to understand (cheap) → `Edit` the `.c` → the resident watcher classifies it → `dev status` to confirm.** In `mode=auto`, exactly one allowlisted stateless island is compiled, probed, and published into the isolated dev node; every other edit is forced onto the contained verify-only proof path.

**Editor/agent LSP (clangd):** `make compdb` regenerates the gitignored root `compile_commands.json` from the exact dev-object recipes (~25 s). The root `.clangd` adds the C23 fallback flags for out-of-database files (`src/cli.c`, `examples/`, fuzz, `vendor/`), suppresses GCC-only `-Wno-*` noise, and keeps the background index on disk (`.cache/clangd/`). It complements the navigator — navigator for cheap exact symbol/refs queries, clangd for editor hover/rename/diagnostics. Sanity-check one TU with `clangd --check=<file> --compile-commands-dir=.`.

## Develop fast — the native dev loop (this is how you stay efficient)

The platform exists so you **drop in C and let the machine classify, build, and test it.** Do not hand-run every step or drop to bash to inspect — that is the slow path the platform was built to remove.

1. **Persistent watcher (default-safe, hot when requested):** `zclassic23-dev dev loop ensure` (or `make dev-watch`) starts verify mode. For the interpreter-like inner loop on the isolated armed dev node, use `zclassic23-dev dev loop ensure --input='{"mode":"auto"}'` once. Then just **Edit `.c`**. One allowlisted stateless island takes the resident compile→link→probe→atomic publish path; everything else is downgraded to verify-only. Read the machine receipt with `zclassic23-dev dev status` (`dev.status`) or block on `dev loop wait`.
2. **Hot-swap tiers for the fast inner loop — three of them, pick by surface:**
   - **Swappable read-only island (sub-250 ms measured loop):** owners are enumerated in `config/hotswap_swappable.def`; additional stateless controller/view/service/codec/metaverse TUs are attached in `config/hotswap_islands.def`. With the auto watcher running, edit any island member. The persistent authority reuses its native action plan, invokes stock GCC/Clang directly, links one `-Bsymbolic` module, probes the candidate against the resident node, and atomically replaces the owner leaf set. It does not start Make, a shell, or a throwaway command process. `tools/dev/hotswap-resident-bench.sh` is the 20-distinct-artifact p95 gate. Manual `make hotswap-try`, `make hotswap-apply`, and `dev hotswap probe` remain useful diagnosis surfaces. See `docs/work/HOTSWAP.md`.
   - **Everything else — two verification tiers, know which proof you need:** eligible stateless handler changes may build a candidate shared object and run `dev.hotswap.probe` without changing the resident registry. All other changes use the mapped compile/test proof lane and produce a reload candidate only. Neither tier publishes or restarts a process.

   The Make/dev-loop control plane captures one exact source record and reuses it across nested Makes. Exact single-profile goals load only their depfiles; mixed, unknown, and default goals load every profile. Use `make ff`, `make t-fast ONLY=<group>`, and `make fast-compile` for iteration, then run the strict gates below. Full-suite success is summary-only; focused runs and failures retain diagnostics, and `--verbose` requests the transcript. **Never fabricate or manually pass `BUILD_SOURCE_RECORD` / `ZCL_FAST_BUILD_SOURCE_RECORD`**—the parent Make or watcher owns capture, and every artifact session verifies it.

   The watcher coalesces only an exact, deterministic compiler diagnostic. Source bytes, ABA mutation token, execution/toolchain epoch, flags, and phase must all match; any change forces execution. Tests, lint, timeouts, signals, locks, infrastructure failures, and malformed receipts always execute. The current cycle verdict's `failure_id` is authoritative; `dev.diagnose.latest` is only the most recently recorded compiler failure and can be stale after an edit or green cycle. Inspect the returned ID with `zclassic23-dev dev diagnose show <failure_id>`; use `--view=full` only for the bounded capsule. `zclassic23-dev dev ff` deliberately reruns the current checkout without coalescing—it is not historical replay. Cycle and failure state are worktree-scoped and SHA3-sealed. Never edit or delete their files to influence a verdict.
3. **Typed commands over bash — always.** `zclassic23 status` (compact status), `ops state --subsystem=<name>`, `ops logs`, `core storage query`, `discover help|search <q>`, `dev status` — instead of `ss`/`ps`/`tail`/`grep`. **Every reach for bash to inspect the node is a missing typed command — add it.** The registry is the only agent interface.
4. **Big refactor/test campaigns → workflows of tiered subagents.** Author a `Workflow` (Opus for hard lanes, Sonnet for scoped, to save tokens); each lane runs in an isolated worktree (`isolation:'worktree'`), self-gates (build + focused test + `make lint`), and commits its green work to a `wf/<name>` branch. You then merge the green branches to main and push. Orchestrate + review; the fleet does the volume.

`dev test run` binds a focused proof to one source epoch without rehashing every
source byte on the unchanged fast path. Dev/test runners carry the source ID
plus their publication-verified build mutation receipt (the host-local receipt
is never baked into the reproducible release binary); the command pins the
runner inode and admits it with the current inventory/ABA CAS. If metadata moved
because the checkout was copied or an edit was reverted, it falls back to one
full byte capture and still reuses an exact-content runner. The post-proof CAS
binds nanosecond file/directory/index epochs and effective Git exclude policy.
Edit/revert ABA during the proof, newly appearing compiler inputs, and policy
changes all refuse. Build and publication boundaries still use the heavier full
`verify-record`.
The resident dev surface also emits `source_cas_sha3` over the public C23
source roots. This is the persistent native Merkle identity used to measure and
develop the build fabric: `source_cas_work` reports files read, nodes rehashed,
and elapsed microseconds. It is deliberately marked `source_cas_authority:
shadow`; the exact shell-derived SHA-256 source/mutation record remains build
and publication authority during differential rollout.
5. **Push flow + its two traps:** `make lint && make -j"$(nproc)" build-only`, run the mapped focused tests with parallel Make, then `git push` (hook runs `make pre-push-ci`). **Trap A (impact-rules):** every changed `.c` must map to a focused group in `app/controllers/include/controllers/agent_impact_rules.def` or the push is BLOCKED ("no focused test mapping") — add the mapping. **Trap B (pre-push SIGPIPE):** git may not drain the hook's stdout, so a GREEN `make pre-push-ci` can die with `make[2]: write error: stdout` and spuriously block — confirm green out-of-band (`make pre-push-ci >log 2>&1; echo $?` → 0) then `git push --no-verify` (verified, not skipped).
6. **ZVCS:** each green cycle may anchor candidate source/artifact evidence. Source revert is available only with generation relinking disabled. Explicit full-generation publication uses `zclassic23-dev dev generation activate --idempotency-key=<key>` and its returned `commit_input`; automatic relinking remains contained. Sealed-core changes require the owner unseal ritual (`check-core-seal`).

**Full-generation activation:** the narrow auto watcher still publishes only an allowlisted read-only island. An owner may explicitly stage and activate a full isolated-dev generation with the native plan/commit command above. It resolves an immutable source epoch, stages and preflights the exact candidate, compare-and-swaps the expected resident epoch under the activation lock, quiesces and atomically publishes, probes the exact process through the public registry, and accepts or restores the prior generation. Canonical-node and release publication remain separate and contained.

## The model in four lines

1. One durable append-only log of facts on disk (`consensus.db` — the reducer kernel's own SQLite file since the A3/A4 flip; `progress.kv` is a legacy alias that now holds only address_index/txindex projections). `consensus.db` is the only consensus authority.
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
Consensus predicates + params: `core/` — **byte-sealed** by `core/MANIFEST.sha3`
and the `check-core-seal` gate; an edit there fails `make lint` until
`make core-unseal REASON="…"` then `make core-seal` (read
[`core/UNSEAL.md`](../core/UNSEAL.md) first). Pure bounded contexts:
`domain/{encoding,wallet}` (no clock/RNG/IO). Primitives: `lib/`. Hexagonal write seam:
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

- `make -j"$(nproc)" build-only` — fast parallel compile-check (inner loop). **It compiles
  library objects and does not link** — `src/main.c` and the binaries are never
  built, so it cannot catch a broken entry point, a missing symbol, or a link
  gap. Green here is not green.
- Compile epochs are **toolchain+flags-keyed** (since 2026-07-27): a source
  edit recompiles only the stale TUs (timestamp+depfile) inside the stable
  `build/*/epochs/<epoch>/` dir — a one-line `.c` edit is ~2 compile
  invocations (the TU + the identity TU `clientversion.o`, which always
  rebuilds on a source-identity move via `BUILD_IDENTITY_STAMP`). Only a
  Makefile/flags/toolchain edit re-keys the epoch and rebuilds everything;
  that includes per-object CFLAGS override lines, which ride the
  `BUILD_SYSTEM_ID` Makefile fingerprint.
- `make -j$(nproc)` — full build (`zclassic23`, `test_zcl`, `zclassic-cli`).
- `make -j"$(nproc)" test` / `make -j"$(nproc)" test-parallel` — the canonical test runner. **Use this, not `test_zcl`.**
- `make -j"$(nproc)" t-fast ONLY=<substr>` — one focused run. `ONLY=` is a **substring**
  match, not a group name (`ONLY=wallet` runs 36 groups). Exact names:
  `make test_parallel && build/bin/test_parallel --list` — the underscore
  target is the only one that publishes that alias; the run targets execute an
  epoch candidate under `build/bin/test-strict/epochs/<epoch>/`.
- `make lint` — all gates; must pass before tests. `make ci` — lint + build + tests + checks.
- `make custody-check` — the exact wallet-identity, session-policy, intent,
  broker-money, and receipt regression set. It uses isolated fixtures only and
  never contacts a live wallet or moves funds. After an owner-created broker
  binding exists, the read-only live check is
  `zclassic23 metaverse agent money --dir=<absolute-broker-dir>`; it reports
  `UNKNOWN`, `STALE`, or `CONFLICTED` instead of inventing a zero.
- `make custody-status` — the read-only rollout doctor: source support, current
  dev activation, canonical prod targeting, private broker binding, and the
  complete two-wallet snapshot in one five-step progress line. Add
  `ARGS='--broker-dir=/absolute/path'` after the owner creates the binding.
  For a scoped money operation, add `--wallet-scope=dev|prod`; that reports
  readiness for the explicitly targeted wallet while continuing to show an
  incomplete portfolio as partial. It never promotes the other wallet to zero.
  Raw balance reads are labeled `OBSERVED`, never promoted to identity-bound
  `CURRENT`; endpoints and datadir paths are absent from its output. Its
  hermetic contract check is `make custody-status-selftest`.
- `make transaction-lab-proof` — run the exact isolated transaction evidence
  matrix with real signatures, Sapling proofs, consensus verification, HTLC
  interpretation, and overlay builders. `make transaction-lab-status` prints
  separate proof and live-mainnet bars plus value/fee totals; it never treats
  simulated confirmation as a live spend. The append-only, redacted live
  notebook defaults to private mode-0600 local state and is never committed;
  its recording procedure lives in `docs/work/TRANSACTION_LAB.md`.
  `make transaction-lab-check` validates only the reproducible repository
  baseline, while `make transaction-lab-status` validates the private working
  ledger when one exists.
  The two Sprout proof eras are independently pinned by
  `test_sprout_phgr13_kat` (mainnet height 241) and
  `test_sprout_groth16_kat` (mainnet height 476970); both embed only public
  transaction/VK bytes and require no wallet or live node.
- `make transaction-micro-lab-check` validates the stable 100-slot,
  1,000-zatoshi live-campaign allocation and its redacted append-only receipt
  state-machine template. `make transaction-micro-lab-status` validates the
  private, never-committed working ledger when present and reports confirmed slot and
  type coverage, exact value/fee totals, fee distribution, and confirmation
  latency. Both are evidence-only and cannot plan, sign, authorize, broadcast,
  or touch a datadir. The owner runbook is
  [`TRANSACTION_MICRO_LAB.md`](work/TRANSACTION_MICRO_LAB.md).
  The agent-fast path is the node-free native command
  `zclassic23 app transaction-types micro-lab [--slot=N]`; it joins a numbered
  slot to the semantic transaction catalog and current typed guide input
  without reading wallet state or granting spend authority.
  `make transaction-micro-lab-wallets-setup` creates the two persistent,
  isolated recipient wallets without funding them or printing their addresses;
  `make transaction-micro-lab-wallets-status` is the redacted resumable check.
- Before using or adding a money-shaped native leaf, reverse-audit it with
  `zclassic23 app transaction-types command <path>`. `mapped` names every
  semantic workflow and role; `explicitly_non_chain` carries a reviewed reason;
  `unclassified` is a hard stop, never permission to assume the command is
  off-chain. `test_api` makes new ready wallet-risk/chain-worded mutations fail
  until they have positive catalog coverage or an explicit negative row.
  `test_transaction_wire_evidence` separately pins an exact mainnet v1
  transaction plus canonical P2PK, P2PKH, P2SH, nulldata, and nonstandard
  output examples. It also proves why no mainnet v3 fixture can exist:
  Overwinter and Sapling activate together at height 476969, so v3 is
  premature one height earlier and Sapling-invalid at activation.
- `zclassic23 app transaction-types list` — the compile-time semantic catalog
  of every known transaction shape and its exact builder/commit/inspect path.
  Use `app transaction-types show --type=<id>` for one entry, or
  `app transaction-types guide --type=<id>` to join it to exact live command
  schemas, allowed keys, authority, confirmation, and the safe next decision in
  one read. Use `app transaction-types wire` for the separate finite catalog of
  consensus versions, serialized components, mainnet reachability/evidence,
  script-policy classes, and the explicit open-ended script/memo buckets. REST mirrors the semantic catalog at
  `/api/v1/transaction-types`; the AI-safe workflow and extension checklist are in
  [`TRANSACTION_API.md`](./TRANSACTION_API.md).
- `zclassic23 app payments zpay compose|inspect` — deterministic, public
  adapters for canonical 512-byte ZPAY invoice/payment/receipt memos. Compose
  returns `memo_hex`; the existing owner-only `core.wallet.shielded.send`
  remains the sole value-moving step. Inspect requires an explicit expected
  network and clock. Neither leaf accepts an identity seed.
- `make deploy` is owner-gated live deployment. For the isolated dev lane,
  `zclassic23-dev dev generation activate --idempotency-key=<key>` is the sole
  full-generation authority: its plan returns exact `commit_input`, and its
  commit is source/resident-CAS-bound with exact-process verification and
  rollback. Other dev publication, relink, and recovery-apply entry points
  still hard-refuse; source identities and environment variables alone grant
  no activation authority. Gated leaf hot-swap remains available through
  `hotswap-try`/`hotswap-apply`.
  `make deploy` rm's the stale binary first
  (a stale binary was a real multi-day outage) and verifies `build_commit`.
- **Gate every change with `tools/scripts/gate-and-report.sh <lintlog> <testlog>`**
  — `make lint` → full link build → `make test-parallel`, keyed on the pass
  token rather than a grep match. Running it by hand: the runner prints
  `SUITE VERDICT mode=<cold|cached> … groups_ran=N groups_failed=N` and then one
  of `ALL TESTS PASSED` / `ALL TESTS PASSED (CACHED)` / `SOME TESTS FAILED`.
  There is no `N passed, M failed` line. A bare `grep -q "ALL TESTS PASSED"`
  matches the `(CACHED)` form and green-lights a run that executed **zero**
  groups — that false green already shipped once (see the comment above the
  `SUITE VERDICT` printf in `lib/test/src/test_parallel.c`). Force cold with
  `make -j"$(nproc)" test-parallel TEST_PARALLEL_ARGS=--no-cache`.

## The agent surface — native command registry

The interface is the native registry: `zclassic23 <path>` under seven roots —
`status`, `core.*`, `app.*`, `dev.*`, `ops.*`, `discover.*`, `code.*`. Start with
`zclassic23 status`. Three diagnostic primitives cover most questions:
`ops state --subsystem=<name>` (generic state dump), `ops logs`, and
`core storage query` for SELECT-only SQL. Discover everything with `discover help` /
`discover search <q>` (query is **positional** — the `--input='{"query":…}'`
form its schema advertises returns `MISSING_QUERY`).

**Enumerating the 156 dumpstate subsystems is `zclassic23 statecatalog`**, not
`ops state` with no `--subsystem` — that errors `MISSING_SUBSYSTEM`.
`statecatalog` returns each subsystem's owner `.c` file, accepted key forms,
cost, and owning test path. Add one by appending a `DIAG_*` descriptor row to
`app/controllers/include/controllers/diagnostics_dumpers.def` — full recipe in
[`docs/CODEBASE_MAP.md`](CODEBASE_MAP.md) §3.

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

Main repo orchestrates; `wt2`/`wt3` are workers. Run **`make worktree-prime`** once per fresh
worktree before any other `make` target — it `cp -a`s `vendor/lib/*.a` from the primary checkout
(auto-detected via `git rev-parse --git-common-dir`; override with `SRC=<path>` for a non-primary
source) instead of paying the vendor-bootstrap rule's from-pinned-source rebuild (measured ~57s
on this host for a from-empty `vendor/lib`, vs ~1s to copy). This formalizes what used to be
tribal knowledge (manual `cp -a` vendor static libs from main — fresh worktrees can't link without
the gitignored `vendor/lib/*.a`). Compiled `.o` object caching (`ccache`/`sccache`) is already
cross-worktree for free — see docs/BUILD.md's "already cross-worktree, not just cross-edit" note —
so no extra step is needed there. When fanning out work across lanes, give each lane a disjoint
file set, prove each on its own datadir copy, and merge in dependency order. See
`docs/work/README.md` + `docs/work/agent-protocol.md`.

## The discipline that matters most

The node's whole reason to exist is one property: **derive every fact by folding a replayable log and
check it against its own cryptographic checkpoints — never serve an unproven value, never halt without
naming the exact block + reason.** Every change should make that more true (more self-derived, fewer
borrowed/cached authorities), or it's off-mission. The current work to finish it is
`docs/work/self-verified-tip-plan.md`.

**Verify fresh. The live code is the only authority; this page and every doc it links can be stale.**
