# C23 development-loop performance ledger

This is the one performance authority for the live C23 developer loop. It
records measurements, not aspirations. The product contract remains
[`ZCODE_DEVELOPMENT_PRODUCT.md`](./ZCODE_DEVELOPMENT_PRODUCT.md); the runtime
and safety contract remains [`HOTSWAP.md`](./HOTSWAP.md).

The mission is simple: edit C23, let the resident owner classify and prove the
change, then read one concise result. Production remains one static,
reproducible, LTO-optimized binary. Consensus, reducer, storage, wallet,
transaction, network, supervisor and deployment authority never becomes
dynamically reloadable.

## Frozen benchmark v1

Command: `make dev-loop-history-bench`. The history window is permanently
anchored at `cdb0305a7a68544cdd26209e9074adaeda24a1a9`; later implementations
are compared against the same edit population. The generated, gitignored receipt is
`build/dev-loop/history-benchmark.json`; `make
dev-loop-history-bench-selftest` pins the boundary classifications.

At source head `cdb0305a7a68544cdd26209e9074adaeda24a1a9`, the benchmark walks
the latest 100 commits that each changed at least one production C translation
unit. It covers 259 edit occurrences in 158 current translation units, from
`b0d0f218ea5253a85a36686ae6ac0b557190491d` back through
`ee05b19a2b2dfdf0590d28800ecdf948d98b781d`; the frozen classified rows hash to
SHA-256 `2d3beeba79ceb1f69333adfc34a8583a4584d55d9c53e37d89cca1b034684b01`.

| Current class | Edit occurrences | Unique TUs |
|---|---:|---:|
| currently live-reloaded | 15 | 9 |
| eligible but unregistered | 3 | 2 |
| blocked by mutable file-scope state | 21 | 16 |
| blocked by direct global/state access | 61 | 31 |
| blocked by whole-node/host ABI assumptions | 35 | 21 |
| requires fast restart | 98 | 60 |
| forbidden dynamic authority | 26 | 19 |

The current narrow eligibility denominator is deliberately conservative:
existing live islands plus explicit pure codec/base/JSON/encoding/view/
condition roots that pass the static-state and direct-state scans. On that
denominator, weighted live-reload coverage is 15/18 = **83.33%**. Across all
non-forbidden production edits, only 15/233 = **6.44%** currently reach the
resident live-feedback path. The largest measured miss is therefore fast
restart and component coverage—not the already-fast status example.

The representative replay set is derived, not handpicked: the 16 most frequent
non-forbidden current TUs, ordered by edit frequency then path. Its leading
members are the vault-intent controller, ZCODE task index, ZCODE work command
owners, Living Commons codecs/verifiers, build-fabric executor and current
wallet/metaverse read islands. The JSON receipt carries the exact full list and
classification of every occurrence.

## Measurement ledger

