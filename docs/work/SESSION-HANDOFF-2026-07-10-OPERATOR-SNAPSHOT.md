# Session handoff: target-owned operator truth + dev-loop P0

## Start here

This session hardened the detached MCP/node boundary so the proxy cannot stitch
a healthy verdict from mixed, stale, proxy-local, or peer-advertised evidence.
It also audited the C development loop. Read
[`DEV-WATCH-PLAN.md`](DEV-WATCH-PLAN.md) before changing deploy tooling.

No canonical node was restarted or deployed. Continue to honor the existing
owner gate and the Wave 3 copy-prove gate.

## Landed architecture

- Native `operatorsnapshot` is the one-builder contract for operator truth.
  MCP `zcl_operator_snapshot` proxies it; `zcl_operator_summary` projects its
  embedded summary. Exact old-target `-32601` is the only summary compatibility
  fallback. A malformed supported response fails closed.
- Identity binds build, network, PID, random process instance ID, initialization
  time, and monotonic per-process sequence.
- Chain evidence keeps served H*, indexed tip, and validated-header tip separate
  and requires durable served authority, hash/work/status bindings, ancestry,
  monotone work, validation floors, and no failure bits.
- Peer heights remain untrusted hints. Healthy requires a fresh peer snapshot
  with at least one handshake-complete `NODE_NETWORK` peer.
- Blockers use one locked generation/count/entry snapshot; conditions use one
  registry pass; the operator latch is coherent and observation never clears it.
- Header-tip mutation routes through the chain-state repository, and peer
  handshake state release-publishes immutable metadata to readers.

## Proven gates

```bash
make t-fast ONLY=mcp_controllers
make t-fast ONLY=syncdiag_rpc
```

Run `make lint` plus remaining mapped groups before deploy. The final commit
message/handoff records any additional gates completed.

## Next work, in order

1. Implement `DEV-WATCH-PLAN.md`: transactional activation before the watcher.
2. Emit the durable H* witness separately from the active-window served value;
   preserve both sides of contradictions.
3. Add frontier generation tokens. Value equality cannot detect ABA churn.
4. Make diagnostics latency bounded with try/timed locks or cached explicit
   `busy`/`stale` state. Current work/attempt count is bounded, wall time is not.
5. Add validator mutation tests for zero hash/work, missing durability,
   authority/ancestry/work/validity faults, stale/no-ready peers, and
   root/summary identity/frontier disagreements.
6. Extract the snapshot validator from the large MCP ops controller once that
   mutation matrix pins behavior.
7. Migrate the older `agent` surface to the shared frontier service only after
   dedicated durable-authority fixtures exist; this session kept that path at
   its tested compatibility boundary.

## Development-speed conclusion

The expensive foundations already exist: `ccache`, changed-object builds,
depfiles, non-LTO dev linking, focused-test routing, an isolated dev service, and
MCP self-tests. The high-leverage missing layer is save-event orchestration plus
immutable-generation activation and rollback. That provides JavaScript-like
edit/save feedback without weakening C release correctness.

