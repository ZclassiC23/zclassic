# Chaos Harness

Two chaos tools, for two layers:

| Tool | Layer | What it kills | Make target |
|------|-------|---------------|-------------|
| `zclassic23-chaos` | **Simulation engine** | Nothing real — a deterministic in-process state machine driven by `.scenario` files | `make chaos` |
| `crash_recovery_test` | **Real process (C7)** | A real `build/bin/zclassic23` binary, via `SIGKILL` to its process group | `make test-crash` / `make test-crash-bootstrap` |

Use the **full-binary kill-9 harness** to prove on-disk recovery of a real
node under `SIGKILL`; use the **sim engine** for fast, hermetic,
seed-reproducible consensus/boot scenarios.

---

## Full-binary kill-9 (C7)

`crash_recovery_test` (`tools/crash_recovery_test.c`) spawns a **real**
`build/bin/zclassic23`, drives it briefly, `SIGKILL`s its whole process
group, restarts it, and asserts the recovery invariants. It is the
end-to-end counterpart to the in-process unit test
`lib/test/src/test_kill9_recovery.c`.

### Isolation contract (the hard rails)

The single audited chokepoint is **`tools/scripts/isolated_node_env.sh`**,
sourced by `make test-crash-bootstrap` and `make soak-ci`. It guarantees
a spawned test node can never touch the live node, datadir, or ports:

- **Throwaway datadir** under `/tmp` (`mktemp -d /tmp/zcl23-<kind>-XXXXXX`);
  refuses if the path resolves under `~/.zclassic-c23*`.
- **39xxx port quad** (`-port/-rpcport/-fsport/-httpsport` = base+0..3,
  default base 39030/39040) plus a dead `-connect=127.0.0.1:39999` sink.
  Refuses if any chosen port is in the live refuse-set
  (`8023 8033 8034 … 18034 18232 …`).
- **ss(8) LISTEN preflight** — the *authoritative* collision guard:
  refuses loudly if any chosen port is already `LISTEN`ing (an operator
  port-math error). The trap is armed *before* this check so a refusal
  never leaks the just-minted datadir.
- **`-connect=39999`** is load-bearing isolation: it sets
  `g_connect_only`, which both skips seeds and bypasses the
  auto-addnode-to-`127.0.0.1:8034` path that would otherwise dial
  `zclassicd`.
- **`-fsport` override** is required — without it the node binds the
  hardcoded live file-service port `18034`.
- **Process-group kill**: the node is spawned under `setsid` (its own
  group); teardown sends `SIGTERM` then `SIGKILL` to the whole group,
  with a `pkill -f -- -datadir=$ISO_DD` backstop that can only ever
  match the throwaway datadir string.
- **Cleanup trap** on `EXIT/INT/TERM` kills the group and `rm -rf`s the
  `/tmp` datadir (only after re-proving it is under `/tmp`).

### Bootstrap-regtest self-seed

`make test-crash-bootstrap` runs the harness with **no external
fixture**: it mints an isolated `/tmp` regtest datadir, spawns the node,
mines `--seed-blocks` blocks via `generate`, then runs the kill/restart
loop.

> **Build caveat:** the regtest `generate` RPC does not solve Equihash, so
> the seed stays at genesis. The harness detects this and prints a loud
> `DEGRADED genesis-only recovery mode` warning rather than silently
> claiming a UTXO seed. The kill/restart loop still validates real boot
> recovery; the UTXO-above-tip overshoot window reports `over=-1`
> (not-applicable) until a working regtest miner lands.

### Recovery assertions

Reused from the unit invariants, so the full-binary harness asserts the
same shape the in-process SQLite slice proves:

- **Height monotone** — `getblockcount` never regresses across a
  kill/restart (`CR_HEIGHT_REGRESSED`).
- **UTXO count monotone / commitment identical-or-advanced**
  (`CR_UTXOS_DECREASED`, `CR_COMMITMENT_CHANGED_BUT_NOT_ADVANCED`).
- **Zero UTXO above tip** (`CR_UTXO_ABOVE_TIP`) — reads the spawned
  node's **`node.db`** directly, read-only:
  `SELECT COUNT(*) FROM utxos WHERE height > tip` (tip from
  `node_state.coins_best_block → blocks.height`), the exact invariant from
  `test_kill9_recovery.c:p11_7_count_utxos_above_tip`. There is **no
  `coins.db`** — the canonical UTXO set lives in `node.db`.
- **Recovered within budget** — `getblockcount` answers ≥ pre-kill
  height within the 60 s restart wait.

### Operational variant (NOT in default CI)

