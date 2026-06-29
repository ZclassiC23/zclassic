# Competition action plan — "best zclassic23 node: robust / secure / fast / easy"

Judge installs from a fresh clone, syncs, uses, and tries to break it; competing
against rival AI builds. Source of truth = the live experiments below, not docs.
Plan written 2026-06-25 from: read-only judge audit (`scratchpad/competition-scorecard.md`)
+ two live copy-proves (torn-copy un-wedge STALL, fresh-empty IBD probe).

## What a fresh judge experiences TODAY (measured, full-stack binary `360ed3f76`)

1. `git clone && make` → **FAILS to link** — vendored libs are gitignored, no `make vendor`. **[IL-1]**
2. `./zclassic23` on empty datadir → boots fine (no FATAL), connects to 2 fixed-seed peers.
   - **DNS seeds BOTH DEAD**: `dnsseed.zslp.org` + `mainnet.zclassic.org` fail to resolve.
     Only the 10 hardcoded MagicBean IPs save it. One churn from a silent 0-peer dead-end. **[IL-NEW-B]**
3. `getblockcount` → **3056758 (PHANTOM)** — but `getblockhash 3056758` = "out of range",
   `getblockchaininfo` returns nothing (logs `provable tip hstar=3056758 unresolved`).
   First query lies; an RPC is broken on fresh boot. **[IL-NEW-A]**
4. Sync to tip → **cold genesis fold ≈ days** (reducer cursors in the low hundreds after 90s).
   `-load-snapshot-at-own-height` on an empty datadir **FATALs** ("header tip=0, REFUSING to seed":
   the seed needs headers present first). No shipped bootstrap. Judge never sees "reaches tip". **[IL-2]**

## Wedge (the thing I'd been chasing) — correctly DEPRIORITIZED to #3

Live `~/.zclassic-c23` is wedged at H*=3157645. The full-stack cure binary RESOLVES the
header-source layer (validate_headers climbs to 3160068) but stalls at a DEEPER blocker:
**utxo_apply ok=0 at 3157647, silent (no logged reason)** = torn `coins_kv`. The sticky
escalator runs every rung (retry→targeted_rederive→resnapshot→reindex) but **none re-folds
coins from a clean base**, so it can't clear it. Block 3157647 (`00000e6b9`) is
NETWORK-CANONICAL (whole net at 3160101 applied it) → NOT consensus → **a fresh judge never
inherits this torn state**. Repairing this one torn datadir is the frame error; the CLASS
cure is a "re-fold coins from a header-verified trusted base" terminal rung.

## Winning fresh-judge target

`git clone && make && ./zclassic23` → builds, finds peers, and reaches a usable tip in
ONE sitting (~10–25 min), answers "is my node healthy?" in one command, and survives
kill-9 / corruption unattended.

## Work, sequenced around builds (each item: effort / needs-build?)

### Batch 1 — IN FLIGHT (agent a590458c80e17f05f, branch `sticky/ship-candidate`)
- [IL-1] Merge `sticky/make-vendor` → fresh clone links via `make vendor`. (M / build)
- Merge `sticky/fuzz-http` (parser fuzzing). (S / build)
- [SEC] Cap unbounded onion reassembly DoS (`dynhost_handlers.c`). (S–M / build)
- Verify: build green, `make lint`+E13 green, `test_parallel` green.

### Batch 2 — next build (set up read-only first; pinpoint sites)
- [IL-NEW-A] Phantom H*: `getblockcount`/hstar must NOT return the trusted anchor when it's
  `unresolved` (no data below it) — return the resolved height (0 on fresh) +
  fix `getblockchaininfo` to not blank out. (S–M / build) — highest judge-visibility, cheap.
- [IL-NEW-B] DNS seeds: replace/repair the two dead hostnames; verify the 10 fixed IPs live;
  add ≥1 more onion seed (retire single-point-of-failure). (S / maybe no build if config)
- [IL-2 core] Snapshot loader: **defer-seed-until-headers-present** instead of FATAL, so a
  fresh datadir can: P2P-headers → auto-seed snapshot @anchor → fold remainder. (M / build)
- [ROBUST #3] Add escalator terminal rung: utxo_apply ok=0 on a network-canonical block →
  wipe coins above last-good → re-seed from header-verified trusted snapshot → re-fold.
  This also self-heals the live node on restart + un-BLOCKs the fault-matrix row. (M–L / build)

### No-build, anytime
- [IL-2 ship] Package a bootstrap bundle (block-index/headers + `utxo-seed` snapshot) as a
  release asset + doc the one flag/auto-load → "tip in a sitting". (M)
- [EASY] README quickstart honesty: real build (`make vendor`), 3-line "verify it's running"
  (zcl_status), the bootstrap-to-tip flow, MCP/AI-operator pitch. (S)
- [SEC honesty] Disclose the anchor-membership stub (`coins_view.c:~476`) as a known/pinned
  parity gap per CONSENSUS_PARITY_DOCTRINE. (S)

### Proof / demo (gated on copy-prove, H* climb, never live-first)
- Bootstrap-to-tip end-to-end copy-prove (the killer demo): clone → build → run → tip → healthy.
- `make crash-recovery-demo` 100× kill-9 (harness `tools/crash_recovery_test.c`).
- Build-twice-identical SHA3 reproducibility (now that clone-build is deterministic).

## Guardrails (standing)
Consensus parity INVIOLABLE (E13 + test_consensus_parity; a fork = instant loss).
Copy-prove before live; gate on H* CLIMB, not "booted w/o FATAL". NEVER stop zclassicd.
Commit/push + live deploy are owner-gated. NEVER python.
