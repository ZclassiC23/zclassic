# S7.2 operational integrity and Sovereign Space/Scout lane

**STATUS: COMPLETE (`wf_zcode-s7-2-space`, implementation head `1312261ee`)**

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

### Phase 3 — banked

- Born-red witness: the base science resolver copied at most eight POINTER
  rows, sorted unrelated publishers by their signed but publisher-controlled
  sequence, and aborted the entire fetch when policy denied any one candidate.
  The record projection did not tell an operator which rows were conflicted or
  superseded. Thus eight high-sequence Sybil/conflict rows could hide an honest
  transport root, and one denied publisher could deny an otherwise permitted
  root from another stream.
- Record comparison now defines a stream by kind, namespace, network,
  delegation master, provider and the kind's semantic/transport subject.
  Sequence is compared only within that stream. Both members of an equal-slot
  valid conflict remain explicit and unusable; a row is superseded only by a
  higher sequence in its own stream. Signed record sequences are constrained
  to `1..INT64_MAX`, closing the accepted-wire-to-signed-JSON conversion gap;
  the exact boundary is tested.
- The record read projects `conflicted`, `superseded` and
  `provider_authenticated` for every row, plus usable/superseded totals and a
  separate conflict list. Science resolution consumes the full bounded
  64-record result, removes conflicted/superseded rows, orders authenticated
  provider evidence first and otherwise canonically, admits distinct transport
  roots before duplicate rows, applies FETCH/STORE/INDEX policy per candidate,
  and caps the post-policy carrier set at DHT `K=16`. If every observation is
  unusable it returns the named `POINTER_EVIDENCE_UNUSABLE` result rather than
  silently choosing one.
- Direct regressions construct 64 observations: eight same-root
  maximum-sequence Sybil rows, 54 conflict rows and two honest roots. They prove
  an authenticated honest root ranks first, both honest roots survive, sequence
  cannot order independent streams, and conflicts are counted but not selected.
  Projection coverage includes a two-row conflict, a genuinely superseded row
  in an independent stream and a still-usable older row outside that stream.
  Policy-adapter coverage proves publisher, transport full-root, semantic
  full-root and service-type blocks affect only their exact candidate/action.
  The science carrier test proves all 16 admitted candidates are retained.
- Focused receipts (cold): `test_zcode_dht_record` 1/1 PASS and
  `test_zcode_science_store` 1/1 PASS. `make lint` PASS (132/132) and
  `make -j16 build-only` PASS (`epoch=c1d4af1c...`). Symbol inspection of the
  production objects confirms the ranking, policy and RPC-render test seams are
  absent.
- Honest limit: ranking is deterministic local evidence selection, not a
  reputation system or truth oracle. Authentication proves the current Noise
  provider session associated with an observation; it does not prove that
  independent ZIDs, providers or roots represent independent people.
- Independent review initially rejected missing genuine supersession, exact
  policy-adapter fields and the unsigned-sequence JSON boundary. Those witnesses
  were added and pass. The technical re-review found no remaining correctness
  issue and returned **BANKABLE** after the ledger audit. Phase 3 is banked with
  Phase 4 in coherent operational-hardening commit `2273b7b60`.

### Phase 4 — banked

- Born-red witness: completed routing retained only the first responsible set,
  so loss during record query/publication could turn candidate 17 into permanent
  evidence loss. Publication completion counted aggregate successes rather than
  proving the sorted responsible prefix was resolved, and renewal retry used a
  wall-clock gate. The science provider route could report a refused restricted
  fetch as successfully scheduled, and the native fetch surface had no exact
  resumable cancellation operation.
- Discovery and publication retain the full bounded 64 authenticated lookup
  candidates. Their completion predicate scans canonical distance order: every
  earlier candidate must be complete, and only successful rows count toward the
  `K` target. A pending candidate 17 cannot be silently replaced by a faster
  candidate 18. Lost sessions fail their child operations, expose the next
  candidate and preserve incomplete/partial truth instead of manufacturing a
  responsible-set success.
- A 20-node in-process Noise fixture advances one transport frame at a time to
  the exact routing/query boundary. For discovery it fails one responsible peer,
  holds candidate 17's response, allows candidate 18 to answer first, proves the
  operation remains pending with zero records, then releases candidate 17 and
  finds its pointer. For publication it stops when all 19 remote candidates are
  captured with zero successes, denies and closes the exact closest 16 before
  queued STORE delivery, then proves candidates 17–19 each store and acknowledge
  the record. The cycle honestly remains a partial retry rather than reporting a
  false responsible-prefix success.
