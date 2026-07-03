# Security-audit response — 2026-06-09

Disposition of the "Deep Security & Cryptographic Audit (Round 1)" findings
(audit target commit `cd79bceb5`). Every CRITICAL/HIGH was re-verified against
source by independent recon agents before any code changed; two findings were
refuted with citations. All fixes below landed together with the
`consensus/zclassicd-parity-2026-06-08` merge, gated by build + `make lint`
(all gates) + `test_parallel` 0/380.

## Fixed

| ID | Finding | Fix |
|----|---------|-----|
| C-1 | `script_get_op` memcpy'd up to ~9994 bytes into 520-byte caller buffers (remote DoS/corruption on the validation path) | Destination-capacity guard before the copy (`script.h`); **companion fork-safety fix**: `script_get_sig_op_count` now walks data-less like zclassicd `GetSigOpCount` so sigops after an oversized push are never undercounted; `-fstack-protector-strong` added; regression tests un-skip the documented 521-byte case |
| C-2 | No coinbase subsidy ceiling on the live reducer path (inflation) | `utxo_apply_compute_block_delta` enforces `value_out(coinbase) <= get_block_subsidy(h) + fees` (zclassicd `bad-cb-amount` parity, genesis exempt, `-1` MoneyRange sentinel rejected explicitly). New status `bad_cb_amount` — deliberately NOT `value_overflow` so repair machinery never treats inflation as a repairable hole |
| C-3 | No consensus nullifier set — cross-block (and cross-tx-in-block) shielded double-spends accepted | New `nullifier_kv` kernel-store in progress.kv (`(nf,pool)` PK; Sprout/Sapling separate namespaces per zclassicd `coins.cpp`); checked+inserted in `utxo_apply` step inside the stage txn (atomic with coins commit), two-pass so a rejected block leaves zero rows; all three rewind paths delete in-range rows (fold into `utxo_apply_delete_rows_above`). **Known limit:** enforcement is activation-forward on existing/snapshot datadirs (same trust model as deferred proof validation below the snapshot anchor); a PERMANENT typed blocker `utxo_apply.nullifier_backfill_gap` records the gap; closure needs a backfill walker or snapshot nullifier extension. From-genesis replay/reindex datadirs get the complete set automatically |
| H-1 / #26 | Per-tx contextual rules (Overwinter expiry, NU version gating, finality, BIP34 height) ran nowhere live | `contextual_check_block` gained `is_ibd` (gates ONLY the per-tx call, zclassicd `main.cpp:941` parity; finality+BIP34 unconditional) and is wired at the head of the `script_validate` stage behind the tip-proximity gate from `docs/work/contextual-check-wiring-spec-2026-06-08.md` (+ the `tip_h >= 0` unseeded-tip correction). Transient infra failures (zk-params race, alloc) persist as resurrectable `internal_error`, never as a permanent consensus reject — reviewer-caught wedge class. Gate logic lives in `script_validate_contextual.c` |
| C-5 | Wallet backups written cleartext; AEAD encryptor dead code | `WALLET_BACKUP_PASSWORD` env wires `wallet_backup_encrypt_file` (WBE1, ChaCha20-Poly1305) into the run path: write → verify → encrypt → unlink plaintext → rotate; `.sqlite.enc` rotates/lists; explicit encrypt-without-password fails loudly; `--decrypt-wallet-backup` one-shot CLI is the restore path. No password ⇒ plaintext continues with a loud boot warning (refusing would silently kill the fleet-wide key-loss safety net) |
| H-3 | Unauthenticated `/api/wallet` (+ `/api/messages`, `/api/swaps`) on the 0.0.0.0 TLS explorer | `api_route_is_operator_private` classifier + listener-side 403 (no CORS header) in `https_server.c` before dispatch. Public chain-data routes unchanged; onion exposes no `/api`; the in-process `wallet_gui` consumer bypasses the listener and keeps working |

## Refuted (no code change needed — pinned so nobody "fixes" them later)

| ID | Audit claim | Reality |
|----|------------|---------|
| C-4 (retarget half) | "Difficulty retarget not enforced on live ingest" | **False.** Every P2P header runs `accept_block_header → contextual_check_block_header → GetNextWorkRequired` (`bad-diffbits`) — `msg_headers.c:355 → accept_block_header.c:391`. The reducer's `validate_headers` deliberately re-checks only absolute PoW + Equihash; duplicating retarget there would re-introduce the sparse-snapshot-tail wedge class. The real gap was the per-tx half (#26), fixed above |
| M-2 | "SIGHASH_SINGLE should return the Bitcoin `uint256(1)` sentinel" | **False positive — implementing it would CREATE a fork.** zclassicd throws `logic_error` (`interpreter.cpp:1158-1163`), catches it (`:1197-1202`) and returns false → OP_CHECKSIG pushes false. c23 already matches bug-for-bug. ZClassic forked zcashd post-sentinel-removal, so no mainnet tx can need it. Pinned by a CONSENSUS comment in `sighash.c` + an end-to-end test that signs over the would-be sentinel hash and asserts reject |

## Deferred (tracked, not in this change)

- C-3 anchor-existence half of `HaveShieldedRequirements` (FR-4 sapling-root) — Codex-owned per `docs/work/consensus-parity-supplemental-audit-2026-06-08.md`.
- Nullifier backfill for pre-activation history (typed blocker `utxo_apply.nullifier_backfill_gap`).
- Mempool-level shielded double-spend check (block-connect enforcement makes this policy, not consensus).
- H-2 (sync persistence writes keys plaintext despite passphrase), M-1 (mempool standardness flags), M-3..M-10, LOW items — next round.
- Audit M-3 note: `OP_CODESEPARATOR` divergence stands as-is pending a dedicated parity decision.

## Deployment gating

The merge gate (build / lint / 0/380 tests) is NOT the deployment gate. Before
`make deploy`: prove on a datadir COPY per `docs/work/fast-path.md` —
historical-replay zero-contextual-reject below the proximity window, nullifier
false-positive-free advance over real shielded blocks, and the IBD-latch parity
checks listed in the #26 spec.
