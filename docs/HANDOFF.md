> **Read this file first for current live-node state.** Historical handoff
> revisions are evidence, not standing facts; recover them from Git with
> `git log --follow -- docs/HANDOFF.md` and
> `git show a22dc39265^:docs/HANDOFF.md` when incident archaeology requires it.
>
> **Durable hierarchy:** [`work/SOVEREIGN-NETWORK-ROADMAP.md`](work/SOVEREIGN-NETWORK-ROADMAP.md)
> preserves the Phase 0–6 program and promotion gates: stable sovereign sync
> first, transactional C23 hot swap second, sandboxed publishing third. It does
> not displace the immediate canonical cure below.

# HANDOFF — current state (2026-07-15)

## 0-NEW. Incoming-developer handoff (current state — 2026-07-15 evening)

**ROOT CAUSE FOUND AND FIXED: the baked SHA3 checkpoint was corrupt, not the
producer.** The prior producer that "failed" 187 times was CORRECT all along.
Two independent re-derivations proved the compiled trust-root constants at
h=3,056,758 were a mis-minted capture matching no height (the count `1,354,771`
was the h=3,056,759 value; the supply was +10,000 zats off even for that
height). The corrected constants are now baked (sealed core re-baked, ROOT
`9a7e1d6a…`) and the mint ceremony refuses to mint unless `--height=N` matches
the projection exactly, so this off-by-one class cannot recur:

```
h=3,056,758  count 1,354,769  supply 1,036,413,794,674,881
sha3 5817f0ec66738db6989cf881cf37b2148d07b978fd69e5a334855b4991ac5f85
```

**A NEW producer is running and folding honestly.** `zclassic23-mint.service`
(transient, `Restart=no`), content-addressed binary
`699140d1c4a431e84975ec50e03b660ee475a34d49fcb23710ef4b4115d32a81`, fresh
datadir `~/.zclassic-c23-mint`, FULL validation with the in-RAM fold cure. It
holds the corrected checkpoint, so its terminal gate at the anchor will pass
this time. Watch it:

```bash
zclassic23 ops producer status --datadir="$HOME/.zclassic-c23-mint"
```

The native command reports durable height and a successful-apply rate/ETA to
the compiled anchor from `progress.kv`; it does not mistake the human log or
the later replay/copy-proof/cutover work for sync authority. Proof-verify (`pv`)
is currently dominant (~70% of block time) —
`pv` cross-height parallelism is designed/in-flight to cut this to a few hours
(plan `research-this-node-it-clever-dawn.md` W4-0). The OLD failed producer
datadir `~/.zclassic-c23-mint-receipt` (binary
`34d78c95…`) is preserved evidence — do NOT restart it, do NOT relabel/inject.

**THE CURE PATH IS NOW UNBLOCKED END-TO-END.** When the producer reaches the
anchor: (1) the bundle exports; (2) `zclassic23 -verify-consensus-bundle=PATH`
run against a datadir folded to the anchor re-derives every component digest
from that datadir's OWN tables and writes the independent replay receipt
(`config/src/consensus_state_replay_receipt.c`) — the ONLY thing that lifts
production ACTIVATE's `VERIFIED_CONTAINED` latch; (3) copy-prove install on a
datadir COPY, gating on **H\* CLIMB past 3,176,325** (never "booted clean");
(4) live cutover with the three-layer revert (pre-authorized on green
copy-proof; the checkpoint re-bake was the one owner-gated step and it is done).

**Producer decision pending:** if the in-flight `pv`-lookahead lands ≥3× on its
parity oracle, restart the producer on the newer binary to save ~15 h;
otherwise let the current fold finish. Either way it now gates honestly.

**Bundle activation containment — HOW IT LIFTS (updated):** production ACTIVATE
still returns `VERIFIED_CONTAINED` before touching `progress.kv` UNLESS the
independent replay receipt described above is present and binds this exact
bundle + anchor + component digests + the running binary image. That machinery
now EXISTS and is adversarially tested (a bundle whose contents differ from the
honest local fold gets no receipt; a foreign/tampered receipt is refused). The
receipt is read through the datadir capability fd, not a pathname.

