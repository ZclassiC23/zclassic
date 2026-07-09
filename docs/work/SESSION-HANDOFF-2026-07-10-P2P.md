# Session handoff — 2026-07-10 (P2P services + Groth16 cure wave)

`origin/main` @ **`720547d73`** (pushed). Verify live/dev state before trusting
this doc. This session was orchestration-heavy: many parallel Claude subagent
lanes (Opus for hard/crypto/security, Sonnet for scoped work), each in an
isolated git worktree, merged + gated + pushed by the orchestrator.

## Landed on origin/main this session

1. **Sapling proving bridge** (`f70b368dd`) — wallet-side proving delegates to
   the SHA-pinned canonical **librustzcash** (MIT/Apache-2.0, Zcash devs / ECC
   credited), fail-closed behind a self-test the independent **pure-C23
   consensus verifier** must accept before any proof is built. Consensus
   verification stays 100% C23. This is an **interim, oracle-verified** bridge;
   the pure-C23 prover cure (below) is the ship target that replaces it. See
   memory `c23-only-zcash-attribution`.
2. **Full t→z shielded path GREEN** — `test_shielded_payment_gate` (opens a real
   `wallet_sqlite`), proof built + verified + admitted + tamper-rejected + memo
   decrypted.
3. **Lane C merged** — in-sim shielded pipeline (note-commitment tree, real
   prover→verifier t→z/z→z, durable nullifiers + double-spend reject). Root-cause
   of the old MAX_MONEY SIGABRT: `sim_free_tx` free()ing a stack shielded-output
   (fixed `5861dd8d8`). Params-heavy Sapling groups gated behind
   `ZCL_PARAMS_TESTS=1` / `--only=`.
4. **simnet_fuzz + simnet-repro** — deterministic multi-node cluster convergence
   fuzzer (byzantine + partition + reorg, named-reason invariant, replay
   capsule) and `make simnet-repro SEED=` / `make simnet-replay CAP=`.