| Slice | Coverage | Latency/process evidence | Result |
|---|---|---|---|
| Baseline | eligible live 83.33%; all non-forbidden live 6.44% | frozen replay latency not yet measured | largest miss: 98 fast-restart edits |
| P1 profiles | unchanged | `DEV_LIVE`, `DEV_RESTART`, and `INTEGRATION` contain no LTO; `RELEASE` retains LTO | `make check-dev-loop-profiles` PASS |
| P2 exact artifact cache | unchanged | isolated miss → hit → edit miss → revert hit → second-worktree hit; hits report 0 compiler and 0 linker processes | `test_dev_platform` cold PASS; frozen 100-commit benchmark re-derived unchanged |
| P3 resident restart candidate | bounded non-consensus `.c` edits outside live islands now receive isolated candidate feedback; mapped-proof coverage is not yet claimed | one real watcher edit: 1.993 s total = 168 ms compile + 1.416 s link + 70 ms probe; 1 compiler, 1 linker, 1 candidate; 0 Make/shell/LTO/datadir/port/service processes | `candidate_ready`; `runtime_published=false`; `proof_complete=false` |
| P4 resident affected proof | bounded non-consensus `.c` candidates now relink the exact changed bytes into the existing fast-test graph and execute the complete canonical exact-group expansion cold | `bg_validation_dump.c`: 2.649 s candidate + 2.078 s proof compile/link + 63.412 s for 21 groups = 73.150 s total; zero failures/skips | `proof_ready`; `proof_complete=true`; `runtime_published=false`; five-second proof target MISSED |
| P5 prompt cancellation | watcher shutdown no longer waits behind its active bounded compiler, test, or generic proof child group | real watcher interrupted an active generic `make ff`: 0.30 s stop wall time; watcher lock released; complete descendant tree absent | typed cancellation is distinct from timeout; prior source remains unchanged |
| P6 latest-wins resident | a newer save cancels the active epoch, preserves a debounced exact path batch, suppresses the stale verdict, and reaps every process group in the bounded child session | real `bg_validation_dump.c` proof superseded by `status_native_handlers.c`; only the latter became epoch 15; stop 0.07 s; zero session descendants; automatic action-plan first compile 225.6 ms | no manual hot-swap-plan command; zero Make/shell/LTO processes on the selected newest live path |
| P7 proof tiers | all production behavior groups remain immediate; the closed `make_lint_gates` policy/tooling self-test family remains required by full integration expansion and is explicitly deferred from save cycles | `bg_validation_dump.c`: 9.057 s total = 1.942 s candidate + 1.280 s proof compile/link + 2.090 s for 8 immediate groups + 3.745 s identity/closure/overhead; 13 integration groups deferred and hash-bound; zero failures/skips | `feedback_ready`; `immediate_proof_complete=true`; `integration_proof_deferred=true`; `proof_complete=false`; 73.150 s baseline reduced 87.6%, five-second target still MISSED |
| P8 epoch source guard | candidate and proof builders retain two guards when called independently; one resident epoch now guards the combined candidate/closure/proof transaction once before and once after instead of rescanning around both sub-builds | `bg_validation_dump.c`: 8.823 s total; 2 source guards = 0.248 s; closure = 3.443 s; candidate = 1.627 s; proof = 3.504 s (including 2.090 s tests) | exact source stayed stable; sub-receipts report zero private guards and the epoch reports two; five-second target still MISSED |
| P9 resident closure snapshot | the save cycle opens the existing verified graph, unions its old symbols with a direct scan of the current changed file, and defers the fresh full-index rebuild to integration; a hermetic born-red case proves a newly defined symbol still reaches its pre-existing caller | `bg_validation_dump.c`: 5.411 s total; closure fell from 3.443 s to 6.6 ms; 2 source guards = 0.247 s; candidate = 1.645 s; proof = 3.511 s | `closure_refresh_deferred=true`; full expert plans still rebuild; five-second target narrowly MISSED |
| P10 parallel feedback branches | candidate compile/link/probe and affected proof compile/link/test run concurrently inside the same before/after source epoch; cancellation polling is mutex-serialized so both branches observe latest-save cancellation without racing the watcher | `bg_validation_dump.c`: 3.832 s total; candidate branch 1.612 s overlapped proof branch 3.560 s; closure 7.5 ms; source guards 0.264 s; 8 immediate groups green, 13 integration groups deferred | representative restart feedback target PASSED; status now exposes proof tier, closure deferral, parallelism and timing without opening the full receipt |
| P11 frozen restart replay | `make dev-loop-history-replay` derives and replays all 14 non-live paths in the 16-path representative set; every sample is verify-only, comment-only, watcher-stopped, and byte-restored before the next path | 65 weighted edit occurrences: censored p50 10.070 s, p95 10.087 s; 10 paths/45 occurrences hit the 10 s bound, 2 paths/11 occurrences hit `plan-group-cap`, 1 path/6 occurrences had no exact group, and 1 path/3 occurrences was unmapped; observed bound receipts used 4 compiler, 4 complete-graph linker and 4 probe processes, with zero Make/shell/LTO | **0/65 trustworthy within 5 s**; receipt is honestly `partial` because timeouts have no final bound cycle; all replayed source bytes restored and no watcher remained |
| P12 ignored-directory containment | the recursive watcher now discards create/remove traffic for the same generated, dependency and dot-prefixed scratch directories it refuses to enter; a focused test can no longer synthesize a broad `Makefile` edit, supersede its exact source epoch and silently replace the bounded proof with generic `make ff` | all 14 paths produced final bound verdicts: p50 11.254 s, p95 16.298 s; 20/65 weighted occurrences were trustworthy-green but none were below 5 s; 45/65 failed closed on exact closure/accounting rather than timing out; 24 compiler, 24 complete-graph linker, 10 test and 14 probe processes; zero Make/shell/LTO | replay receipt is `complete`, source bytes were restored, and no watcher remained; **0/65 trustworthy within 5 s**, so the latency gate remains honestly failed |
| P13 explicit optional-proof groups | event-log correctness remains immediate, while its opt-in kill9 fuzz and throughput benchmark are separate named integration groups; the Sapling-parameter-dependent shielded payment gate is also explicitly integration-only. The runner still rejects any self-skip in an immediate group | 45/65 weighted occurrences now produce trustworthy feedback (10/14 paths); p50 10.958 s, p95 15.421 s; remaining failures are 11 capped, 6 empty and 3 unmapped occurrences; 24 compiler, 24 complete-graph linker, 10 test and 14 probe processes; zero Make/shell/LTO | proof-accounting false negatives fell from 25/65 to zero without dropping ordinary event-log correctness; **0/65 within 5 s**, so test execution and complete-graph links are now the measured critical path |
| P14 content-addressed focused proofs | the resident runner requests the existing per-group cache and accepts a cached result only when `groups_ran + groups_cached` equals the exact selected set. Skip-free executions alone may mint PASS records; the v3 key domain retires older records that could represent self-skips | 124/172 selected group executions were verified cache hits across 9 paths; 48 ran fresh. Frozen replay p50 fell 10.958→8.679 s and p95 15.421→12.163 s; 45/65 weighted occurrences remain trustworthy-green, with 24 compiler, 24 complete-graph linker, 10 test and 14 probe processes and zero Make/shell/LTO | cache accounting is receipt-bound and benchmarked, but **0/65 within 5 s**; per-group timing now identifies `test_zcode_score_receipt` (5–7 s), `test_zcode_dev_objects` (6 s), event-log correctness (6 s) and transaction intent (3–4 s) as the fresh critical groups |

