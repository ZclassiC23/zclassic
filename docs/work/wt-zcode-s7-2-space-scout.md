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