- Publication renewal/backoff now uses monotonic deadlines; wall time remains
  solely the signed record-validity clock. A direct regression applies large
  forward and backward wall jumps while monotonic time advances and proves the
  retry cannot run before its monotonic deadline. Session generations and late
  frame handling in the fixture prove disconnect/reconnect traffic cannot be
  mistaken for the old session.
- Provider routing renders `NO_STORE`, `FULL` and I/O refusal as named
  fail-closed results and never widens a restricted source set. Native
  `zcode.science.fetch cancel=true` requires the exact blob root, recursively
  cancels swarm work and deletes only that root's resumable state. Direct handler
  coverage asserts `canceled=true` and `restriction_widened=false`; restart
  coverage proves canceled state does not resume and an explicitly restricted
  retry still uses only its permitted provider. The already-banked owner-bound
  record lifecycle regression separately proves parent cancellation recursively
  cancels both the routing lookup and every active record child.
- Focused receipts (cold): `test_zcode_dht_service` 1/1 PASS,
  `test_zcode_swarm` plus `test_zcode_swarm_net` 2/2 PASS, and
  `test_zcode_science_store` 1/1 PASS. `make lint` PASS (132/132),
  `make -j16 build-only` PASS (`epoch=c1d4af1c...`) and `git diff --check` PASS.
  Symbol inspection confirms the publication view, provider-route render and
  native cancel/ranking seams are absent from production objects.
- Honest limits: fallback is bounded to the one 64-candidate lookup result; it
  is not an unbounded retry network. Cancellation prevents local continuation
  and restart but cannot erase bytes or messages already accepted by a remote
  peer. Wall-time validity still correctly changes when the operator's signed
  validity clock changes; only scheduling/backoff is monotonic.
- Independent review rejected an early churn fixture because it drained all
  STORE traffic before disconnect, and rejected an arithmetic coverage rule
  that allowed a fast farther node to replace a pending nearer one. The final
  fixture stops at the causal boundary and the sorted-prefix rule passes. The
  technical re-review found no remaining correctness issue and returned
  **BANKABLE** after the ledger audit. Phase 4 is banked with Phase 3 in coherent
  operational-hardening commit `2273b7b60`.

### Phase 5 — banked

- Born-red witness: no `space_manifest.v1` or `service_descriptor.v1` codec,
  CAS service, generic space publication namespace, or `metaverse.space.*`
  leaf existed. `test_space` was first registered before its codec symbols and
  failed to link on those missing APIs.
- `lib/vcs/space` now owns strict canonical variable-length wires and
  domain-separated semantic roots. A service descriptor requires a full
  protocol root, accepts only the bounded read verbs discover/fetch/list/query,
  and carries sorted unique object and capability roots. It has no endpoint,
  wallet, grant, execution or policy field. A space manifest binds bounded
  name/description, sequence, validity, service/object/portal roots and an
  optional admission-statement root to the existing canonical DHT delegation
  and online-key signature. Live verification requires the expected genesis,
  current signed window and an exact ACTIVE-ZID/beacon chain callback; expired
  signed evidence remains decodable for historical `show`.
- The metaverse space service produces an exact stateless plan token, stores
  canonical bytes only after confirm/token revalidation, reloads and re-hashes
  even pre-existing addressed objects, and uses the existing bounded blob
  carrier for network transport. Admission derives both the transport blob
  root and semantic object root before CAS write. No object parser invokes an
  executor.
- Five typed leaves are READY: `metaverse.space.plan|commit|show|publish|discover`.
  Plan is side-effect free. Commit checks local STORE and INDEX policy. Publish
  checks STORE, INDEX, SERVE and FORWARD before creating the blob carrier, then
  composes the existing signed generic POINTER/PROVIDER lifecycle under
  `space.manifest` or `space.service`. Discover checks DISCOVER before lookup,
  DISCOVER+FETCH per pointer candidate, and STORE+INDEX again after verified
  bytes reveal the manifest owner. That final gate retains the signed pointer
  publisher and, for a manifest, additionally requires its distinct owner to
  pass the same actions. The policy subject binds semantic root,
  transport/package root, publisher ZID and service type; local classification
  is deliberately never supplied by remote data.
