# S7.2 operational integrity and Sovereign Space/Scout lane

**STATUS: IN PROGRESS (`wf_zcode-s7-2-space`)**

**Worker:** `wf_zcode-s7-2-space`
**Branch:** `lane/zcode-s7-2-space`
**Base:** `origin/main` at `f5b63718ef4e7a0246a54382723323a0b03590af`
**Authority:** owner attachment dated 2026-08-05. No deployment, live-datadir
mutation, wallet operation, consensus change, automatic C23 execution or second
network stack is authorized.

## Goal and ordering

First make S7.1 possession, replication, pointer selection, churn, time and
cancellation operationally sound. Only after Phases 1–4 and their focused
tests pass, add the first read-only sovereign-space objects and deterministic
bounded scout. Every object and discovery operation reuses the existing CAS,
ZID/delegation, sovereignty-policy, DHT record, ZENDP, connman, Noise and swarm
substrates.

## Current-state audit at claim

- `boot_zcode_dht_periodic()` snapshots every STORAGE_ACK root and performs a
  full manifest/chunk rehash on every ordinary tick. There is no package
  mutation generation or bounded fair validation scheduler.
- ACK plan and commit both perform full-byte proofs, as required. Publication
  renewal is gated by one boolean `possession_current`, but restart and every
  periodic pass reach the same unbounded rehash path.
- `zcode_dht_replication` reads one local eight-row PROVIDER page plus one
  local eight-row ACK page. It is local-cache-only and does not label itself
  partial.
- science POINTER selection globally sorts unrelated publishers by their
  attacker-controlled sequence and truncates to eight transport roots.
  Policy denial of any candidate aborts the whole fetch instead of skipping
  that candidate.
- record discovery has a monotonic deadline, but publication retry/backoff is
  wall-time based. Query-time peer loss marks a responsible node complete and
  does not reopen candidates 17–64 or restart bounded routing.
- no `space_manifest.v1`, `service_descriptor.v1`, `space.scout.*` command or
  deterministic scout evidence-map object exists.

## Phase contracts

### 1. Bounded possession validation

- Add per-package mutation generation and invalidate cached proof state after
  pin, unpin, completion, deletion, mutation or failed verification.
- Snapshot generation/complete/pin, verify outside locks, then recheck both the
  generation and bytes before accepting success. The final acceptance step
  must close the last-chunk-read-to-proof-publication race.
- Keep mandatory synchronous full proofs at ACK plan and commit, after restart
  before renewal and before every renewal. Drive background validation through
  strict packages-per-cycle and bytes-per-cycle budgets with fair progress for
  large and small packages.
- Report proof age, queued roots, bytes verified, failures and next monotonic
  deadline without paths.

### 2. Distributed replication status

- Compose the owner-bound asynchronous record begin/poll/cancel lifecycle for
  all bounded PROVIDER and STORAGE_ACK pages from responsible nodes.
- Merge deterministically, retain conflicts, expire stale evidence and report
  provider hints, authenticated providers, live signed ACKs, locally
  revalidated local ACKs, declared groups, incomplete/conflicted evidence and
  the conservative durable verdict.
- Label local-cache-only output partial. Never describe remote ACKs as locally
  reverified or owner groups as separate operators.

### 3. Pointer and policy safety

- Sequence is comparable only inside one signed publisher/master/provider
  stream. Select each stream's newest nonconflicted record, then order
  independent transport roots by deterministic local evidence.
- Denied candidates are skipped individually while remaining allowed
  candidates continue. Exact root/publisher/service blocks still win for their
  subject.
- Traverse the full bounded result set, prioritize distinct streams and
  transport roots, and expose conflicts separately so eight Sybil/conflict
  rows cannot crowd out one honest candidate.

### 4. Churn, monotonic time and cancellation

- Responsible-node loss during lookup/query/publication admits candidates
  17–64 or triggers one bounded routing restart.
- Retry queues, backoff, operation deadlines and scheduling use monotonic time;
  wall time is used only for signed validity. Tests cover large forward and
  backward wall jumps under normal monotonic progress.
