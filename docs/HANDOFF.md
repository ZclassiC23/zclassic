# HANDOFF — read this first

**Restart command:** type **`continue zclassic23 development`**.

Verify HEAD with `git status --short --branch`. Detect your worktree with `pwd`
(`main` = orchestrator; `wt2`/`wt3` = workers — see `docs/work/README.md`).

---

## Current state (2026-06-16)

**The live node is HEALTHY and at the chain tip.** After a multi-day stale-binary
+ torn-datadir outage, it was recovered: the hardening branch merged to `main`,
the datadir rebuilt, and the node is following new blocks with a healthy peer
set, zero crashes.

**What landed:**
- **Hardening** (`docs/work/rock-solid-program-2026-06-16.md`): byte-exact
  zclassicd coinbase encoding; cold-import restart-fragility fix (OPTION 1);
  `chain_restore_finalize` SEGV fix (cs_main serialization);
  `replay_canary` cross-build-staleness + `contradiction_frozen` self-clear.
- **Stability improvements + cleanup** (`docs/work/stability-improvements-2026-06-16.md`):
  the deploy stale-binary footgun (Makefile), DNS-seed + cold-import-seed
  loudness, dead-code/cruft purge.

**The honest open weakness — cold-import bootstrap is slow + fragile.** A fresh
cold import leaves a ~12k-block header-band gap that backfills slowly over P2P
*even though the bodies are already on local disk*, and the first boot can latch
a transient freeze that needs a restart. The cure is tracked as **C1 (local
band-fill) + C2 (band-aware freeze)** in the improvements doc; the strategic fix
is **O2 (make trustless FlyClient + snapshot sync the flagship, demote
cold-import to opt-in)**. Until then, the fast recovery path is to copy a
known-healthy datadir onto the target lane.

**The v1 contract is `docs/MVP.md`; the plan is `docs/work/FORWARD_PLAN.md`.**
Architecture reference: `docs/FRAMEWORK.md` + `docs/REFACTOR_STATUS.md`.

---

## Service lanes

| Lane | Binary | Datadir | P2P/RPC | Deploy |
|---|---|---|---|---|
| **live** | `~/zclassic23/build/bin/zclassic23` (symlink → repo) | `~/.zclassic-c23` | 8023 / 18232 | `make deploy` (owner-gated) |
| **dev** | `~/.local/bin/zclassic23-dev` | `~/.zclassic-c23-dev` | 8053 / 18252 | `make deploy-dev` |
| **soak** | `~/.local/bin/zclassic23-soak` | `~/.zclassic-c23-soak` | 8043 / 18242 | deliberate re-baseline |

`zclassicd` (the C++ reference) runs at 8033/8232 — **never stop it.**

---

## Standing method (do not skip)

- **Deploy builds fresh + verifies.** `make deploy` now `rm`s the binary and
  rebuilds (the binary rule isn't depfile-tracked — a header-only edit would
  otherwise ship a STALE binary; that was the outage root). `deploy_verify.sh`
  confirms the running `build_commit`. Never trust `make -j` to have produced
  the production binary — only `make zclassic23` does.
- **Copy-prove before live.** Reproduce any boot/recovery/coins change on a
  datadir COPY (isolated ports) before deploying. Never live surgery.
- **Consensus parity is inviolable** (`docs/CONSENSUS_PARITY_DOCTRINE.md`): never
  ship a consensus change to zclassic23 first; the `check-consensus-parity` lint
  gate + `test_consensus_parity` enforce it.
- **Never weaken a safety/operator gate.** The anti-eclipse peer floor, the
  detached-island guard, the torn-import gate are honest messengers.
- Gate every change: `make` (fresh binary) + `make lint` + `make test_parallel`
  (read the `N passed, M failed` line).