- Discovery consumes the existing 64-row iterative record projection after its
  per-signed-stream conflict/supersession selection. The shared native adapter
  ranks independent candidates only by current provider authentication and
  canonical roots, then gives distinct transport roots the first pass before
  duplicates. Policy denial skips only that candidate. Every cache miss first
  completes iterative PROVIDER-record discovery for the exact transport root,
  then invokes the existing authenticated restricted swarm route; zero provider
  rows never schedule a fetch. Pointer publisher and signed manifest owner are
  rendered as separate evidence.
- Direct tests prove codec round trips, bounds, canonical ordering, wrong
  network, expiry, signature tamper, chain-callback refusal, INT64 sequence
  boundary, stateless planning, stale-token refusal, corrupt pre-existing CAS
  refusal, idempotent commit/admission, blob-root rederivation, typed command
  plan/commit/show behavior, absent execution/authority, and 20 duplicate
  publisher rows failing to crowd one distinct root. They also prove that a
  pointer-publisher STORE block and a distinct manifest-owner INDEX block each
  deny admission, and that provider routing is unreachable until iterative
  provider discovery returns at least one row. Focused cold receipts:
  `test_space` PASS; all seven `test_zcode_dht*` groups PASS; both
  `test_zcode_science*` groups PASS; both `test_zcode_swarm*` groups PASS; and
  `test_command_registry_catalog` PASS. `make -j16 build-only` passes at
  `epoch=c1d4af1c...`; `make lint` passes all 132 gates.
- Honest limits before Phase 6/7: service descriptors are content-addressed
  declarations authenticated only when referenced by a signed manifest; they
  are not separately signed principals. Fetch scheduling is asynchronous, so
  one discover call can report `fetch_scheduled` and a later call performs
  verified admission. Space discovery is exact-root lookup, not search or a
  global catalog. No doorbell, mailbox, board, remote action, wallet authority,
  automatic package execution or second transport exists.
- Independent review first rejected direct provider routing without iterative
  PROVIDER discovery and the loss of pointer-publisher identity at post-byte
  STORE/INDEX policy. Re-review also caught an uninitialized zero-provider JSON
  result. The shared discover-then-route adapter, dual-identity admission gate
  and direct regressions close all three. The reviewer reran the focused matrix,
  lint and build-only, confirmed test seams are absent from production objects,
  and returned **BANKABLE** with no remaining sovereignty finding. Phase 5 is
  banked in commit `e3e018823`.

### Phase 6 — banked

- Born-red witness: the Phase 5 head had no scout mission codec, traversal
  engine, evidence map, attestation, CAS service or `metaverse.space.scout.*`
  command; the phase began from that disk-backed symbol/catalog absence rather
  than a fabricated runtime failure.
- `space_scout` now defines one domain-separated canonical mission containing
  sorted starting roots, a frozen observation time and explicit depth, space,
  portal, byte and monotonic-duration caps. The pure sequential BFS attempts
  each unique root once, follows only sorted `space_manifest.v1` portal roots,
  and emits fixed typed outcomes. Its map contains visited roots/depths,
  verified owners, advertised service roots, portal outcomes, failures, the
  policy-denial count, accepted manifest bytes and a named truncation reason.
  Mission-aware validation rejects impossible evidence graphs: cap overclaims,
  unvisited cycle targets, followed targets without a depth+1 visit and limit
  outcomes inconsistent with their source depth. A portal queued but not
  visited before a byte/portal/deadline stop is named `truncated`, never
  misreported as followed.
  It has no service dispatch, package loader, wallet, message or executor
  callback.
- The deterministic evidence map excludes observer identity, so identical
  mission plus observations encode identically. A distinct signed attestation
  binds the mission root and evidence-map root to this node's existing DHT
  delegation and online key. The signature is explicitly local evidence; it
  grants neither ownership nor authority over a visited space. Mission, map and
  attestation use separate semantic domains and are each stored in local CAS,
  then reloaded, decoded, re-rooted and byte-compared even when pre-existing.