**Codebase review:** the dated, non-authoritative consolidation receipt is
[`work/archive/CODEBASE-CONSOLIDATION-REVIEW-2026-07-14.md`](work/archive/CODEBASE-CONSOLIDATION-REVIEW-2026-07-14.md).
The current plan of record is the OS-architecture plan
`~/.claude/plans/research-this-node-it-clever-dawn.md` (Wave 4).

### The OS-architecture program (owner directive 2026-07-15): zclassic23 is a secure OS whose trust foundation is the ZClassic PoW network

The organizing model is a layered machine where the first ~3.05M finalized
blocks are IMMUTABLE data — computed once, hash-committed, never recomputed —
and every trust claim reduces to PoW + the compiled binary (no DNS/CAs/registries
in the trust path). Full picture, laws, and the five-qualities design charter
(extendable / secure / LLM-friendly / fast / good UX) live in the plan file above.

**Landed this session (all on main, lint 81 gates green):**
- **ROM layer:** corrected checkpoint + height-asserted mint ceremony (above).
- **Sealed history:** `lib/storage/chain_segment.{c,h}` — write-once 0444
  SHA3-committed segment files + manifest for finalized block bodies;
  `sealsegments`/`verifysegments` commands; `dumpstate chain_segments`.
- **Software anchoring (trust from the chain):** ZANC OP_RETURN overlay
  (`lib/zanc/`, `app/controllers/src/anchor_controller.c`) — `anchor_publish`
  a SHA2/SHA3 package/file digest on-chain; `anchor_verify`/`anchor_self` check
  a file (or the running binary) against PoW history. `docs/SOFTWARE_ANCHORING.md`.
- **Kernel organs:** SYSINIT declarative boot records (12/12 stages) +
  `-sandbox=steady` (Landlock+seccomp); memory-pressure organ; watchdogd
  supervision drain; coins_ram in-RAM fold overlay; checkout build lock.
