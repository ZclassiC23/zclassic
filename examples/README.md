# zclassic23 teaching examples

Ten small, standalone C23 programs that each exercise a real slice of the
node — the same `lib/sim`, `lib/consensus`, `lib/wallet`, `lib/script`,
`lib/znam`, `lib/zslp`, `lib/net` code a live node runs, driven through the
deterministic simulator (`lib/sim/include/sim/simnet.h` and friends) instead
of a live chain. No disk, no real proof-of-work, no live funds, no wall
clock — but genuine consensus validation (`connect_block()`), a real
projection layer, and a real P2P message processor.

Each example is paired with a walkthrough doc under
[`docs/cookbook/`](../docs/cookbook/) that explains the mental model, shows
expected output, and maps every function called back to its production
counterpart.

## Build and run

```bash
make -C examples          # build every example into examples/bin/
make -C examples run      # build, then run every one; stops at the first
                           # nonzero exit
make -C examples clean    # remove examples/bin/ and examples/obj/
```

The examples Makefile is standalone (see [`Makefile`](./Makefile) for how it
reuses the root build's object tree and compile/link flags without
duplicating or drifting from them) — it does not modify the root
`../Makefile`. Building it will, on first run, build (or reuse) the root's
`test_parallel_fast` object tree via `make -C .. test_parallel_fast`.

## The examples

| # | Program | What it teaches | Run it |
|---|---------|------------------|--------|
| 01 | [`01_mint_and_spend.c`](./01_mint_and_spend.c) | The simulator "hello world": mint a coinbase to a deterministic wallet, mine past `COINBASE_MATURITY` (100 blocks), spend it, and verify the UTXO-set fold. | `./bin/01_mint_and_spend` |
| 02 | [`02_multi_io_p2sh.c`](./02_multi_io_p2sh.c) | Two transaction shapes a wallet must build: a multi-input/multi-output transparent send, and a P2SH HTLC fund + reveal-the-secret redeem. | `./bin/02_multi_io_p2sh` |
| 03 | [`03_op_return_overlay.c`](./03_op_return_overlay.c) | `OP_RETURN` as a provably-unspendable data carrier — permanent in the block, pruned from the live UTXO set — and the Lokad-ID-tagged-payload pattern every overlay protocol (ZNAM, ZSLP) builds on. | `./bin/03_op_return_overlay` |
| 04 | [`04_zslp_token.c`](./04_zslp_token.c) | The ZSLP (SLP Type 1) token overlay end to end: GENESIS -> SEND -> MINT, folded into the `zslp_tokens`/`zslp_transfers` projection and re-parsed independently from the wire bytes. Also shows a real projection gap (MINT doesn't update `total_minted`). | `./bin/04_zslp_token` |
| 05 | [`05_znam_names.c`](./05_znam_names.c) | The ZNAM on-chain name registry: REGISTER -> owner UPDATE -> SET_TEXT, then a non-owner forged UPDATE that parses as valid wire data but is discarded by the projection's owner-address check. | `./bin/05_znam_names` |
| 06 | [`06_htlc_swap.c`](./06_htlc_swap.c) | The HTLC (Hash Time-Locked Contract) atomic-swap primitive: both settlement paths — redeem (secret-holder claims) and refund (funder reclaims after CLTV timeout), including the pre-timeout rejection. | `./bin/06_htlc_swap` |
| 07 | [`07_cluster_reorg.c`](./07_cluster_reorg.c) | Multi-node convergence: two independent simulated chains disagree, then a real `disconnect_block`/`connect_block` reorg brings both nodes to the same tip hash and UTXO digest. | `./bin/07_cluster_reorg` |
| 08 | [`08_byzantine_blocks.c`](./08_byzantine_blocks.c) | Four ways a block/header can be adversarially malformed (bad merkle root, over-subsidy coinbase, negative output, invalid PoW) — each rejected with a named reason, tip unchanged, chain still live afterward. | `./bin/08_byzantine_blocks` |
| 09 | [`09_seed_replay.c`](./09_seed_replay.c) | Determinism as a debugging tool: a `seed_tape_t` records RNG draws, clock advances, and injected events; both a plain `.bin` snapshot and a postmortem crash capsule replay the exact same continuation. | `./bin/09_seed_replay` |
| 10 | [`10_wire_adversaries.c`](./10_wire_adversaries.c) | The in-memory P2P wire harness under attack: an honest peer plus a flooding adversary, a partition of the honest link with no silent halt, then heal and recover. | `./bin/10_wire_adversaries` |

Every example prints a numbered step narration and exits 0 on success (the
same convention `make -C examples run` checks). Two examples are
intentionally *not* byte-for-byte reproducible across runs, and say so in
their own header comments and console output:

- **06** — the HTLC secret comes from the real OS CSPRNG (`GetRandBytes`),
  not the seed tape's mockable RNG hook, because genuine cryptographic
  secret material must never be routed through a replayable test hook. The
  surrounding chain mechanics (heights, fees, tx sizes, addresses) are still
  deterministic.
- **09** — its own point is snapshot/replay of a seed tape, so it creates a
  fresh `mkdtemp()` scratch directory (and a PID-suffixed capsule filename)
  every run; the actual RNG continuation and event replay it demonstrates
  are still verified bit-for-bit inside the run itself.

## Where to go next

- [`docs/cookbook/`](../docs/cookbook/) — one walkthrough doc per example,
  with expected console output and a "Production counterpart" section
  mapping every simulator call back to the real node code path (or naming
  the gap, where one exists).
- [`docs/SIMULATOR.md`](../docs/SIMULATOR.md) / [`docs/SIMULATOR_TXNS.md`](../docs/SIMULATOR_TXNS.md) —
  the simulator's own design docs and transaction-shape coverage table.
- `lib/test/src/test_simnet*.c` — the ground-truth test suite these
  examples are built from; when in doubt about an API's contract, that's
  the authoritative source.
- [`docs/CODEBASE_MAP.md`](../docs/CODEBASE_MAP.md) — where things live in
  the wider node codebase.