- Three typed leaves are READY under the existing branch:
  `metaverse.space.scout.plan|run|show`. Plan is stateless and side-effect free.
  Run requires the exact plan token plus `confirm:true`, current observer chain
  authorization and a matching online key. Show rechecks all three linked CAS
  objects and renders every bounded observation while stating
  `local_evidence:true`, `global_truth:false`, `grants_authority:false` and
  `executable:false`.
- Production observation consults DISCOVER and FETCH policy first by semantic
  and package root plus `space.manifest` service type, then repeats it with the
  verified owner ZID. A denial becomes only the public typed
  `policy_denied` result; rule IDs, reasons, classifications and paths never
  enter evidence bytes. Missing local objects compose the Phase 5 exact-root
  POINTER/PROVIDER discovery/admission path. The deadline-aware adapter drives
  the existing owner-bound record begin/poll/cancel lifecycle, bounds provider
  routing and delegation RPCs by the remaining monotonic budget, and cancels
  its lookup capability at expiry. It adds no RPC, DHT record or transport.
- `maximum_bytes` is enforced before local CAS/blob reads and threaded into the
  existing provider route. The generic swarm intent persists a package-content
  ceiling, verifies/parses the manifest and rejects an oversized package before
  storing its manifest or requesting a content chunk. The v3 intent reader
  remains compatible with unbounded v2 records. A bounded scout never tightens
  or aborts an unrelated shared transfer, while later explicit ordinary demand
  can lift a scout-owned bound; the scout still refuses any local read beyond
  its own cap.
- Before any mission/map/attestation CAS write, both STORE and INDEX policy are
  checked independently for all three exact evidence roots. Native mutation
  truth includes manifest admission, scheduled resumable work, CAS directory
  initialization and each new evidence object on both success and failure.
- Direct tests currently pass for canonical mission/map round trips, stable
  rerun bytes/root, cycles, dead and policy-denied portals, depth/space/portal/
  byte/deadline caps, no execution callback, exact plan confirmation, stale
  token refusal, restart-stable CAS, signed attestation linkage/tamper,
  impossible-graph rejection, lookup cancellation on deadline, shared-transfer
  coexistence, bounded-swarm restart and legacy v2 record migration.
  `test_space_scout`, `build-only`,
  `test_command_registry_catalog`, `check-command-input-keys` and
  `check-doc-counts` pass. The full impacted matrix also passes both space
  groups, all seven `zcode_dht*` groups, both `zcode_science*` groups and both
  `zcode_swarm*` groups. Full `make lint` passes all 132 gates.
- Final independent non-author verdict by `phase1_review`: **BANKABLE**. Its
  released-tree receipts reran both space groups, both swarm groups, all seven
  DHT groups, both science groups and the command catalog, with
  `git diff --check` clean. Static audit found no executor, wallet, message,
  service-dispatch, doorbell or mailbox path and no second protocol surface.
- Honest limits before Phase 7: a scheduled asynchronous fetch is terminal for
  one scout run and a later rerun may observe the admitted manifest. The scout
  records advertised service roots but deliberately does not fetch or invoke
  descriptors. Evidence sharing beyond local CAS is not added; any future
  carrier must reuse the existing package/swarm substrate. No doorbell, board,
  mailbox, agent mission, remote action or automatic C23 execution exists.
  The mission byte ceiling covers accepted manifest/carrier content; bounded
  control envelopes are governed by the separate monotonic deadline.

### Phase 7 — banked

- The existing 12-node sparse iterative proof remains unchanged. A distinct
  16-node acceptance case now creates sixteen real DHT services and Noise-bound
  sessions on a sparse distance-ordered chain, then recreates a late joiner
  with one authenticated bootstrap connection and a mission containing only
  the alpha space root.
- Four canonical signed manifests are built bottom-up in the provider's CAS and
  mirrored into four real one-chunk blob carriers. Both late joiners begin with
  empty package/object stores; each Scout observation composes iterative POINTER
  discovery, iterative PROVIDER discovery, an authenticated provider route, the
  existing provider-restricted ANNOUNCE/WANT/DATA swarm engine, semantic-root
  admission and signed-manifest verification before it can visit a space.
  Alpha advertises bravo, charlie and a dead root; bravo converges on charlie
  after alpha already queued it (the deterministic seen/cycle result); charlie
  advertises delta. Separate sovereignty-policy objects block delta on node A
  and allow it on node B; the maps assert `policy_denied` versus `verified` and
  each CAS projection matches its node-local policy (A lacks delta while B
  admits it). Both policies deny EXECUTE for an unknown
  advertised package, and the Scout path makes zero EXECUTE policy requests.