MVP #7's literal "caught up to **peer**-tip within 2 min" needs a second
node. The `--with-peer` two-node resync variant (spawn a 2nd isolated
regtest node, mine on B, kill A, assert A resyncs to B's tip within 120 s)
ships opt-in/operational — regtest two-node P2P sync is timing-sensitive —
and is **not** in `make ci` or the default self-test. See `docs/RUNBOOK.md`.

---

## Simulation chaos engine (`zclassic23-chaos`)

`zclassic23-chaos` runs declarative scenarios. Each non-comment line is one
command, arguments are whitespace-separated, and assertions use simple integer
comparisons.

Run every checked-in scenario:

```bash
make chaos
```

Run one scenario with command-level progress:

```bash
make zclassic23-chaos
build/bin/zclassic23-chaos --scenario=tools/sim/scenarios/peer_churn.scenario --verbose
```

Failed standalone runs write a summary and a copy of the scenario into
`chaos-output/`. Use `--artifact-dir=PATH` to redirect those files.

## Scenario Format

Comments start with `#`. Blank lines are ignored. A minimal scenario looks like:

```text
seed        0x0000000000000001
boot_phase  idb_complete
peer_count  0
advance_clock +60s

expect      no_crash
expect      consensus_rejects == 0
```

Keep scenarios short and deterministic. Prefer explicit seeds and concrete
assertions over broad smoke checks.

## Commands

`seed HEX`
: Sets the scenario seed. Hex and decimal values are accepted.

`boot_phase idb_complete|listening|mempool_open`
: Selects the simulated boot phase reached before injected events run.

`peer_count N`
: Creates `N` in-process simulated peers.

`at_event HEIGHT COMMAND [ARGS...]`
: Records a scheduled event and dispatches the nested command immediately.
  This gives scenarios the same shape as future height-driven replay while the
  current harness remains single-pass.

`kill_peer ID`
: Disconnects a configured simulated peer.

`random_kill_peers count=N`
: Uses the scenario seed to choose `N` connected peers and disconnect them.
  The selected peers are deterministic for a given seed.

`send_block peer=I file=PATH [height=N]`
: Reads a non-empty fixture file from a connected peer, records the simulated
  send, and advances `tip_height`. When `height=N` is present, `tip_height`
  moves to that height if it is higher than the current tip. Full consensus
  validation is future work. Checked-in block fixtures live under
  `tests/fixtures/blocks/`.

`send_malformed_block peer=I type=ENUM`
: Simulates a bad block from a connected peer and increments
  `consensus_rejects`. Valid types are `invalid_pow`, `bad_merkle`,
  `bad_timestamp`, `bad_size`, `bad_coinbase`, `duplicate_tx`, `bad_bits`, and
  `bad_nonce`.

`advance_clock +DURATION`
: Moves the virtual platform clock forward. Durations use `s`, `m`, `h`, or
  `d`, for example `+30s` or `+1h`.

`trigger_oom_at LABEL`
: Arms the one-shot safe allocation fault hook and verifies the next allocation
  at `LABEL` fails.

`partition_network for=DURATION`
: Arms the network partition hook until the virtual wall clock passes the
  duration. Peer block traffic during the window is counted as
  `partition_drops` and is not delivered to the simulated node.

`expect no_crash`
: Asserts that the scenario did not mark itself crashed.

`expect METRIC OP VALUE`
: Compares a metric with `==`, `!=`, `>=`, `<=`, `>`, or `<`. Current metrics:
  `tip_height`, `reorg_count`, `consensus_rejects`, `mempool_prune_runs`,
  `mempool_prunes`, `active_peers`, `killed_peers`, `blocks_sent`,
  `malformed_blocks`, `block_bytes`, `clock_advance_count`,
  `clock_advance_seconds`, `scheduled_events`, `alloc_faults`,
  `graceful_shutdowns`, `partition_drops`, and `sim_time`.

## Adding Fault Injection

Add the production hook first, defaulting to inactive and cheap on the hot path.
Then add a narrow chaos command that arms the hook and proves it fired. Keep
the command deterministic, update `test_chaos_harness`, and add a scenario only
after the command has focused unit coverage.

Existing examples:

- Allocation faults: `lib/util/src/safe_alloc.c` plus `trigger_oom_at`.
- Network partitions: `lib/net/src/net_fault.c` plus `partition_network`.

## From Capsule To Scenario

When a postmortem capsule exposes a replayable failure, convert it into the
smallest scenario that preserves the causal shape:

1. Use the capsule seed as the scenario `seed`.
2. Map the boot state to `boot_phase`.
3. Convert peer disconnects, clock movement, malformed inputs, OOM labels, and
   network stalls into chaos commands.
4. Add the assertion that would have caught the bug, usually `expect no_crash`
   plus a specific metric.
5. Check in the scenario as a permanent regression.

## Debugging Failures

Start with `--verbose`; the harness prints each accepted command as it runs.
If a scenario fails after a production crash, inspect the postmortem capsule
beside the scenario and reduce the command list until the failure is minimal.
For parser failures, the `chaos:LINE:` prefix points to the offending line.
The failure summary in `chaos-output/` records the seed, replay command,
scenario path, boot phase, peer counts, byte counters, clock movement,
partition drops, and the key metrics needed to promote the failure into a
checked-in regression.

Before committing a scenario, run:

```bash
ZCL_TEST_ONLY=chaos_harness build/bin/test_zcl
make chaos
```