The earlier single-island resident microbenchmark measured 227.280 ms p50 and
232.141 ms p95 on 20 distinct artifacts. That is historical evidence for one
status island, not a result for this frozen representative benchmark and not a
coverage claim. The next measurement must replay the derived set and report
detection, identity, compile, link/reload, test, total time, process counts,
bytes scanned, cache disposition and LTO/whole-node-link counts.

The existing non-LTO dev object graph relinks directly in 1.37 seconds and a
candidate `discover help` probe takes 0.05 seconds. Those measurements justify
the candidate path but not full process replacement: audited isolated regtest
readiness took 11.054 seconds cold and 11.204 seconds after a crash, while a
graceful stop did not drain within ten seconds. Full-node restart therefore
remains an integration tier rather than being mislabeled a sub-five-second
inner loop.

## Build profiles

- `DEV_LIVE`: one admitted module/island, affected immediate probe, no LTO.
- `DEV_RESTART`: affected cached objects plus an incremental static dev link,
  isolated restart/probe, no LTO.
- `INTEGRATION`: static non-LTO combined build and required test union.
- `RELEASE`: clean whole-program LTO and reproducibility proof.

`make check-dev-loop-profiles` is part of `watcher-safety-gates` and
`dev-loop-selftest`. The resident action-plan loader independently rejects
`-flto` and linker-plugin flags, so a stale or edited `flags.env` cannot
smuggle release work into a save cycle.

## Open gates

- Remove the restart replay's dominant proof-selection failures. All 65
  weighted occurrences terminate with exact receipts and 45 are now
  trustworthy-green, but none are below five seconds. Of the remaining 20,
  11 hit the bounded plan-group cap, 6 have no exact group and 3 are unmapped.
  Before group reuse, p50 was 10.958 s and p95 was 15.421 s.
  Content-addressed group reuse reduces the current completed measurements to p50
  8.679 s and p95 12.163 s, but does not move any weighted occurrence under
  five seconds. Source-byte instrumentation also remains open.
- The pull-verifying P7 pre-push retry visibly invoked `cc -flto=auto` while
  linking `test_parallel_fast`. Reconcile that effective command with the
  non-LTO `INTEGRATION` contract; the current profile-text gate is not enough.
- Rebuild the generated source-identity object inside the resident proof epoch.
  Tooling edits whose closure reaches `code_capsule` currently fail closed
  because the otherwise unchanged `clientversion` object still binds the
  setup epoch; no proof-complete claim is made for that class yet.
- Replace an isolated runtime only after that proof layer exists; the measured
  full-node launch is currently too slow for the five-second target.
- Bring at least 95% of non-forbidden edits under five seconds by live reload
  or isolated fast restart; current measured coverage is 6.44% live-only.
- Measure two- and four-worktree throughput.
- Batch at least ten compatible focused-green commits per full-suite/LTO proof
  without adding deployment authority.

No live service, canonical datadir, wallet, transaction, custody, deployment,
core or consensus path is part of this ledger or its benchmark.