- Alpha has 24 signed pointer rows: eight equal-slot equivocation pairs (all 16
  members asserted conflicted), seven independent hostile high-sequence streams
  with distinct unusable transport roots, and one low-sequence legitimate
  stream. Iterative discovery crosses more than one eight-record response page
  without truncation, reaches the far provider through multiple hops, retains
  the hostile evidence, excludes every conflict from use and ultimately fetches
  the legitimate carrier. Provider disconnect/reachability/rebind proves churn
  recovery on the existing authenticated route.
- The bounded scout starts from alpha alone, visits permitted multi-hop spaces,
  names the dead and policy-denied roots, emits byte-identical maps on repeat,
  and stores the deterministic map plus signed attestation. After closing and
  reopening the late joiner's DHT service, package store and swarm engine, the
  canonical byte-sorted 24-record set is identical, every locally admitted
  permitted space has identical CAS bytes, the evidence map reloads identically
  and a rerun returns the same evidence/attestation roots as already recorded.
  Every assertion failure reaches one unconditional teardown for engines,
  stores, policies, maps, sixteen services and all temporary directories.
- Focused `test_zcode_dht_service` passes the original 12-node proof and the new
  16-node proof together. Both space groups, all seven DHT groups, both science
  groups, both swarm groups, store (20 cases, two declared self-skips), market
  and yardsale regressions pass. The focused ASan+UBSan DHT/model gate passes
  all seven initial and repeat groups, including the 16-node proof, with zero
  suppressions.
- The first strict cold suite correctly caught four new READ leaves absent from
  the registry-derived no-datadir-write matrix. Adding them exposed a deeper
  defect under independent review: delegation/key "load" created nested
  `zcode/dht`, while the old test observed only the top-level file set. Commit
  `1312261ee` makes load-only identity paths non-creating, preserves directory
  creation for create/save operations, directly tests both API modes and
  recursively snapshots typed datadir paths without following symlinks.
  Focused `test_read_leaf_no_datadir_write` and
  `test_zcode_dht_delegation` pass. Independent `strace` found zero former
  mkdir hits for both planners and the WAL fixture; final review verdict:
  **BANKABLE**.
- All 132 lint gates and full-program LTO pass. The final strict cold suite
  registered 904 groups, ran 895, cached 0, policy-gated 9 parameter-heavy
  groups, failed 0 and reported 22 explicit self-skips (85.0 s, 32 workers).
  `ci-reproducible` produced two byte-identical 22,375,112-byte binaries at
  SHA3-256 `e1cdb6541e0312a7fd47796584070ab85b1654c2b350c94501d38799cb3b19b2`.
  `repro-verify` produced two byte-identical 22,375,192-byte binaries from
  different-length snapshot paths at
  `9c040efa9dc5f6df827f6c11007811d640fbfc3fb753dba5626c08ea3b5fb0af`.
  Mandatory pre-push CI passed strict build-only, fast lint and 895/895
  runnable source-wide groups (9 parameter gates, 19 declared fast-lane
  self-skips); the live topology probe was intentionally disabled by the
  gate's default `ZCL_FAST_LIVE=0`.
- No deployment, live-datadir mutation, wallet operation, consensus edit,
  automatic C23 execution or second network stack occurred. Doorbells, boards,
  mailboxes, store interaction, agent actions and arbitrary service invocation
  remain future work; they must reuse the generic object/discovery transport
  and remain subject to independent local policy.

## Integrated-main receipt — 2026-08-05

- Orchestrator fetched `origin/main` at
  `9f678f70f47744ff09a64bc60a5b3f4273056a20`, then created true two-parent
  merge `21428f0a91cb62c45f43e413ddac832df3bcc4b8` from that head and final lane
  head `d77d61eefe4ec9a83dcf784f5868aab93ed1fdec`. Implementation
  `1312261ee62a5b7bcfbef849208bc39e5f2f7d07` remains an ancestor. The merge
  preview and `--remerge-diff` are conflict-free with no resolution delta;
  `docs/API_REFERENCE.md` was regenerated from all 24 combined command
  definition files and exactly matches the 521-entry registry (402 leaves,
  354 READY). Command-input-key validation passes all 370 leaf handlers.