5. **examples/ 1–11 + docs/cookbook/** — one-command (`make -C examples run`),
   zero-setup, self-asserting C23 teaching demos; example 11 is the collectible
   market (3 sale paths through real `connect_block()`). UX pass: uniform
   `=== SUCCESS ===` banners, line-buffered output, `run-<name>` targets.
6. **ZNAM write RPCs** — `name_update/transfer/renew/set_record/set_text` + MCP
   tools + owner-auth gating (`zslp_command_build_owner_base_tx`) + tests.
7. **shielded-send sim fix** (`0f938a86a`) — routes the test's proving through
   the direct C23 circuit (not the Rust wallet facade), and adds a sim-local
   Sapling **anchor registry** (`null_view` `get_anchor` hook) so cross-block
   z→z spends resolve their anchor in-sim.
8. **CI impact-rule mappings** — every new changed-file class maps to focused
   test groups so the pre-push fast gate picks the right tests.

Gates at push: 0/507 default groups, 0/510 with `ZCL_PARAMS_TESTS=1`, lint
green, all examples green. NOTE: `test_sapling`'s `curve25519_scalarmult
timing` check is a **constant-time side-channel test that flakes under heavy
build load** (measured ratio bad under saturation, clean when quiet) — rerun
isolated before treating it as a regression.

## Landed after the first handoff write (also on origin/main)

9. **Wallet-P0 lock-holder fix** (`fix/wallet-p0-lock-holder`, verified
   merge_ready + deploy_ready, copy-proven) — the live `getnewaddress` P0. Root
   cause (re-verified, not assumed; the earlier catchup-connection fix was
   already deployed): the wallet flush's BEGIN IMMEDIATE retried only 4× with no
   backoff and stranded keys in RAM under a long WAL write-lock hold, and the
   periodic WAL checkpoint used TRUNCATE which returns BUSY on the multi-writer
   node.db so the WAL grew unbounded (196 MB live) and lengthened every hold.
   Fix: 8-attempt exponential backoff outside the sqlite call + PASSIVE
   checkpoint fallback on BUSY. 3-file storage/wallet-only diff, 0 consensus.
   **Deploy is owner-gated** — after deploy the live latched state (302 keystore
   vs 201 rows) self-heals on the next flush.
10. **On-chain ZMSG memo channel** (`feat/zmsg-onchain`, verified merge_ready)
    — agent-to-agent messaging through the existing 512-byte Sapling encrypted
    memo, parity-safe (no opcode/consensus edit). Binary 38-byte-header wire
    format (magic/version/flags/len/reply_to), send via the existing t→z path
    with a binary-safe `memo_hex`, automatic ingest at tip-finalize with
    deterministic `msg_id = SHA3(txid‖memo)` dedup, and a full in-sim e2e test
    (real prover build→mine→decrypt→parse→ingest→store, params-gated). A second
    weaker impl (`feat/onchain-memo-channel`, printable-ASCII, scan-on-demand,
    no e2e test) was superseded and NOT merged — delete that branch.

## In flight when this handoff was written (isolated worktrees, NOT merged)

- **MCP-over-onion mesh** (`feat/mcp-over-onion` @ `9b4ff1465`) — impl COMMITTED
  but its adversarial security verifier **failed on a harness error**
  (StructuredOutput retry cap), so the security verdict never completed. DO NOT
  merge until an independent adversarial verify passes (default-deny allowlist,
  always-auth, off-by-default, no bypass). Re-run the verify stage.

- **Groth16 pure-C23 prover cure** (`fix/groth16-c23-prover` @ `be2d7d5ff`).
  Two of three root-caused defects FIXED + unit-pinned + committed:
  (a) output circuit R1CS now matches the trusted setup exactly (7827
  constraints / 7821 aux — the dominant gap was a non-strict `g_d` bit
  decomposition + 5 booleanization sites; ported from sapling-crypto with
  attribution); (b) the A/B MSM is now **density-filtered** to match bellman's
  Parameters layout (was dense-positional → mispaired scalars). Remaining:
  the positive prover→verifier round-trip (all-or-nothing pairing) + the
  **spend circuit** (`sapling_spend_synthesize` still a stub, ~dozens vs
  ~98777 constraints — reference trace at
  `scratchpad/spend_circuit.trace`; the new strict-unpack gadgets are its
  prerequisites). Reference traces extracted from pinned librustzcash via a
  throwaway Rust harness (scratchpad only, never built into the binary).
- **Four P2P-service lanes** (per `docs/work/p2p-services-roadmap.md` once it
  lands): on-chain Sapling-memo message channel (`feat/onchain-memo-channel`,
  Opus); proof-bundle service (`feat/proof-bundle-service`); MCP-over-onion
  mesh — off-by-default, always-auth, default-deny allowlist
  (`feat/mcp-over-onion`, Opus, security-verified); agent-storefront example 12
  + roadmap doc (`feat/agent-storefront-demo`, was paused on the collision
  protocol waiting for a quiet build tree — resume it).
- **Wallet-P0 lock-holder** (`fix/wallet-p0-lock-holder`) — the persistent
  node.db write-lock holder (Cause 2 of the live getnewaddress failure) +
  copy-prove. Deploy is owner-gated; NOT this session's to ship.

## Standing directives reaffirmed this session

- **Shipped code MUST be pure C23**; Zcash reference (librustzcash/bellman/
  sapling-crypto, zcashd) is a **comparison oracle to port from**, never linked
  into the binary, always **prominently attributed**. Memory:
  `c23-only-zcash-attribution`.
- Push = publication, not deployment; live node untouched all session.
- Every lane: impl (worktree) → independent Sonnet verify → orchestrator
  merges + gates (`make pre-push-ci` solo-green, then push) → push. The
  pre-push `make ci` races concurrent agent builds in the shared build dir; run
  the gate solo then `git push --no-verify` for that one push.