- **Independent replay receipt** (the cure's authority mechanism, above).
- **Fold crash-proofs:** `test_fold_inram_crash_proof` — durable/INRAM
  byte-equivalence + SIGKILL-resume determinism.
- **Net robustness:** inbound evict-not-reject, `-addnode-file`, addrman
  round-trip proof (addresses the 2-peer coordinator-floor breach).

**Next work, clearly ordered (plan Wave 4):**
1. **W4-0 finish:** land `pv`-lookahead (cross-height proof-verify pool, parity
   oracle is the merge bar) → producer speed decision → push.
2. **W4-1:** complete the cure (producer→bundle→receipt→copy-prove→cutover).
3. **W4-2:** wire the fold read path to prefer the sealed segment store below the
   frontier; supervised background sealer + corruption healer (refetch-by-hash);
   lint gate forbidding writers below the sealed frontier.
4. **W4-3:** kill the four remaining O(chain) boot passes (`block_index_repair_heights`,
   nChainTx pre-scan, the single-pass index scan, and `wallet_scan_blocks(0,tip)`
   — give each a durable cursor like the pprev-repair fix, so boot is O(delta)).
5. **W4-4/6:** anchor the release binary on-chain + two-builder gate; overlay SDK
   (factor the ZNAM/ZSLP/ZMSG/ZANC skeleton); Noise transport P1 wiring;
   wallet-encryption default + `zcl_sql` column denylist; finish zero-MCP;
   command-contract lint ratchet (schema+semantics+bounded-output per leaf).

**Live node (read-only observation 2026-07-15 05:08 UTC):** public canonical
remains wedged: H\*=3,176,325, local projection 3,176,326, zclassicd
3,181,871, lag 5,545. The hashes agree exactly at comparison height
3,176,326, so this is not evidence of a fork. The coordinator has no selectable
source: native P2P has 2 healthy peers vs the required floor of 3, and the
advisory mirror is blocked on `rpc-unreachable`. The imported index also
references historical `blk00049.dat` under the legacy block corpus while the
canonical C23 block directory contains only `blk00000.dat`; repair must be
proved on a datadir copy, never by live symlink/surgery. The complete shielded-
state sovereign cure remains required. Verify live before trusting this file:
`build/bin/zclassic23 getmirrorstatus`, then
`build/bin/zclassic23 dumpstate reducer_frontier` and
`build/bin/zclassic23 dumpstate chain_advance_coordinator`.

The containment latch that §0-NEW's "how it lifts" describes is the resolution
of the 2026-07-15 red-team finding that bundle provenance was self-asserted;
the independent replay receipt is exactly the "complete-state receipt derived by
local replay and stored outside the bundle" that finding demanded.

**Repo hygiene:** Wave-3/4 lanes ran in `.claude/worktrees/wf_*` worktrees and
have been merged to main; their branches remain for archaeology. Never run two
concurrent `make lint`/builds in one checkout (the checkout lock now enforces
this for the dev watcher; a manual second build still collides).

**Traps:** the shared `core.hooksPath` flaps to absolute under worktree agents (the gate self-heals; else per-process `GIT_CONFIG` override, never `make install-hooks`); the `CODEBASE_MAP.md` `test_groups:` count conflicts on parallel lanes (resolve via `bash tools/scripts/check_doc_counts.sh`); never run two concurrent `make lint`/builds in one checkout; the pre-push hook SIGPIPEs on stdout (pre-push-ci to a file, then `--no-verify`).

## 1. The live node — the wedge

The public daily-driver (`~/.zclassic-c23` : 18232) is **wedged at
H\*=3,176,325** (`coins_applied` 3,176,326), blocker
`utxo_apply.anchor_backfill_gap`: `sapling_anchors` is empty below the
reducer cursor, so unknown Sprout/Sapling roots FAIL CLOSED and hold H\*.
The network transport itself is alive: 2026-07-13 read-only audits observed
headers and block requests/receives increasing while peer count fluctuated
between three and six, and the served
hash at h=3,176,325 exactly matched zclassicd. The deployed build is old
(`3b0de63b0`) and can still promote `download_queue_starved` above the causal
history gap; current source suppresses that downstream condition and selects
the permanent anchor/nullifier blocker first, with end-to-end regression
coverage. Do not mistake the old status ranking for the cause of the freeze.
Soak (`:18242`) and dev (`:18252`) run newer builds near tip and are NOT
wedged. **Unwedging the live node via the sovereign cure is the #1 job.**
Verify with `zcl_status` / `zcl_state subsystem=reducer_frontier` before
acting — this file is a snapshot, not a live read. Root cause + mechanism:
[`docs/work/self-verified-tip-plan.md`](work/self-verified-tip-plan.md) and
memory `project_live_wedge_anchor_frontier_rootcause_2026-07-12`. The
**sovereign cure** is the self-verified UTXO/anchor rebuild that folds real
block bodies forward from the in-binary SHA3/PoW checkpoint and deletes the
borrowed `zclassicd`-minted seed path (see `CLAUDE.md` "Tenacity &
recovery").

## 2. Historical producer/cure detail (current protected producer is in §0)

2026-07-12 (`origin/main` `a7d7baf6a`) landed the fast dev loop (`make ff` /
`code map` / `code tests` / watcher `MODE=verify`), the palace legibility
layer, and an important legacy snapshot producer: one canonical
`coins_kv_snapshot_write()` (version-by-data, byte-identical, pinned SHA3
goldens) plus `snapshot_shielded_collect_from_db()` (Sapling+Sprout frontier
+ nullifiers), wired into `boot_mint_anchor.c`, and
`tools/snapshot_from_coinskv.c --shielded` (fast producer path, no fold).

**Correction:** USS v3 is not and cannot become the sovereign cure. It carries
the current Sapling/Sprout frontiers and supplied nullifiers, but omits the
historical anchor tables and does not prove complete nullifier history. USS
v1/v2/v3 are frozen legacy read-only encodings. The accelerated producer's
`progress.kv` retains the historical rows that a new
`zcl.consensus_state_bundle.v1` exporter must preserve. A 2026-07-13 schema
audit found that the preserved old producer has no `sprout_anchors`,
`sapling_anchors`, or `anchor_state` tables at all: preserve it as independent
transparent/nullifier fold evidence, but never call it a complete shielded
bundle source.

**Blocker found driving the copy-prove:** a soak-datadir copy has the
Sapling frontier but not a usable current Sprout frontier (soak was itself
seeded coins-only) — the legacy collector correctly refuses that missing
frontier rather than fabricate one, but it does not prove full anchor/nullifier
history. Only a from-genesis `-mint-anchor` fold can supply the complete
historical source, and the new exporter/proof receipt must demonstrate that it
actually did. The original producer at
`~/.zclassic-c23-anchor-mint` (PID 3329835 at the 2026-07-13 observation)
is still running and must not be stopped, rewritten, or restarted. Its old
binary predates the current export work; if it finishes, preserve the entire
datadir as immutable producer input. Its mapped executable is a deleted inode;
the read-only SHA3-256 of that actual mapping is
`0223e24712974fb5b96ca554d6139153bd632f07c75c8ccd3cc82b8e68a9f65f`.
That is forensic identity, not complete source/toolchain provenance. Do not
reduce the datadir to a USS v3 artifact.
At the latest read-only audit it was durably through h=1,050,000 at about
1.499 blocks/s. A stale main `progress.kv` mtime was a false stall signal: its
WAL was active 16 seconds before capture and the durable cursor advanced by
1,000 blocks. Never follow an `anchorstatus` restart suggestion based only on
the main DB mtime; use WAL activity and cursor/sample cadence. Current source
does exactly that and never recommends restart from offline file-age evidence;
the real producer reported `mint_in_progress_recent` with a cadence-aware
1,334-second stale deadline after the fix.

An isolated accelerated producer is also running as the low-priority transient
unit `zclassic23-mint-fast-v2.service` over
`~/.zclassic-c23-mint-fast`. It uses immutable diagnostic source identity
`882a8bcd3635bd40d4cddcc0352ff2fda03790eedd33fd40e52d1b2da6a09c88`
and binary SHA3
`69ac57adfebc3cb06a5b82a48b193c8e217fec17dba5c24be9060dbe3f1322a1`.
This is a dirty, offline development candidate, never a release or publication
artifact. The first attempt exposed a real legacy loader bug: file-0
`CDiskBlockIndex.nDataPos=1711` for h=1 was incorrectly translated to 3414,
so the reducer deserialized unrelated bytes. The loader now preserves exact
payload offsets and has a LevelDB round-trip regression test. On the real
history corpus the corrected producer durably advanced all state-applying
stages through h=67,999, with headers through h=69,999 (where the old run
failed at h=1), and retained bounded cyclic stage
skew. Its latest durable samples measured 12.048 blocks/s and an early-chain
ETA of 251,885 seconds (~2.92 days), about 8x the preserved producer's current
window; that is a provisional,
contention-limited observation, not an SLO claim. It runs with runtime CPU/IO
weight 50 (still below the preserved producer's default weight 100) and never
restarted for that adjustment. Profiling found its main thread waiting on
ext4 journal commits in 194/200 samples while the host retained CPU headroom;
the stage runner already gives each drain one outer transaction, so the next
performance pass must profile the remaining node.db/projection/journal commit
sites and batch only the measured missing boundary, without weakening SQLite
durability. Use `anchorstatus` durable cursors/rate plus WAL activity, not
process uptime or startup logs.

**2026-07-13 implementation slice:** legacy USS parsing now verifies an exact
layout and independently recomputes the transparent root/count/supply; the
manual-loader predicates bind those values plus height/hash to the compiled
checkpoint. A new `zcl.consensus_state_bundle.v1` validation core checks an
external candidate but is intentionally read-only: every valid candidate is
reported `VERIFIED_CONTAINED`, returns false, and publishes nothing. It has no
boot caller. Admission binds the declared current Sprout/Sapling roots and
their actual latest source heights, but local-header Sapling proof remains a
required protected-adapter gate. Validator, tests, and exporter use one shared
canonical codec with pinned SHA3 vectors; unfinished SQLite sidecars and
oversized rows are refused.

The contained full-history exporter now takes one locked frozen transaction
over a quiesced process-owned `progress.kv`, refuses an active RAM overlay,
requires exact computed H* plus convention-aware durable H/hash,
`coins_applied=H+1`, genesis-complete anchor/nullifier cursors, continuous
successful reducer rows, and a source receipt bound to the actual running
executable. It copies all coins, historical Sprout/Sapling anchors, and
pool-qualified nullifiers; writes a canonical eight-row proof summary and the
source receipt; independently reopens/validates; and uses FULL-durable
no-replace output. At the compiled checkpoint, hash/root/count/supply must
match exactly. The h=3,056,758 checkpoint was re-baked 2026-07-15: the original
mint captured a corrupt utxos projection (no height assert on the ceremony), so
the checkpoint carried the h=3,056,759 count and a mismatched sha3/supply. The
producer itself was CORRECT — the checkpoint constants were wrong. Two
independent re-derivations from the preserved full-validation producer at
h=3,056,758 agree: sha3 `5817f0ec…`, count 1,354,769, supply 10364137.94674881
ZCL. The mint ceremony now refuses unless the utxos projection height equals a
declared `--height=N`. Source-tree/toolchain receipt fields are honestly producer
claims, not independent rebuild proof. Admission now retains an `O_NOFOLLOW`
descriptor, opens SQLite through that identity, whole-file-hashes before/after
semantic validation, and rejects any non-canonical schema object or column.
Exporter publication is now pinned to a duplicated directory descriptor plus
one normalized filename component. It builds in an anonymous `O_TMPFILE`
through an exporter-private fd-only SQLite VFS, validates that exact inode,
then atomically hard-links the inode into the pinned directory and fsyncs the
directory. There is no mutable staging pathname to swap or clean up; parent
rename/symlink replacement and late final-name creation cannot redirect or
replace the artifact. Producer start/end receipt ownership is wired in
`boot_mint_anchor.c` for a receipt-owning binary; there is still no node-state
publisher.

**2026-07-15 mainline migration (not deployed):** new producer sessions and
receipts are v2 and bind `source_tree_root` to the 64-hex SHA-256 emitted by
`zcl.dev_source_identity.v2` over the current build-source inventory. Git is used
only for path/mode discovery; Git HEAD/object ids are absent from the source-id
preimage. New receipt digests use versioned SHA3 domains and exclude
`producer_commit`; v2 requires that field to remain empty. GitHub/external
trace metadata lives outside the receipt and is never internal authority.
Receipt v1 remains a frozen parse/inspection-only compatibility identity/codec
for already-durable evidence. No binary may resume, finalize, publish, or
install from a v1 session or receipt; it cannot be adopted or relabeled. This
migration is source/provenance plumbing only:
consensus parity is untouched and consensus `OP_SHA1` remains implemented.
V2's legacy-named `source_clean` field means the exact current inventory was
captured and is always true for the production v2 writer; it is not derived
from Git HEAD or gitlink object ids. Dirty bytes are already bound by the
source root.
The v2 source id is a dev supersession identity, not complete reproducible
provenance. Initialized gitlinks are recursively inventoried without consuming
their Git object ids, and exact linked static archives plus recursively included
generated vendor headers are included even when ignored. Other ignored or
generated inputs outside that build include tree, the full toolchain/config,
and independent rebuild proof remain outside it.

Both already-running producers predate this receipt/export contract, so neither
can honestly pass it as-is. Never inject a receipt into their live databases.
Either prove a protected offline ratification against the preserved mapped
executable plus immutable source/toolchain evidence on a completed copy, or run
a newer receipt-owning producer; absence of that proof remains a hard refusal.

The process-singleton selected-chain binder starts only from the opaque
descriptor/whole-file validation receipt and requires stable before/after durable
frontiers, exact selected H/hash and header-pass evidence, failure-free
ancestry, and a Sapling root matching both its sparse source height and bundle
H. It re-samples ancestor predicates and revalidates the exact artifact around
capture. Its lane tag is descriptive, not authority. The opaque result can
become stale after return and cannot activate anything.

Shielded completeness is now fail-closed across the audited recovery paths.
UTXO apply holds every Sapling spend/Sprout JoinSplit before coin writes while
anchor or nullifier history is incomplete. Nullifier backfill persists an exact
selected-chain target/tip receipt, verifies every body hash and Merkle root, and
performs a final progress→active-chain CAS before publishing explicit zero;
fork-bound rows are discarded and the positive marker remains. Full reindex
keeps both anchor pools and nullifiers positive through exact genesis→target
replay, then publishes all three zeros in one final transaction. General
assisted reset cannot publish zero, and legacy `-refold-staged` is contained at
preflight before any progress/reset write. The datadir PID guard is also now a
process-lifetime `O_NOFOLLOW`/`flock` exclusion rather than PID-text inference.
The reducer's shielded-history retry memo is bound to the exact selected block
hash plus Sprout, Sapling, and nullifier completeness markers. An unchanged
causal gap parks without rereading the body or inflating blocker receipts,
while a history-marker advance invalidates the park. A same-height branch
replacement stays held until both script and proof receipts bind the new hash;
missing or foreign receipt hashes are named dependencies and cannot authorize
body reads or coin writes. The canonical exporter likewise requires every
proof receipt to join the exact admitted header hash.

The C23 verify loop also became content-fingerprint-bound and fail-fast: a
controlled compiler failure fell from 185.94 seconds/18 repeated failures to
1.54 seconds/one failure, while a stable all-noop campaign took 1.07 seconds.
Source drift supersedes the campaign instead of mislabelling it. `--status`
now refuses stale pre-binding artifacts in about 0.09 seconds with an explicit
non-evaluable reason instead of replaying old failures as current.
The source-identity collector now emits the v2 SHA-256 supersession identity
over the complete current source inventory, not v1's Git-HEAD plus dirty-overlay
preimage. Fast and portable collectors are byte-equivalence tested for
history-only commits, recursive submodule bytes without gitlink object ids,
selected ignored linked archives, source supersession, deletion, mode,
pathological filenames, symlinks, and hidden-index-bit refusal. A separate
host-local mutation token makes edit/revert ABA fail closed; cached objects bind
that token, and identity-bearing links publish only by verified atomic rename.
The token is never release identity. The old 219-path overlay and 0.07–0.08
second measurements were v1-only; v2 latency is machine/worktree dependent and
is not a portable performance promise.

Canonical was not restarted, deployed, or mutated and remains at
H\*=3,176,325. The PIDs and transient services described earlier in this
section are dated forensic observations, not current process instructions.
Their datadirs remain evidence. The only active cure producer observed in the
2026-07-14 server audit is the protected receipt producer in §0.

**Next cure implementation job (without touching the producer):** independently
review and prove the v2 producer-start/end receipt ownership and exporter on a
new receipt-owning producer, plus a separately reviewed offline ratification
path for preserved immutable input.
Combine contained artifact validation with selected-chain/
trust/authority evidence under one publication CAS. Then finish a boot-only,
`FULL`-durability candidate-store
publisher with strict no-quarantine reopen and atomic old/new file exchange,
projection reconciliation, a physically restorable prior generation, and an
ENOSPC/I/O/kill/reopen campaign.
Only after those pass may manual load/refold route through it. Copy-prove on a
canonical datadir clone, gate on H\* climbing past 3,176,326 plus exact parity
and warm/kill-9 restart, then seek owner authorization for live deployment.
The legacy manual loader's payload-bound marker proves retry convergence, not
all-or-nothing publication. Runtime peer-snapshot activation remains contained.
Do NOT delete the borrowed seed / flip `-refold-from-anchor` to default in
the same move — that is a separate, later, owner-gated step.

## 3. Zero-MCP program (secondary track)

Owner directive: delete the MCP server entirely — the native CLI
(`zclassic23 <command>`) becomes the ONLY agent interface. W0/W1-A/W1-B/C
(hot-swap re-targeted onto the command registry, `88b4e1030`) are DONE.
Next: W2 (~47 remaining call sites) then W3 (delete the MCP server), per
[`docs/work/MCP-REMOVAL-WORKLIST.md`](work/MCP-REMOVAL-WORKLIST.md) (the
114-site inventory). W3 is blocked on 3 prereqs — memory
`project_zero_mcp_w3_prereqs_2026-07-12`.

## 4. MVP status

**MRS 4/8** (do not bump without proof): C1 install, C2 Tor onion <60s, C4
receive shielded, C7 kill-9 recovery pass local operator proof. C3
cold-start, C5 file market, C6 7-day soak, C8 exact parity are gated on the
sovereign cure + accumulated soak time; current formal C6 judge is
`NOT_MET` (`operator_intervention_detected_x2`). Bar: [`docs/MVP.md`](MVP.md).
Live scorer: `tools/mvp_gate.sh`.

---

## 5. Live ops state & lanes

| Lane | Datadir | Deploy | Purpose |
|---|---|---|---|
| **live** | `$HOME/.zclassic-c23` | `make deploy` (owner-gated) | Public daily-driver node. Restart only for vetted live deploys. |
| **dev** | `$HOME/.zclassic-c23-dev` | verify/probe only (`make deploy-dev` refuses) | Isolated build/test lane. Public tooling cannot restart it or publish a generation during containment. |
| **soak** | `$HOME/.zclassic-c23-soak` | deliberate re-baseline | Long-uptime / weekly evidence lane. Do not churn during development. |

The committed units declare the same intent to the binary with
`-operator-lane=canonical`, `-operator-lane=dev`, and `-operator-lane=soak`.
`zclassic23 agent`, REST `/api/v1/agent`, and MCP `zcl_operator_summary`
surface that as `operator_lane` (`zcl.operator_lane.v1`) with the lane's
restart policy. Prefer that native contract over parsing systemd names when a
lane's RPC is reachable.

`make deploy-dev`, `make deploy-dev-fast`, and `make agent-deploy-fast` are
Phase-0 contained: every public invocation refuses before service, datadir, or
generation mutation. Build, source verification, simulations, and hermetic
fixture probes remain available; resident `dlopen` probing is contained.
Retained activation machinery is exercised only by its inherited-FD-bound
hermetic self-test and is not a public deployment authority.

`make lane-health` is the read-only three-lane status check. It reports the
public live lane, long-uptime soak lane, and fresh-build dev lane with systemd
state, RPC reachability, listener state, height, lag from the live lane, peer
count, restart count, memory pressure, role readiness, soak-evidence
eligibility, any `-reindex-chainstate` flag, and the binary-owned
`bootstrapstatus.snapshot_loader` posture: snapshot seed height, active loader
path, and `recovery_hint`. `role_ready` answers whether a lane is serving its
assigned purpose (`canonical_ready`, `soak_evidence_ready`, or
`dev_lane_ready`); the dev lane is not role-ready when its lag exceeds the lane
threshold, even if RPC/listeners are up. `soak_eligible=false` means the soak
lane is alive but not currently earning clean MVP-C6 evidence. It is an
observability/failsafe check, not an automatic failover mechanism.

`make lane-recover LANE=dev` / `LANE=soak` is a read-only bounded recovery
planner that emits `zcl.lane_recovery_plan.v1`. Public `--apply` and
`ZCL_LANE_RECOVERY_APPLY=1` invocations refuse before unit, datadir,
snapshot-copy, drop-in, header-import, daemon-reload, or restart mutation;
`live`, `canonical`, and `main` remain refused as well. The script contains no
mutation implementation. Use the plan after `make lane-health` reports a
recovery hint, then obtain separately reviewed activation authority rather
than treating the planner as an apply path.

The live datadir runs `-load-snapshot-at-own-height` re-seeding 3,156,809 and
folding forward, so each restart takes about 13 minutes to replay back to the
same permanent shielded-history gap. This is retry convergence, not a cure or
self-healing success. `zclassicd`
(the C++ reference) runs as a co-located service — **never stop it.**
Ports/runbook: `docs/SYNC.md`; re-pull live state (`zcl_status`) before acting.

**Standing method:** `make deploy` rm's the binary first (a stale binary was
a real multi-day outage; `deploy_verify.sh` confirms the exact source SHA-256
and running executable SHA-256; Git `build_commit` is display-only).
Copy-prove every recovery path on a COPY before live, never live surgery;
gate on **H\* CLIMB**, not "booted without FATAL." Never weaken a
safety/operator gate. Gate every change: `make` + `make lint` +
`make test_parallel` (read the `N passed, M failed` line, not the pipe
exit). Replay any consensus-predicate tightening against REAL history first
(the h=478544 lesson — `docs/CONSENSUS_PARITY_DOCTRINE.md`).

---

## 6. Pointers

- [`docs/work/FORWARD_PLAN.md`](work/FORWARD_PLAN.md) — THE plan.
- [`docs/work/self-verified-tip-plan.md`](work/self-verified-tip-plan.md) — the sovereign-cure spine.
- [`docs/work/ROADMAPS.md`](work/ROADMAPS.md) — live vs superseded roadmaps.
- [`docs/AGENT_TRAPS.md`](AGENT_TRAPS.md) — looks-broken-but-isn't; read before "fixing" anything.
- [`docs/MVP.md`](MVP.md) — the v1 acceptance bar (8 criteria).
- [`docs/work/archive/`](work/archive/) — dated handoffs/audits/superseded roadmaps (history only).

## 7. Verify before you trust this file

A map, not the territory. Re-read the cited file:line before building on any
claim — trust the code you read THIS minute over this file. Architecture
reference (off the v1 path): [`docs/FRAMEWORK.md`](FRAMEWORK.md) +
[`docs/REFACTOR_STATUS.md`](REFACTOR_STATUS.md).