- Newer main transaction, multisig, mixed-Sapling, process-only proof-catalog,
  32 GiB ROM and 2,048-entry block parse-cache work is preserved. Relative to
  the lane head the eight newer-main files are byte-identical to the main
  parent. S7.2 possession scheduling, distributed replication, pointer policy,
  monotonic time/churn hardening, Space v1 and Scout v1 remain intact.
- Product/test head `3ccb853f872795b11b1e05e7da32293c8dbcd5da`
  strengthens the independent-review fix: the four
  `metaverse.space.plan|show|scout.plan|scout.show` READ leaves each start from
  a literally zero-entry datadir, then the recursive tree comparison proves
  that no top-level `zcode/`, nested `zcode/dht/`, policy, publication, space,
  scout or database artifact was created. The focused group passes with 59
  registry-derived datadir READ leaves accounted for (31 directly exercised,
  28 named pre-existing gaps).
- Focused integrated tests pass: `test_zcode_store`; both `test_space*`; all
  seven `test_zcode_dht*`; both `test_zcode_science*`; both
  `test_zcode_swarm*`; command registry/generated API contracts; the 20-group
  store sweep (two declared stress-only self-skips); file market; all three
  yardsale groups; all four transaction groups; both multisig groups; ROM
  manifest; block parse cache; native API contract; and REST/API catalog.
  `transaction-lab-proof` passes 30 exact groups and reports 39/39 cases,
  38/39 simulated/live confirmations, zero failures/blocked cases inside the
  proof, and the intentionally unavailable mainnet custody tier.
- The existing seven-daemon DHT acceptance passes twice: once directly and
  once as the canonical prerequisite of `test-science-acceptance`. The latter
  then passes its two-daemon package and generic-blob carriers, cold restart,
  semantic-root rederivation, and byte-identical SQL-wipe reconstruction. The
  hermetic 12-node replication proof and 16-node Space/Scout sparse proof both
  pass inside `test_zcode_dht_service`.
- Focused ASan+UBSan passes the DHT, message, service, lookup and 32-node/12,000
  transition model gates, including the 16-node proof, with no sanitizer
  finding or suppression. All 132 lint gates pass. The cold uncached suite
  registers 904 groups, runs 895, caches 0, policy-gates 9 parameter-heavy
  groups, fails 0 and reports 19 declared self-skip markers in 87.4 seconds on
  32 workers. Full-program LTO passes.
- Fresh same-tree reproducibility produces two byte-identical 25,782,344-byte
  binaries at SHA3-256
  `806af6b13170d5aede79a5f639a86d318a8a1cc5556cb47b6aec0bb9e24ffc45`.
  Different-length snapshot paths produce two byte-identical 25,782,424-byte
  binaries at
  `d46173ceef566084320094368781d32c16f12dda41ee43e6782e70f53ac6a795`.
  Mandatory pre-push CI passes strict build-only, fast lint and all 895
  runnable source-wide groups (9 parameter gates, 19 declared self-skips); its
  live topology probe remains intentionally disabled by `ZCL_FAST_LIVE=0`.
- Independent integration review at merge `21428f0a9` returned **BANKABLE**:
  both histories and all named main/S7.2 behavior are present, the generated
  registry is combined, and there is no second transport, automatic execution,
  wallet, consensus, deployment or live-datadir expansion. Its final non-author
  audit replayed the literal-empty-datadir group, verified every integrated
  count/hash and checked the receipt-only delta; verdict remains **BANKABLE**.
- Space v1 proves canonical bounded descriptor/manifest bytes, delegated
  expiring signatures, exact-root CAS admission, generic pointer/provider
  discovery and independent local DISCOVER/FETCH/STORE/INDEX/SERVE/FORWARD
  decisions. Scout v1 remains a bounded READ plan plus explicit confirmed run:
  it follows only signed permitted manifest portals and returns a deterministic
  map with a separately signed local attestation. It is local evidence, not
  global truth, authority or execution permission. Doorbells, boards,
  mailboxes, posts/writes, store interaction, arbitrary service/package
  execution, deployment, live datadirs, wallet operations and consensus
  changes remain absent. Unknown C23 stays default-denied and confined to the
  explicit local-policy ZCODE executor; future services reuse this transport
  and object-discovery foundation rather than forming a protocol silo.
