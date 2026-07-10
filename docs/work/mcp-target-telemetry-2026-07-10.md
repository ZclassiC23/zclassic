# MCP target-telemetry hardening (2026-07-10)

## Failure proved live

The detached MCP process and canonical node are different processes (and can
run different builds). Before this change, `zcl_status` and `zcl_blockers`
read the MCP proxy's local blocker globals and reported zero blockers while
`zcl_state subsystem=blocker` reported six blockers from the target node.
That was a false-green operator result.

## Enforced contract

- Blocker telemetry used by these composite MCP tools is read through the
  target node's native RPC surface.
- `zcl_status`, `zcl_operator_summary`, and `zcl_blockers` all use
  `dumpstate blocker`; none reads blocker globals in the MCP controller.
- The target snapshot must identify the `blocker` subsystem, contain the five
  internally consistent counts, a blocker array, and a nonnegative escape
  count. Missing, malformed, or contradictory evidence is **unknown/error**,
  never synthetic zero.
- The status response labels `execution_locus=composite`; blocker summaries
  label `execution_locus=target_node`, the source RPC, and target capture time.
- Top-level sync lag is `locally_validated_header_tip - served_H*`.
  A peer's advertised height is labeled untrusted and cannot set the target.
  When both inputs exist, exact `sync_gap` is returned; missing or invalid
  evidence produces JSON `null` plus a source-specific error. The compatibility
  boolean uses the explicitly returned `sync_behind_threshold_blocks=144`
  policy. `header_gap`/`header_sync_behind` remain separate, peer-advertisement
  header-download hints and are not authoritative consensus evidence.
- If target health omits its build commit, the response explicitly labels
  `build_commit=null` and `build_commit_source=target_node.unavailable`;
  `mcp_build_commit` is always separate.
- `zcl_state` keeps proxy-known subsystem names as an advisory schema list, but
  forwards well-typed target-only names instead of rejecting them in a stale
  proxy.
- `zcl_operator_summary` keeps served H* distinct from indexed/active tips:
  `gap=served_gap=validated_header_tip-served_H*`, while `index_gap` is
  separate. Cached health, an advanced index, malformed download counters, or
  missing peer/health/blocker evidence cannot manufacture `healthy=true`.
- Bare/wrapped RPC errors and wrong-subsystem dumpstate envelopes become
  present-null fields plus explicit errors across status, KPI, and sync
  diagnostics. Peer counts come only from a validated array of objects.
- Frontier order is an invariant: `served_H* <= indexed_tip <= header_tip`.
  Contradictory captures are named `chain_evidence_inconsistent`, never
  clamped into a zero gap. Zero peers blocks even at gap zero, and peer-height
  hints remain null until at least one peer supplies a valid integer claim.
- A healthy operator summary requires the typed sync state `at_tip`; a cached
  healthy flag plus `blocks_download` (even at a transient zero gap) remains
  degraded.
- Peer direction/readiness have independent known bits. A peer object that
  omits `inbound` is not silently counted as outbound, and absent readiness is
  returned as unknown rather than false.

## Regression proof

`test_mcp_controllers` seeds a conflicting proxy-only blocker and asserts that
both status surfaces return only target-node IDs/counts. It also pins bare
target-RPC failure behavior, no local fallback, target/state payload
parity, dominant-blocker selection, JSON escaping, served-H* lag semantics,
peer-height spoof resistance, malformed/error response matrices, target-only
state forwarding, build-provenance unknown labeling, adverse-evidence
priority, served-vs-indexed tip separation, invalid/max download counters,
and peer/error objects that previously looked like counts or state.

Run:

```bash
make lint
make t-fast ONLY=mcp_controllers
make t ONLY=mcp_controllers
make t-fast ONLY=mcp_router
```

Detached live verification after building the source-tree binary:

```bash
make agent-mcp-call TOOL=zcl_status
make agent-mcp-call TOOL=zcl_operator_summary
make agent-mcp-call TOOL=zcl_blockers
make agent-mcp-call TOOL=zcl_state ARGS='{"subsystem":"blocker"}'
```

The three active/per-class counts must agree. A canonical deploy/restart
remains owner-gated; verification against the existing canonical process is
read-only.

## Read-only live result

At 2026-07-10 16:13 UTC, the freshly built source-tree MCP binary queried the
unchanged canonical process (node build `3b0de63b0`) through detached HTTP:

- `zcl_status`: 6 active = 3 permanent + 2 transient + 1 dependency;
- `zcl_blockers`: the same counts and `escape_dispatched_total=1`;
- `zcl_state subsystem=blocker`: the same counts;
- `zcl_operator_summary`: the same counts;
- `zcl_status` named `utxo_apply.anchor_backfill_gap` as the dominant blocker,
  reported served H* 3,176,325, validated header 3,176,712,
  `sync_gap=387`, and set the thresholded `sync_behind=true`;
- the live peer snapshot contained 13 peers with independently known
  directions (9 inbound, 4 outbound), and the validated header—not the
  peer-advertised height—remained the sync target.

The source-tree MCP build was `ed8dcc3db-dirty`; no canonical deploy, restart,
or datadir write was performed. The exact final tree passed `make lint`,
`make t-fast ONLY=mcp_controllers`, `make t ONLY=mcp_controllers`, and the
router-focused regression suite.

## Next architecture ratchet

The compatibility implementation validates and enriches the native blocker
snapshot in the MCP process because the deployed target predates a dedicated
aggregate contract. The next ratchet is a versioned native target-node operator
status/blocker contract, captured atomically inside the node and proxied without
transport-side reshaping. That removes cross-RPC restart skew and restores the
one-builder-per-contract rule while retaining this fail-closed compatibility
path for older targets.

## Ratchet implemented later on 2026-07-10

The native aggregate now exists as `operatorsnapshot` /
`zcl_operator_snapshot`. It owns collection and classification inside the target
process, embeds the compact summary, binds process/build/network/sequence
identity, and carries typed chain, peer, download, blocker, condition, latch,
and invariant evidence. `zcl_operator_summary` uses that single target call and
falls back to the older composite only for an exact method-not-found response.
A supported but malformed response fails closed.

The capture model bounds attempts and work size, not lock-acquisition latency.
Before describing it as watchdog-safe, implement the timed/cached component
ratchet listed in
[`SESSION-HANDOFF-2026-07-10-OPERATOR-SNAPSHOT.md`](SESSION-HANDOFF-2026-07-10-OPERATOR-SNAPSHOT.md).