- Cancellation recursively closes record children, routing lookups, provider
  restrictions and resumable fetch state without ever widening a restricted
  fetch.

### 5. Sovereign Space v1

- Add canonical bounded `service_descriptor.v1` and signed
  `space_manifest.v1` CAS wires using the existing ZID/delegation identity.
- A service is identified by a full schema/protocol root and can declare only
  read-only verbs, object roots and capability requirements. It cannot grant
  wallet authority, execute code or override local policy.
- Add exact plan/commit/show/publish/discover typed commands. Publish through
  generic POINTER/PROVIDER. FETCH/STORE/INDEX/SERVE/FORWARD all consult the
  existing sovereignty engine.

### 6. Read-only Scout v1

- Add `space.scout.plan|run|show` with bounded starting roots, depth, spaces,
  portals, bytes and monotonic deadline.
- Follow only permitted portal manifests. Never ring, post, spend, expose
  private policy or execute packages.
- Store a canonical evidence-map root containing visited roots/owners,
  manifest results, advertised services, followed portals, failures, policy
  denials and truncation. Same frozen mission and observations must produce
  identical bytes. The map is local evidence, never authority or global truth.

### 7. Acceptance and integration

- Sparse 16-node proof: root-only late joiner, multi-hop discovery, cycles,
  dead portals, pointer conflicts, hostile high sequences, blocked candidates,
  provider churn, restart, deterministic bounded map, local-only bans,
  possession loss without I/O storm, more than one eight-record page, and
  identical records/intents/spaces/scout evidence after restart.
- Focused tests, ASan+UBSan, DHT/science acceptances, the existing 12-node
  replication proof, new 16-node proof, store/market/yardsale regressions,
  all lint gates, cold uncached suite, full LTO, both reproducibility gates and
  mandatory pre-push CI.
- Independent non-author implementation review and integration review precede
  merge/push. HANDOFF records exact heads, verdicts, hashes and honest limits.

## Evidence ledger

Each phase appends its born-red witness, commit, focused verdicts and any
remaining limitation here. Completion requires every bullet above to have
direct test or artifact evidence; a broad green suite cannot substitute for a
missing phase-specific proof.

### Phase 1 — banked

- Baseline witness: `config/src/boot_zcode_dht.c` at base
  `f5b63718e` called `vcs_package_store_verify_possession()` for every
  published ACK on every ordinary periodic pass. The first executable focused
  run was initially blocked before the test body by missing host presentation
  headers (`Xrandr.h`, then its development-header dependencies); no product
  failure is being claimed from that infrastructure stop.
- The package store now publishes a monotonic per-package mutation generation.
  A proof snapshots generation/manifest/pin/completeness under the store lock,
  hashes bounded chunks outside it, rejects symlinks and re-stats every
  verified CAS object, and records success only under a final
  same-generation/state check. Store-mediated plan, commit and renewal signing
  remain inside that final store-to-DHT critical section.
- The fair scheduler admits at most 64 watched roots and spends exactly one
  package, one chunk and 1 MiB per composition cycle. Reconcile performs no
  package scan; selected-root and cached-proof checks use O(1) committed,
  generation and pin snapshots. Unchanged proofs remain cached until the
  six-hour monotonic scrub deadline; generation/pin/state drift queues only the
  changed root. ACK plan and commit still call the full synchronous proof.
  Restart and renewal make the publication fail closed until a fresh scheduled
  proof completes. A service-global proof token binds each request to exactly
  one publication state, including same-root concurrent publications.
- Diagnostics expose root hashes, proof age, queued/tracked counts, byte and
  failure totals, failure class and next monotonic due time. They expose no
  path or local policy contents.
- Focused receipts (cold): `test_zcode_store` 1/1 PASS and
  `test_zcode_dht_service` 1/1 PASS. `make lint` PASS. `make -j16 build-only`
  PASS (`epoch=c1d4af1c...`). Builds used a read-only `/tmp` extraction of the
  missing XRandR/XRender/XInput/XFixes development headers via `CPATH`; neither
  the repository nor system package database was modified.
