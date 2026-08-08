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
SHA-256 `e42c793d14b0e8f4eb3659592a291f5b1af83bb9d462e483790e5dcbfe6caa25`.

| Current class | Edit occurrences | Unique TUs |
|---|---:|---:|
| currently live-reloaded | 19 | 10 |
| eligible but unregistered | 3 | 2 |
| blocked by mutable file-scope state | 21 | 16 |
| blocked by direct global/state access | 61 | 31 |
| blocked by whole-node/host ABI assumptions | 35 | 21 |
| requires fast restart | 94 | 59 |
| forbidden dynamic authority | 26 | 19 |

The current narrow eligibility denominator is deliberately conservative:
existing live islands plus explicit pure codec/base/JSON/encoding/view/
condition roots that pass the static-state and direct-state scans. On that
denominator, weighted live-reload coverage is 19/22 = **86.36%**. Across all
non-forbidden production edits, 19/233 = **8.15%** currently reach the
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
| P15 parallel score proofs | the seven independent score/package, rejection, creation, patronage, reproduction and shadow contracts are separate exact groups under the existing `zcode_score_receipt` proof family; the full family still runs, but its members execute concurrently instead of serially in one process | the former 5–7 s score group became seven groups whose slowest measured member was reproduction at 3–5 s; frozen p50 fell 8.679→8.143 s and p95 12.163→11.957 s; 45/65 weighted occurrences remain trustworthy-green and zero Make/shell/LTO were invoked | no evidence was removed; **0/65 within 5 s** because cache planning rebuilds the source index for about 3.3 s in every focused-test process before those groups run |
| P16 resident proof-cache snapshot | the resident proof runner opens the already verified dependency snapshot and supplies every changed translation unit explicitly; a prior group closure that reaches any edit is forced fresh, while an unrelated closure may reuse its exact skip-free PASS. Missing snapshots and invalid or empty changed sets disable reuse rather than rebuilding or guessing | two consecutive frozen replays reported startup around 0.25–0.33 s instead of about 3.3 s. The transition run measured p50 5.592 s / p95 11.487 s; the immediately repeated run measured p50 5.387 s / p95 13.231 s. Both had 133/202 exact group hits across 9 paths, 24 compiler, 24 complete-graph linker, 10 test and 14 probe processes, and zero Make/shell/LTO | trustworthy sub-five-second coverage rose **0/65→10/65 (15.38%)**; 45/65 still produce trustworthy-green feedback. The remaining latency is dominated by fresh group bodies and two complete-graph links per admitted restart path, while 20/65 still fail closed at proof selection |
| P17 proof-owner terminals | reverse-call selection now records a reached file with an explicit shared impact rule as a terminal proof owner instead of climbing through its generic dispatcher into unrelated suites. Unowned callers still traverse. The package executor gained the missing existing `build_fabric` route, and resident execution refuses more than 32 immediate exact groups before compiling or linking | task-index expansion fell 181→28 groups; vault 100→76; wallet 94→86; package executor became a mapped 23-group plan. Frozen replay completed with p50 5.145 s / p95 12.336 s, 91/164 executed groups served from exact cache across 11 paths, 26 compiler/linker, 12 test and 14 probe processes, and zero Make/shell/LTO | trustworthy-green coverage rose **45/65→54/65** and sub-five-second coverage **10/65→14/65 (21.54%)**. The remaining 11/65 are explicit `affected immediate proof set exceeds resident bound` refusals, not timeouts or silently shortened plans |
| P18 metaverse read island | the read-only agent service now receives its controller-owned RPC transport as an explicit call input instead of storing it in mutable file-scope state. Its four READY/read-only leaves join the existing metaverse owner, and the native planner resolves an edited island member back to that owner and probe | frozen live occurrences rose 15→19, narrow eligible coverage 83.33→86.36%, and all-nonforbidden live coverage 6.44→8.15%; the restart replay population shrank 65→61. The public plan selected `hotswap`, `metaverse.property.list`, and an admissible 27-group closure; the real six-leaf unity module linked. A clean verify-only replay was `partial`: 3/13 paths hit the 10 s verdict bound; bound-sample p50 7.403 s, p95 10.086 s; 8/61 weighted occurrences were trustworthy under 5 s; 18 compiler, 18 complete-graph linker, 8 test and 10 probe processes; zero Make/shell/LTO | source behavior tests and all hot-swap lint gates pass. No resident activation benchmark was run because it requires the isolated dev datadir/process, which this work explicitly did not touch; no sub-250 ms result is claimed for this larger island |
| P19 non-LTO dev companions | `dev-bin` now bootstraps a fixed non-LTO package verifier from the existing `DEV_RESTART` object epoch, and every focused, strict and sanitizer test front door ensures that helper through a nested dev-profile invocation. The build-fabric and package-install owners prefer that fixed development helper while retaining the release verifier fallback. The small model-neutral adapter runner also moved from release `CFLAGS`/`LDFLAGS` to `DEV_RESTART` | the first fresh-worktree replay exposed four package-aware paths (19/61 weighted occurrences) whose proofs failed immediately only because the release verifier had not been built. After this slice those accounting failures are gone: 8 paths/32 occurrences produced bound green feedback, 3 paths/18 occurrences hit the 10 s bound and 2 paths/11 occurrences retained the explicit group-cap refusal. The clean post-slice replay measured p50 7.942 s / p95 10.091 s, 18 compiler, 18 complete-graph linker, 8 test and 10 probe processes, and zero Make/shell/LTO | clean-checkout correctness improved without hiding the real cost: **0/61** were under five seconds in this cache state. The next bottlenecks remain complete-graph relinks, fresh proof bodies, three timeouts and two broad proof plans; the release LTO verifier remains release-owned |
| P20 process-local toolchain capsule reuse | the existing canonical GCC capsule remains byte-derived on its first capture, then one process reuses it only while all nine resolved driver/backend/assembler/sysroot/ABI files retain the same device, inode, mode, size, mtime and ctime and the compiler-relevant environment digest is unchanged. A changed environment forces a fresh byte capture and produces the same canonical root when the toolchain is unchanged | the monolithic `test_zcode_dev_objects` fresh body fell 4.931→2.365 s by replacing 26 identical eleven-process GCC probe sets with one capture plus guarded cache hits. The same isolated frozen `native_zcode_work_command.c` replay fell 11.801→9.214 s. After integrating a new main source identity, the first full replay measured p50 6.828 s / p95 10.088 s and 8/61 under five seconds; its immediate repeat measured p50 5.452 s / p95 10.073 s and 10/61. The repeat used 22 compiler, 22 complete-graph linker, 10 test and 12 probe processes, with zero Make/shell/LTO | clean-source-identity sub-five-second coverage rose **0/61→8/61 (13.11%)**; the immediate repeat reached **10/61 (16.39%)**, and ordinary `zcode work run` feedback completed in 4.512 s. No proof group, action, toolchain byte, or canonical domain was removed |

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

