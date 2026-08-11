# Local zero-wait reflex reactor

North star: one warm, local service turns each immutable edit epoch into the
earliest trustworthy observation it can prove, then continues monotonically
through exact affected evidence, reusable acceptance, human approval, and P2P
publication; the foreground reflex never waits on Git, Make, a database,
network I/O, publication, a full link, or a full proof, and no faster candidate
may replace the trusted machinery until the current machinery proves it.

## Stage contract

```text
save
  |
  +--> EDIT_SEEN ------> IMPACT_READY ------> REFLEX
                                               |
                                               +--> ASYNC PROOF
                                                      |
                                                      +--> ACCEPTANCE
                                                             |
                                                        human approval
                                                             |
                                                      PUBLICATION
```

`EDIT_SEEN`, `IMPACT_READY`, and `reflex_ready` are observations, not proof or
publication authority. `feedback_ready` means the immediate exact groups are
green; it still names every deferred group and keeps `proof_complete=false`.
Only the existing conservative source-wide verify can produce reusable
acceptance. Only the existing ZVCS boundary after that acceptance may enqueue
publication. No stage publishes a runtime in verify mode.

The first slice persists each stage through the SHA3-sealed workspace cycle
record so `dev drive` can observe progress. That record is latest-value state,
not yet a lossless event journal: a sufficiently slow poller may skip an
intermediate stage. A per-edit immutable event stream is the next slice; until
then, missing stage observations are reported as missing rather than zero.

## Exact ordinary-edit trace

The frozen representative is
`app/controllers/src/vault_intent_controller.c`, the highest-frequency
ordinary production owner in the current replay population (6 weighted edits).
The harness atomically adds a unique comment, uses an isolated verify-only
watcher and cross-filesystem cache, then restores the source byte-for-byte.

Before the split, the first green receipt arrived in 12.677 s. Its 9.906 s
test process dominated 1.066 s aggregate compilation and 2.028 s aggregate
overlay linking. The first split-only run put the candidate reflex at 1.613 s.
The final run with all progress stages and timing fields observed the candidate
reflex at 2.463 s and the same five-group proof at 10.603 s under host load.
That exact stage-1 receipt records:

| Work | Monotonic time | Processes |
|---|---:|---:|
| `EDIT_SEEN` observation | missed by latest-value polling | 0 |
| `IMPACT_READY`, edit to observation | 164.468 ms | 0 |
| path impact body | 0.113 ms | 0 |
| closure snapshot | 20.263 ms | 0 |
| exact test selection | 207.636 ms | 0 |
| source guards, three incremental captures | 98.428 ms | 0 |
| candidate + proof compiler startup | 12.232 ms | 4 |
| candidate + proof compiler bodies | 1,009.205 ms | 4 |
| candidate + proof linker startup | 6.541 ms | 2 |
| candidate + proof linker bodies | 3,353.458 ms | 2 |
| test-runner startup | 2.742 ms | 1 |
| test-runner body | 4,741.473 ms | 1 |
| command-runtime probe | separately receipt-bound | 1 |

The test selection is five immediate exact groups and 71 named deferred
groups. The five are `test_db_migration_idempotent`,
`test_transaction_intent`, `test_wallet_funds_safety`,
`test_command_registry_catalog`, and `test_command_input_bounds`. The receipt
contains a separate monotonic selection duration; no agent chose this set.
Every save-path receipt reports zero Make, shell, LTO, and complete-graph link
processes.

The current first-slice gates are honest: the 0.113 ms impact calculation is
fast, but observable `IMPACT_READY`, candidate reflex, and immediate proof all
miss their final targets; latest-value polling missed `EDIT_SEEN` entirely.
The destination remains `EDIT_SEEN p95 <10 ms`,
`IMPACT_READY p95 <50 ms`, first diagnostic `<250 ms`, ordinary behavior story
`<500 ms` (hard story `<1 s`), and first useful red `<1 s`.

## Reachability and latency firewall

```text
REFLEX (foreground)
  inotify + 15 ms debounce
    -> static path impact rules
    -> local incremental source CAS
    -> local code-index closure snapshot
    -> pinned compiler children
    -> overlay/frozen-base linker child
    -> local immutable artifact cache
    -> bounded candidate command probe

ASYNC PROOF
    -> exact group expansion (path floor + closure)
    -> pinned test-profile compiler/linker
    -> exact test runner + verified local PASS cache

ACCEPTANCE
    -> zcl_devloop_run_cycle_mode
    -> make ff (source-wide compile, tests, lint-fast)
    -> finish_cycle

PUBLICATION
    -> vcs_devloop_anchor_cycle
    -> local ZVCS commit/proof receipt
    -> publication queue / DHT / P2P workers
```

Audited foreground reachability has no Git, Make, SQL/SQLite/database, DHT,
socket/network, P2P publication, full proof, or complete-object-graph link.
Those first become reachable after the reflex receipt: Make at conservative
acceptance, and ZVCS/DHT/P2P publication only after a green complete proof.
The local SHA3-sealed cycle record and content-addressed artifact cache are the
only foreground durable writes.

## Self-hosting rule and next slices

The trusted pre-change resident is stage 0. New reactor binaries run only as
isolated stage-1/shadow candidates until stage 0 proves their exact source.
A focused test or a candidate self-test is necessary evidence but cannot grant
replacement authority.

Next, in order:

1. Replace latest-value progress with a bounded immutable per-edit event
   journal keyed by one in-memory edit epoch; expose it through the same warm
   `drive` surface without polling gaps.
2. Make compiler diagnostics stream as soon as bytes arrive and preserve the
   first-error timestamp independently of successful process completion.
3. Add behavior stories that execute against the exact candidate epoch and
   become the default sub-second human/agent feedback unit.
4. Keep a persistent shadow compiler/test runner warm, compare it against the
   pinned authoritative path, and promote it only after exact equivalence and
   stage-0 proof.
