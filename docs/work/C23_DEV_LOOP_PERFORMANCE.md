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

- Replay the representative benchmark and establish p50/p95 plus process and
  byte counts.
- Reduce the remaining 3.745-second identity/closure overhead and duplicate
  candidate/proof links. The first honest tiered receipt is 9.057 seconds:
  substantially faster, but still outside the five-second save-cycle target.
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