- Bring the remaining broad plans under the resident proof bound without
  shortening them silently. The restart-only population is 61 weighted
  occurrences. The clean-source-identity replay was partial because three
  paths (18 weighted occurrences) hit the 10-second verdict bound; its
  immediate repeat reduced that to one path (6 occurrences). Two other plans
  (11 occurrences) still exceed 32 immediate exact groups and are refused
  before proof compilation/linking. The clean replay measured p50 6.828 s and
  p95 10.088 s, with 8/61 (13.11%) trustworthy under five seconds; its repeat
  measured p50 5.452 s / p95 10.073 s and 10/61 (16.39%). Source-byte
  instrumentation remains open.
- The effective `dev-bin` recipe graph is now mechanically checked for LTO,
  including its package-verifier and adapter companions. Extend the same
  expanded-command proof to every integration/pre-push entry point; profile
  variable inspection alone is not sufficient evidence.
- Rebuild the generated source-identity object inside the resident proof epoch.
  Tooling edits whose closure reaches `code_capsule` currently fail closed
  because the otherwise unchanged `clientversion` object still binds the
  setup epoch; no proof-complete claim is made for that class yet.
- Replace an isolated runtime only after that proof layer exists; the measured
  full-node launch is currently too slow for the five-second target.
- Bring at least 95% of non-forbidden edits under five seconds by live reload
  or isolated fast restart; current measured coverage is 8.15% live-only.
- Measure two- and four-worktree throughput.
- Batch at least ten compatible focused-green commits per full-suite/LTO proof
  without adding deployment authority.

No live service, canonical datadir, wallet, transaction, custody, deployment,
core or consensus path is part of this ledger or its benchmark.
