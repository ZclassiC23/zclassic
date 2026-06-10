# Boot decomposition — seam plan (the `S`-into-handle route)

Status: **verified plan, prove-first (2026-06-03).** Pure verbatim extraction
of `config/src/boot_services.c` is EXHAUSTED. The two clean moves already
shipped (`boot_tip_hooks.c`, `boot_projections.c`) were the last units whose
inputs all arrived by parameter. A 5-agent audit (read every cited line)
classified the remaining 177 functions / 42 statics: **zero are cleanly
movable** — each is blocked by the shared `S` static or a cross-TU static-call
seam. Further shrink requires a deliberate seam, not a move.

## The root blocker — the `S` static

`static struct boot_svc_ctx *S` (`boot_services.c:153`, written `S = svc` at
`:2223` in `app_init_services`) is read by the accessors `boot_runtime`
(`:155`), `boot_node_db` (`:162`), `boot_db_service` (`:170`), `boot_wallet`
(`:178`), AND directly by stayers `app_add_node` (`:3435,:3461`) and
`app_start_metrics` (`:3469-3475`). Any unit whose bodies call an `S`-accessor
therefore shares mutable state with functions that must stay → no behavior-
neutral move. Collapsing the file means **redesigning `S` into a passed-in
handle** so worker/adapter bodies read `svc->…` instead of the global.

Second shared static: `g_mmb_leaf_store` (`:1730`) couples
`boot_build_flyclient_proof` with `app_init_services` (`:2775-2794`) and the
replay worker (`:2090-2111`) — a self-contained mmb-leaf-store cluster.

## Units, blockers, and the seam each needs (safest-first)

| # | Unit (~LOC) | RULE a (S) | RULE b (call-seam) | Seam to design |
|---|-------------|-----------|--------------------|----------------|
| 1 | service-kernel-adapters (~720) — 32 `zcl_service_spec` start/stop adapters + `boot_configure_frontend_rpc` + sd-watchdog trio | PASSES (read `svc->*` only) | FAILS — addresses named by the 3 `boot_register_*_services` (`:1187,:1246,:1258`); `boot_miner_start` names `boot_submit_mined_block` (`:1093`) | move adapters **+ the 3 register funcs** as one anchor → `config/src/boot_service_adapters.c`; expose only `bool boot_register_{frontend,network,runtime}_services(svc)`. `g_sd_watchdog_*` ride along as file statics. |
| 2 | **msg-callbacks (~25 fns, ~380)** — block/snapshot/header/peer/wallet callbacks | PASSES (`:701-1080` use `svc->state`/`csr_instance()`/`sync_monitor_*`, no S) | FAILS — named by `app_init_services` (`:2393-2446`), `app_start_metrics` (`:3473`), `boot_miner_start` (`:1093`) | one wiring entry `void boot_wire_msg_callbacks(svc)` replacing the inline `msg_processor_set_*` block at `:2393-2430` → `config/src/boot_msg_callbacks.c`. **EXCLUDE** the mmb-leaf-store cluster (`boot_build_flyclient_proof`, `boot_load_block_hashes_range`, `boot_compute_utxo_sha3`, `boot_serialize_utxo_snapshot`, `boot_mmb_leaf_store_*_legacy`) — shares `g_mmb_leaf_store`. |
| 3 | thread-lifecycle (4 fns, ~60) | pure | callers all in unit 4 | do NOT extract standalone (would add a decl serving only blocked callers); ride into unit 4 as file statics when it unblocks. |
| 4 | background-workers (~700) — payment/replay/offer/address-backfill/projection-backfill/hodl threads + lifecycle wrappers + catchup | FAILS — bodies call `boot_node_db()` (`:1394,:1518,:1544,:1918,:1969,:2125`) | — | thread `node_db`/`db_service` onto `svc` so bodies read `svc->node_db`; THEN expose `boot_{start,join}_background_workers(svc)`. Decls only AFTER the S-dependency is removed (else unbuildable). |
| 5 | shutdown-phases (~238, 6 fns) — `app_shutdown_svc` orchestrator | FAILS — `shutdown_persist_runtime_state` calls `boot_node_db()` (`:3282`) + the S-blocked `boot_join_*` (`:3275`) | — | move as ONE contiguous block, **LAST**, after unit 4 unblocks (HIGH ordering sensitivity). |
| 6 | accessor-core (`S` + accessors + `boot_profile_has_*` + `svc_clock_ms`) and service-init-core (`app_init_services` + the 3 registers) | — | — | **STAY** by definition — the seam root. Redesigning `S` into a handle is the real prerequisite that unblocks 1/4/5. |

## Execution order (each step build + test_parallel + boot-validate on a copy)

1. **msg-callbacks** (unit 2, excluding the mmb cluster) — RULE-a-clean, low
   runtime risk (callback registration only; identical order preserved). One
   wiring seam + at most two exposed decls for stayer-named callbacks
   (`boot_submit_mined_block`, `boot_metrics_external_gauges`).
2. service-kernel-adapters + registers + sd-watchdog (unit 1).
3. **`S`-into-handle redesign** — the keystone; unblocks the rest.
4. background-workers + thread-lifecycle (units 4, 3).
5. shutdown-phases (unit 5) — LAST, one contiguous block.

## Per-extraction checklist

- `config/src/*.c` is wildcard-built (no Makefile edit).
- Add the new file to the scaffold-label `files[]` at
  `lib/test/src/test_make_lint_gates.c:2252`.
- If moving adapters, update `test_onion_bootstrap.c` line refs (cites
  `boot_services.c:1303-1316`).
- Boot-validate on a copy: `tools/repro_on_copy.sh <tag> --port=18299
  --p2p-port=18933 --deadline=480 -- -nobgvalidation` (deadline ≥480 to clear
  the ~328s `mmb_register` boot cost). Confirm tip restores, no crash.
  NOTE: while the live datadir is in the §3 coins-wedge (`utxos max_height >
  tip_height`), a fresh boot FATAL-halts at the coins-integrity gate before
  P2P starts — so no boot seam (or the UAF header-sync path) can be
  boot-validated on that datadir until the wedge is resolved (owner-gated).

## Related deferred item — boot_projections.c descriptor table (hardening audit, 2026-06-03)

`config/src/boot_projections.c` repeats a 7× block (set_event_log → open →
NULL-check → catch_up → `UINT64_MAX` branch → fprintf) for mempool, peers,
znam, wallet, contacts, onion_ann, hodl_history. A real dedup is a typed
`{name, open_fn, set_event_log_fn, catch_up_fn}` projection-descriptor table —
a NEW surface, not an inline helper (each block calls differently-named
per-type functions with no shared vtable, plus call-site-specific
`obs-ok:phase4-storage` diagnostics). Because the file was just extracted
verbatim (Wave D), this behavior-sensitive refactor must be its own
boot-validated pass on a datadir copy, not churn on the fresh extraction.
Prove-first defer until the boot path is boot-validatable again.