- Independent non-author review by `phase1_review` initially rejected five
  concrete race/boundedness gaps. After fixes and repeated re-review its final
  verdict was **BANKABLE**: store-mediated mutation serialization, lock order,
  global proof-token freshness, failure retry, ordinary-pass boundedness and
  diagnostics contention were all cleared. Direct operator writes to the
  datadir after final observation remain outside the supported store-mediated
  mutation boundary; an operator with that authority can invalidate any signed
  claim immediately after signing.

### Phase 2 — banked

- Born-red witness: the base `zcode.network.replication` handler performed two
  local `VCS_ZCODE_DHT_RECORDS_PER_FRAME` queries, so it could observe at most
  eight PROVIDER and eight STORAGE_ACK records, never drove responsible-node
  discovery, and emitted neither an evidence-completeness nor local-cache-only
  qualification.
- The native read now composes two owner-capability-bound asynchronous child
  discoveries (PROVIDER and STORAGE_ACK), retains a completed child while its
  sibling progresses, cancels both recursively, rejects service-generation
  changes, and retains the terminal result for a bounded 30 monotonic seconds.
  Each child uses the existing iterative 64-result responsible-node discovery;
  reaching that ceiling is surfaced as truncation and makes evidence partial.
- The deterministic evaluator deduplicates provider IDs, ACK provider IDs and
  declared owner groups; excludes both sides of a valid same-stream/same-slot
  conflict; selects the newest nonconflicted record inside each signed
  master/provider stream before deduplication; expires ACKs by signed wall-time
  windows; distinguishes signed
  provider hints, currently Noise-authenticated providers, live signed remote
  ACK claims and locally revalidated local ACKs. `durable` is fail-closed for
  partial, local-cache-only, truncated or conflicted evidence and still means
  only five live ACKs across three declared groups. Output explicitly states
  that remote ACKs were not locally reverified and groups do not prove separate
  operators.
- Focused receipts (cold): `test_zcode_dht_record` 1/1 PASS,
  `test_zcode_dht_service` 1/1 PASS (including 19 records across the sparse
  12-node multi-hop fixture, above one eight-record page),
  `test_command_registry_catalog` 1/1 PASS, `test_syncdiag_rpc` 1/1 PASS and
  all 13 `make_lint_gates` groups PASS. `make lint` PASS (132 gates) and
  `make -j16 build-only` PASS. A post-review focused rerun will be recorded
  with the banked Phase 2 commit.
- First non-author review verdict: **REJECT**. It proved that a lost
  responsible node or failed child query could still lead to a COMPLETE result
  that replication mistook for complete evidence, and that stale lower
  sequences could influence group/expiry counts. Discovery now carries an
  explicit `incomplete` bit for non-complete routing, peer loss and failed
  child queries; replication requires it to be clear. New regressions replace
  a session after routing and prove COMPLETE remains partial, and prove an
  expired newer ACK supersedes an older live ACK instead of manufacturing
  durability. Both focused groups passed after those fixes.
- Second re-review caught and rejected a healthy-path regression: routing
  returns the local responsible node, whose records were already snapshotted
  locally and which correctly has no remote peer session. It is now completed
  locally without setting `incomplete`; the healthy 19-record sparse proof
  asserts `!incomplete`, while the replaced-session proof asserts
  `incomplete`.
- Final proof review initially rejected the absence of parent lifecycle and
  exact-ceiling tests. A `ZCL_TESTING`-only backend now substitutes just clock
  and child begin/poll/cancel adapters while exercising the unchanged parent
  capability, locking, evaluation and cleanup logic. The regression proves
  wrong-owner denial, two-child cancellation, slot reuse, second-child begin
  failure, generation mismatch/restart interruption, terminal retention and
  expiry, active deadline cleanup and truncation forcing partial/non-durable.
  The real sparse service fixture now reaches exactly 64 records and asserts
  `truncated=true`.
- Final independent verdict by `phase1_review`: **BANKABLE**. It reran both
  focused groups and used symbol inspection to confirm the test backend exists
  in the test object but not the production `c1d4af1c...` build epoch. Final
  local receipts: `test_zcode_dht_record` PASS, `test_zcode_dht_service` PASS,
  `make lint` 132/132 PASS, `make -j16 build-only` PASS.
