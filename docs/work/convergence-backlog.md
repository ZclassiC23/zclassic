# Convergence backlog — the zclassic23 way

Source: tree-wide read-only audit (9 agents, 2026-06-05). Ranked by
value-over-risk. Each item is a SAFE convergence action (DRY / DOC / API /
SHAPE). High-risk consensus-sensitive items are quarantined to §12 and need
repro-on-copy before any edit.

**Discipline:** mutating work runs as edit-only workflows on DISJOINT file
sets, then a single build + `make lint` (35 gates) + `test_parallel` (0/N) +
boot-smoke gate, then commit per logical group. Never grow a baseline, never
weaken a gate, never touch `config/src/boot.c`.

## Wave status

| Wave | Items | State |
|------|-------|-------|
| boot-index decompose | `config/src/boot_index.c` → shape-clean units | in flight (`wjb4k7nac`) |
| Convergence Wave 1 | #1+#11, #2, #3, #6, #7+#8, #9 | in flight (`w37tgxfpv`) |
| Convergence Wave 2 | #4, #5, #10 | queued |
| Consensus-sensitive | §12 (×4) | deferred — repro-on-copy each |

## Items

1. **`coins_alloc` latent NULL-deref + lying return** *(API / real bug, low risk)* —
   `lib/coins/src/coins.c`. On OOM it logs, falls through, derefs NULL `vout`,
   and `return true` masks failure from 3 callers
   (`coins.c:65`, `coins_view_sqlite.c:784`, `utxo_projection.c:674`) whose
   `if (!coins_alloc(...))` guards are dead code. Fix: `num_vout=0; return false;`
   before the null-init loop. **→ Wave 1.**
2. **Document `check_block.h` gate booleans** *(DOC, low)* — four must-never-fork
   entry points expose undocumented "disable a safety check" bools. Lift the
   fast-sync prose from `check_block.c`. **→ Wave 1.**
3. **Unify 3 duplicate `bytes32_nonzero` onto `zcl_chainwork_is_zero`** *(DRY, low)* —
   `chain_evidence_authority_service.c:99`, `chain_evidence_snapshot.c:17`,
   `snapshot_manifest.c:19`. Leave `snapshot_offer.c` (own NULL-guard contract). **→ Wave 1.**
4. **Fix fabricated `nonce` / `confirmations` in block-header serializer** *(API, low)* —
   `blockchain_controller_blocks.c:170/162` emits `nonce:0`, `confirmations:1`
   for every block; consumer `api_controller_lookup.c:102` reads nonce as a
   string. Emit real `uint256_get_hex(nNonce)` string + `1+tip-height`. **→ Wave 2.**
5. **Unify `json_extract_int/real` controller wrappers** *(DRY, low)* — two
   byte-identical wrapper pairs re-adapting the shared `zcl_json_extract_*`,
   ~44 call sites, un-prefixed names leaking via internal headers. Add
   `zcl_json_int/real` to `views/format_helpers.h`; fold in the duplicate
   `get_difficulty`/`explorer_get_difficulty`. **→ Wave 2.**
6. **Document 13 of 15 condition headers** *(DOC, low)* — symptom→remedy→witness
   →cadence, mirroring `tip_fork_stale.h`. The condition registry is the live
   self-heal surface. **→ Wave 1.**
7. **One `stage_block_reader_fn` typedef for 4 reducer stages** *(DRY, low)* —
   four identical typedefs + setter bodies onto one `stage_default_block_reader`;
   alias to keep public names. **→ Wave 1.**
8. **Delete 3rd `cursor_persisted` copy in reorg path** *(DRY, low)* —
   `utxo_apply_delta_reorg.c:73` → `stage_cursor_persisted`. Call site is before
   `BEGIN IMMEDIATE` (no double-lock, verified). **→ Wave 1.**
9. **`condition_reset_state` primitive + `operator_needed_emitted` leak** *(DRY +
   test-isolation, low)* — only 4 of 12 `_test_reset()` clear the field; 8 leak
   operator-needed state between tests. Centralize in `lib/framework`. **→ Wave 1.**
10. **Table-drive the staged-sync supervisor** *(SHAPE, low but load-bearing)* —
    8 cloned per-stage blocks (~390 LOC) differing only by symbol → desc table +
    one generic register. Live liveness wiring; do after the mechanicals. **→ Wave 2.**
11. **Document `coins.h` lifecycle invariants** *(DOC, low)* — "empty record means
    OOM, not pruned." Rides with #1. **→ Wave 1.**

## §12 — Consensus-sensitive (deferred, repro-on-copy each, do NOT batch)

- **`disconnect_block` unused `state` param** (`connect_block.c`) — route reorg
  failures through `validation_state_*`/`REJECT_FATAL` for symmetry, or drop the
  param. Reorg path.
- **`connect_block` 16× duplicated cleanup** — consolidate the `free(checks)/…/
  block_undo_free` sites into one `goto cleanup:`. Reject reasons + DoS scores
  must stay byte-identical.
- **CCoins avail-mask decode — DIVERGED, do NOT merge** (`coins_db.c:104-131`,
  `chainstate_legacy_reader.c:109-136`) — RESOLVED 2026-06-05: read both. They
  share the CCoins mask format but are deliberately distinct: `coins_db.c` (live
  node.db read) is hardened — fixed `avail_stack[4096]` cap + `nMaskCode > 10000`
  reject — to bound untrusted-row memory; `chainstate_legacy_reader.c` (trusted
  external-chainstate import) grows its buffer unbounded so it never truncates a
  legitimately large record. A naive "extract one helper" would either strip the
  live path's DoS guard (a safety gate — forbidden) or impose truncation on the
  import path. Closed as NOT-a-dedup; both sites now carry a matching
  do-not-merge note. No code change beyond the warnings.
- **`legacy_mirror_sync_request_catchup` dual surface** — invert so the worker
  returns `zcl_result` directly (Law 2); keep `bool` as a thin adapter.
- **`update_coins` silently accepts an empty coins record on OOM**
  (`lib/validation/src/update_coins.c:98`) — `coins_from_transaction` is `void`
  and leaves `num_vout=0` on OOM, but `num_vout=0` is ALSO legitimate for an
  all-OP_RETURN/unspendable tx, so the caller cannot distinguish OOM from a
  valid empty record. The real fix is to give `coins_from_transaction` a status
  return (and propagate it through `update_coins` → `connect_block`); a naive
  `if (num_vout==0) fail` guard would false-reject valid txs. Consensus connect
  path — **repro-on-copy**, scope the `coins_from_transaction` signature change
  carefully. (Surfaced by the Wave-1 `coins_alloc` review; the live NULL-deref
  sibling at `coins_db.c:133` was fixed in Wave 1.)

## Round 4 (2026-06-05) — under-swept subsystems audited + fixed

Read-only audit (7 agents: tools/mcp, app/views, lib/wallet, lib/rpc,
app/models, domain, lib/util) → 22 safe findings, executed as a 6-group
edit→adversarial-review workflow on DISJOINT files. Union gate green
(build 0err / lint 35 / test_parallel 0/371). Landed `cf7bbf05f..525ca1ccc`:

- **Build regression fixed** — `crypto/blake2b.h` had `update*/final` whose `*/`
  closed the doc comment early, spilling text into code; clean build was broken
  (a cached build masked it). (`cf7bbf05f`.)
- **3 MCP NULL-deref crashes** — `onion_health`/`listaddresses`/`profile`
  handlers set `res->error` on malloc-fail but fell through to `snprintf` on the
  NULL body; added the early `return 0`. (`b4b77819d`.)
- **contact.c before_save veto-bypass** — logged the veto then persisted anyway;
  now returns false like every other model. (`b4b77819d`.)
- **views DRY** — `format_zcl_price` + `zcl_format_zcl_short` folded onto one
  exact-integer `zcl_format_zcl_trimmed(…,min_decimals)`; output proven identical
  over a 200M-sample sweep. (`7aabb943c`.)
- **~14 public headers documented** (rpc/util/wallet/models/views). (`525ca1ccc`.)

Adversarial review EARNED its keep again: the audit's "lying `return true` in
hd_keychain/bip44" finding was WRONG — `LOG_FAIL` already expands to
`return false`, so those were success/unreachable paths. Review declined the
`.c` edits; only the accurate doc comments landed. `domain/` returned 0 findings
(already clean) — the safe axis on these subsystems is now harvested.

### New deferred items (surfaced in round 4, not executed)

- **`wallet_generate_hd_key` HD-counter race** (`lib/wallet/src/wallet.c:217-246`,
  *medium risk*) — the internal/external counter is read and used for BIP44
  derivation OUTSIDE the mutex and only incremented inside it; two concurrent
  callers can derive the same key index → address collision. Real fix: read the
  counter (and ideally derive) under the lock. Wallet-threading change — scope and
  test deliberately (not a one-liner; verify no caller holds the lock already).
- **model hook-init DRY** (3 patterns across ~7 models: manual `_init_hooks` +
  static bool, manual `_callbacks_ready`, and the `DEFINE_MODEL_BEFORE_SAVE_READY`
  macro) — consolidate onto one macro. SHAPE, low value, touches many files +
  risks the model-callback lint scaffold; do as its own wave if at all.
- **`mcp minconf` param-spec triplicate** (`wallet_controller.c:283/321/328`) —
  identical INT spec defined 3×; low value, each spec is arguably tied to its
  route entry. Leave unless a wider MCP param-spec table lands.

## Round 5 (2026-06-05) — app/controllers + service glue audited + fixed

Read-only audit (4 agents: explorer/web, wallet-store, ops-net, service glue) →
15 findings (≈5 self-rejected as already-correct/intentional). Executed the real
ones as a 3-group edit→review workflow on DISJOINT files. Union gate green
(build 0err / lint 35 / test_parallel 0/371). Landed `678bf081c..030f16a0e`:

- **Explorer NULL-deref / empty-body fixes** (`678bf081c`) — `explorer_controller_tx.c`
  in_rows/out_rows were malloc'd then iterated with no NULL check (crash on OOM);
  `serve_tx_rpc`/`serve_block_rpc` returned empty bodies on bad input. Added
  guards returning the existing not-found/invalid views (free-before-return,
  no leak). Read-only display path, non-consensus.
- **Controller glue fixes** (`030f16a0e`) — NULL-safe `sqlite3_errmsg` in
  store_mark_order_paid; LOG_WARN on uninit name-DB (RPC reply unchanged);
  LOG_WARN on seed-product save failure; folded the duplicated chain_params
  base58-prefix fetch in wallet_helpers onto one file-local helper.

Adversarial review again EARNED its keep: the audit's `explorer_controller.c`
`socket()`-failure finding was WRONG — `LOG_ERR` already `return -1`s, so there
is no fall-through; the edit was correctly declined.

### New deferred items (surfaced in round 5, not executed)

- **`chain_evidence` dump JSON duplicated** (`event_controller.c:27-74` ≈
  `diagnostics_registry.c:329-370`, *observability-sensitive*) — both assemble the
  same chain-evidence snapshot into JSON; the registry version is the superset.
  Extract one canonical `chain_evidence_controller_dump_state_json(…, bool full)`.
  Deferred: it is the live diagnostics surface and a test may assert the emitted
  shape — verify the JSON is byte-identical before merging.
- **`rpc_call` ≡ `api_rpc_call`** (`explorer_controller.c:254` / `api_controller.c:130`)
  — identical HTTP+RPC+base64 client, differ only in timeout(5s/10s)/log-prefix.
  Merge onto one fn with a timeout/context param. Medium value, touches raw socket
  code — do as its own focused change with a request/response equivalence check.
- **`store_url_decode` vs `url_decode`** (`store_controller.c:326` /
  `wallet_view_helpers.c:550`) — the store version validates hex nibbles, the
  wallet one does not; merging onto the stricter one is a (safe but real) behavior
  change. Low value; only if a shared `lib/util` URL-decode lands.
- **base64 alphabet hardcoded ×3** + **31-site `chain_params_base58_prefix`
  fetch** — both low-value broad sweeps; pick up opportunistically inside a
  relevant wave, not standalone.

**Safe-axis status:** the non-consensus subsystems (tools/mcp, app/views,
lib/wallet, lib/rpc, app/models, domain, lib/util, app/controllers, service glue)
are now harvested — audits return mostly self-rejected noise. Remaining real work
is the deferred §12 consensus-sensitive items, the boot shutdown TU (HIGH-RISK,
SIGTERM proof, do ALONE), the flyclient/MMB extraction, and the owner-gated
peer-scoring enum. Do NOT keep fanning safe audits over the swept subsystems.

## Round 6 (2026-06-05) — never-swept small libs audited + fixed

Read-only audit (4 agents: znam/zslp, metrics/health/event, bloom/core/keys/encoding,
storage/policy/mining) → 17 findings (10 safe, 7 consensus-sensitive). Safe ones
executed as a 4-group edit→review workflow; union gate green (build 0 / lint 35 /
test_parallel 0/371). Landed `2bca9c825`, `594dba893`, `fba7f1f1a`:

- **znam DRY** — dropped `app/models` `is_valid_znam_name` dup, call canonical
  `znam_validate_name`. **event.c DRY** — `payload_is_text` + `format_payload_escaped`
  (byte-identical) + log observer-table-full. **mining** — LOG_FAIL on the
  `mine_block_pow` NULL guard; 6 `printf`→`LogPrintf` in gen.c.
- **~docs** for bloom/core/keys/metrics/znam/zslp public headers (reviewer fixed a
  `decode_secret` doc that wrongly claimed scalar-range validation).
- **Build regression fixed** (`fba7f1f1a`) — `zclassic-cli` failed to link
  (`EncodeBase64` undefined) since the base64 unification; `make all`/`make deploy`
  were broken. Added `utilstrencodings.c` to `CLI_SRCS`.

Adversarial review again earned its keep (caught the missed 6th gen.c printf + the
decode_secret doc lie). 

### Round-6 deferred (consensus/crypto-sensitive — NOT executed)

- **`lib/keys` asserts on the BIP32/secp256k1 path** (`pubkey.c:93-95,130`,
  `key.c:35,40,56,109,114`) — flagged as "disabled in Release". **VERIFIED NOT A
  LIVE BUG**: the production CFLAGS (Makefile:62) define **no `-DNDEBUG`**, so
  `assert()` is ACTIVE (fail-fast abort on violation). Converting to graceful
  error-returns is optional hardening *only if* `-DNDEBUG` is ever added — low
  priority, and a behavior change (abort→return) on a crypto path, so treat as
  consensus-sensitive if ever done.
- **znam/zslp builders return 0 without logging** (`znam.c:147-241`, `slp.c:170-292`)
  — on-chain OP_RETURN encode path; add `log_macros.h` + context. Consensus-adjacent
  (parse/serialize), defer with care.
- **mining PoW BLAKE2b-state duplicated** (`gen.c:38-48` vs `miner.c:223-253`) —
  consensus PoW format built two ways; extract one canonical builder. Repro/verify
  the personalization bytes are identical. Node isn't mining, low urgency.
- **mining `tx_size=250` hardcoded** (`miner.c:116`) — block-template fill uses a
  fixed size estimate instead of `transaction_serialize_size`; can mis-fill blocks.
  Block-construction path; defer.

**Safe-axis status (updated):** with rounds 2–6, every non-consensus subsystem
(util/rpc/wallet/models/views/mcp/controllers/domain + znam/zslp/metrics/health/
event/bloom/core/keys/encoding/mining-logging) is now swept. Audits return mostly
self-rejected noise or consensus-sensitive items. **The safe parallel axis is
DONE.** Remaining real work is all deferred/gated: §12 + the round-4/5/6 deferred
lists + the boot shutdown TU + flyclient/MMB + peer-scoring enum + the live wedge
(operational/owner-gated — see `[[project_live_wedge_rootcause_2026-06-05]]`).

## Deferred — peer_scoring typed-API adoption (needs enum extension, owner-gated)

Round-3 #1 (adopt typed `peer_scoring_record()` across ~34 raw
`peer_misbehaving(...,N,...)` sites in `lib/net/src/msgprocessor*.c`,
`msg_compact.c`) was ATTEMPTED and REVERTED. Two naive approaches both fail:
- **Map by meaning** → silently changes ban WEIGHTS (raw 20→INVALID_MESSAGE=10
  halved; raw 50→INVALID_BLOCK=100 doubled). A behavior change to a
  security-relevant DoS surface — must not land silently. (Caught by review.)
- **Map by weight (1:1)** → preserves behavior but mis-NAMES: a weight-20
  snapshot *parse* error becomes `PEER_OFFENCE_FLOOD` (the only weight-20 enum),
  which is a worse lie than the raw number.

Root cause: the enum has only 4 weight buckets (INVALID_MESSAGE/UNREQUESTED=10,
FLOOD=20, INVALID_HEADER=50, INVALID_BLOCK=100), with no name for the
snapshot/transport/proof rejection classes the snapshot path uses at weights
20/50/100. **The right fix EXTENDS `peer_scoring.h`** with semantically-accurate
offences at the SAME weights (e.g. `INVALID_SNAPSHOT=20`, a 50-weight
swarm-chunk class, `INVALID_PROOF=100` for flyclient/SHA3/merkle verification),
then maps each site to the enum that is BOTH weight-preserving AND honestly
named. That is enum design on a DoS-policy surface → owner-gated; the
`msg_blocks.c:540` dynamic-`dos` site (graded 1..49) needs a parametric record,
not a constant enum. Until then the raw `peer_misbehaving` calls stay — they do
not misrepresent the category.

## boot_services.c decomposition plan (2513 LOC → target the shutdown TU last)

Seam map (read-only audit `w6755v1wu`). Extract order by risk:

**Wave A (SAFE — independent, non-consensus, not in shutdown body):**
- `boot_sd_watchdog.c` (~110) — owns g_sd_watchdog_id/ctx, zero shared state. *(in flight)*
- `boot_node_utilities.c` (~130) — app_add_node, metrics start/stop, sync-state logger. *(in flight)*
- `boot_bg_verification.c` (~60) — bg-validation/hash-verify start/stop; re-checks finalized history, not the connect path. *(in flight)*

**Wave B (consensus-adjacent but NOT in shutdown body — boot-smoke validates):**
- `boot_runtime_sync_services.c` (~200) — header_probe / legacy_mirror / gap_fill /
  zclassicd_oracle / rolling_anchor start/stop wrappers. Stop ordering is preserved
  via the kernel spec table (they are not in the shutdown sequence body). Move
  byte-identical; boot-smoke on a copy.

**Wave C (needs prep first):**
- `boot_frontend_services.c` (~470, biggest payoff) — BLOCKED: shares the profile
  statics `boot_profile_has_explorer/store/onion` (13 read sites incl. stayers) and
  `boot_configure_frontend_rpc` is called from app_init. First promote the 3 trivial
  profile accessors to a shared header + make boot_configure_frontend_rpc public, THEN
  extract.
- `boot_flyclient_mmb.c` (~175, consensus) — BLOCKED: `g_mmb_leaf_store` is an extern
  global shared with boot_snapshot_offer.c + read in app_init's MMB-build block; a lint
  gate asserts msgprocessor_snapshot.c does NOT reference it. Extract only together with
  the MMB-build block, and update the gate's expected-owner file.

**HIGH-RISK (do ALONE, with a real SIGTERM stop/restart proof on a datadir COPY — boot-smoke CANNOT validate this):**
- The shutdown section (boot_services.c lines ~2168-2407): shutdown_stop_frontend_services,
  shutdown_persist_fast_restart_state, shutdown_flush_coins_to_sqlite,
  shutdown_quiesce_network_and_flush_coins, shutdown_persist_runtime_state,
  shutdown_release_owned_resources, app_shutdown_svc. The ordering invariant lives here:
  coins.db COMMIT (emergency flush + quiesce) MUST precede block_index fsync / flat-file
  save / block_tree+node_db close. See [[feedback_at_tip_kill9_ordering_invariant]].
- `boot_catchup_job.c` (~35) — small but `boot_join_catchup_service` is called from
  shutdown_persist_runtime_state, so it touches the SIGTERM teardown. Low payoff, not
  worth the shutdown risk until the shutdown TU is being done anyway.

## Dropped / opportunistic (pick up inside the relevant wave, not standalone)

`gap_fill_service` stale doc + descent swap; `stage_helpers.h` roster comment;
`msgprocessor` seen-ring unification; `sapling_keys` LE32 swap;
`zcashconsensus.h` / coins-decode docs. The `sprout_viewing_key_to_address`
placeholder: doc-only edit is safe; defer the `pk_enc = scalarmult_base(sk_enc)`
wiring as a separately-scoped change (derivation semantics).

## Round 8 (2026-06-05) — lib/net feature transports (UNswept until now)
8 read-only audit agents over the P2P feature/infra surface → 22 findings, 16 real
after vetting (rejected peer_lifecycle:507 — two same-condition ifs feed two
intentionally-distinct counters, a metrics behavior-change not a bug). Fixed in 4
commits (`4d60fc49d..436252ddb`): 5 public-fn NULL-deref guards (zmsg/protocol inv),
3 unchecked byte_stream writes (pong reply, getaddr) that could send a NULL/malformed
frame on OOM, 2 unchecked sqlite3_prepare_v2 in onion directory handlers (stepping
NULL), a torn onion-fetch record (body_len set on malloc fail), 5 ignored
fs_send_frame transmission errors, header docs + a dead fs_send_chunk decl removed.

## Round 9 (2026-06-05) — wide app/ + lib/ non-consensus sweep
16 read-only audit clusters → 47 findings; vetted, fixed in 7 commits
(`2867eb1ee..83ce1dece`):
- **Projection event-skip CLASS BUG** (highest value): wallet/mempool/peers/contacts
  projection catch_up advances last_consumed_offset in-flight but on COMMIT/ROLLBACK
  failure returned without restoring it from persisted meta → next catch_up skips the
  failed events. Fixed to match the correct utxo_projection sibling.
- 1-byte stack overflow (chain_inspect saplingtreeinfo hbuf[hlen]), favicon NULL
  datadir, unbounded strchr→memchr in tx vout parse, uninit health struct ×2,
  unchecked fread/fwrite in file manifest, GLib NULL+nonzero-size UB in wallet_gui,
  no-current-row sqlite read, truncated-blob z-addr encode, store.c const-cast-away,
  2 rpc NULL guards, https ssl/plain read-line fold, ~24 documented wallet/storage APIs.
- DEFERRED: wallet_key.c:562 "incomplete script data loading" — labeled DRY but may
  change wallet spend-path data loading; not batched, needs its own look.

## Round 11 (2026-06-05) — un-audited controller surface (wallet/feature/display)
12 read-only audit clusters over ~45 un-audited controllers → 50 findings; vetted,
fixed in 6 commits (`128b25981..c607eba6f`):
- **Post-broadcast flush correctness bug**: rpc_sendtoaddress / direct / sendmany used
  LOG_FAIL for a wallet-flush failure AFTER the tx was broadcast+relayed → reported
  failure (no txid) for a sent tx (resend risk). Made the flush non-fatal (the comment
  already said so).
- 5 db_wallet_utxo script leaks (rescan/replay/sync/coinanalysis), 3 unchecked
  signature_hash returns on shielded signing paths, swap/blog/mining/dashboard NULL
  guards, unchecked keystore_add_cscript + parse_hash_str, missing RPC error bodies
  (nodelog/dbquery/diagnostics/transaction), memo decode/encode unified (3 divergent
  strategies → 1 helper).
- **Vetting REJECTED** wallet_diagnostic_controller.c:32/40 ("lying return true" — the
  `if(...) LOG_FAIL` already returns false; the `return true` is the success path).
- DEFERRED: the success-path wtx->tx ownership leak — being resolved by a dedicated
  prove-then-fix workflow (the mempool/wallet hold independent deep copies per the
  mempool test, so wallet_commit_transaction also leaks its local mempool_entry).

### Round-11 deferred (low / needs-own-look)
- wallet_key.c:562 "incomplete script data loading" (may change wallet spend-path).
- blog_controller.c:363 silent hex truncation at boundary; wallet_view_shield.c:184
  fragile opid NULL deref; the 4th inline memo block in wallet_shielded_keys.c:259
  (needs a shared public helper to fold — out of single-file scope).
- Consensus-adjacent controllers NOT autonomously fixed: sync_controller*,
  repair_controller*, snapshot_controller*, legacy_import_scan (defensive/doc only,
  repro-on-copy for any behavior change). app/services largely consensus-adjacent too.

### Round-11b — wallet send-path tx-ownership leaks FIXED + a nested-mutex note
Prove-then-fix workflow proved (file:line) that mempool_entry_init / tx_mempool_add_unchecked
/ wallet_add_to_wallet each deep-copy, so freeing the originals is UAF-safe. Fixed
(`...`): wallet_commit_transaction now mempool_entry_free's its local entry, and the three
send handlers transaction_free wtx.tx on success. DEFERRED (pre-existing, unrelated):
wallet_commit_transaction locks mempool->cs (wallet.c:1116) and then tx_mempool_add_unchecked
also locks pool->cs (txmempool.c:281) — a nested acquire of the SAME mempool mutex. The
shipped binary runs, so cs is recursive or this path is uncontended; verify whether cs is a
recursive mutex and, if not, restructure to avoid the re-acquire.

### CORRECTION (2026-06-05) — zcl_mutex is RECURSIVE; two "deadlock" calls were overclaims
zcl_mutex_init sets PTHREAD_MUTEX_RECURSIVE (lib/util/include/util/sync.h:40). Same-thread
re-acquire does NOT deadlock. Two consequences:
- Round-10 `fix(wallet): break keypool deadlock ...` (`2aa7ab00a`) was MIS-CHARACTERIZED: holding
  w->cs across wallet_generate_new_key (which re-locks w->cs) is NOT a deadlock on a recursive
  mutex. The shipped change is still correct and worth keeping — it adds a real concurrent-overshoot
  OOB guard on key_pool[] (two parallel RPC top-ups) and snapshots the default_fee/best_block_height
  data races — but the "deadlock" framing was wrong. NOTE: the restructure RELEASED w->cs between
  iterations, which is what created the concurrent-overshoot window the reviewer then guarded; net
  still safe (tests green) but it was not fixing a hang.
- Round-11b nested-mempool-mutex note: wallet_commit_transaction holds mempool->cs across
  tx_mempool_add_unchecked (which re-locks pool->cs == same mutex). RESOLVED-BENIGN: recursive, no
  deadlock. The wallet_commit_transaction explicit lock/unlock around the call is now REDUNDANT (the
  callee locks internally) — harmless; could be removed for clarity in a later cleanup.
Lesson: zcl_mutex is recursive — do NOT call a nested same-mutex acquire a "deadlock" without
checking sync.h first.

## §12-EXT — Consensus-sensitive findings from the round-12 deep sweep (repro-on-copy each)
Round-12 audited the consensus-ADJACENT surface (app/services, sync/repair/snapshot
controllers, storage glue) under a STRICT bar. Autonomously-safe yield was ~nil (the one
"leak" in block_index_projection.c:381 is NOT a leak — sqlite3_finalize always runs; it is
only a per-event prepare/finalize PERF nit, prepare-once like ins_stmt = a follow-up perf
item). The real value is these REAL, vetted, consensus/security findings — each needs a
datadir-copy repro before any edit; do NOT batch:
- **HIGH header_sync_service.c:575** — `if (verify && tip && verify->nHeight==tip->nHeight) return true;`
  accepts a same-height candidate WITHOUT hash equality (the hash-match case already returned
  at :570). Verified real code. MUST analyze the caller contract: if this gates chain
  acceptance (not just reorg-candidate eval), it could accept a competing same-height block.
  Top priority to investigate.
- **MED bg_validation_service.c:312** — when read_block_undo fails, 4b `continue`s (SKIPS script-sig
  verification) yet the block is reported validated (ok=true). Real gap in the BACKGROUND
  validator's guarantee (not the connect_block consensus path). Should report incomplete, not
  validated, when undo is required but missing.
- **LOW(sev) utxo_recovery_restore.c:126** — system("rm -rf '%s' && cp -a '%s' '%s'") with
  operator-controlled datadir. Real but NOT network-reachable (datadir is a launch arg), so low
  severity; still harden (validate path chars or POSIX copy) for defense-in-depth.
- snapshot_sync_service.c:571 — g_snapshot_anchor read/set without g_snapsync_service_lock while
  snapsync_reset frees it under the lock → potential UAF race with header_sync's anchor walk.
- block_index_loader.c:376 — incomplete nChainTx recompute in the flat-file loader.
- legacy_import_service.c:555 — double-free on the sapling-note import error path.
- coins_view_sqlite.c:1056 — commitment write failure not rolled back (silent torn write).
- disk_block_io.c:231 — stale FILE* cached after ftell failure → wrong read position.
- block_index_db.c:406 — deserialization allows nFile<0 without HAVE_DATA/HAVE_UNDO guard.
- quorum_oracle_service.c:255 — asymmetric agreement counts in the quorum vote tally.
- sync_controller_blocks.c:287 — db_tx_save failure early-returns, skipping batch-state cleanup.
- repair_controller_utxo.c:432 — uninitialized prev_txid_hex read on malformed JSON txid.

### §12-EXT triage (round-12b, read-only, 2 skeptical analysts + adjudicator per finding)
- **header_sync_service.c:575 — REFUTED, NOT a consensus bug** (high confidence, both analysts +
  adjudicator). It is an explicit "sanity gate" for queuing block downloads; the real consensus
  arbiter is connect_block.c:150-163 (block->hashPrevBlock must equal coins_view best, else
  REJECT_FATAL), and syncsvc_collect_needed_blocks skips BLOCK_FAILED_MASK forks. A competing
  same-height fork cannot connect. The "a test would fail" claim was a misread (test_sync_service.c:985
  doesn't exercise line 575). Optional low-priority tidy-up only (require hash eq / delete the tier);
  NOT a v1 item. No repro needed.
- **bg_validation_service.c:312 — CONFIRMED REAL** (high confidence). The BACKGROUND validator
  (-nobgvalidation, advisory — NOT the consensus connect_block path) skips ALL script-sig verification
  for every non-coinbase tx when read_block_undo fails, yet returns ok=true and the caller marks the
  height fully "verified" — violating its documented "verifies all script sigs / failure surfaces as
  BG_VALIDATION_FAILED" contract. Honest fix (in progress): do not claim full verification when script
  checks were skipped for missing undo; surface a skipped-count via validationstatus; never stall the
  validator (post-snapshot blocks legitimately lack rev files, so do NOT hard-fail).

### Round 13 (2026-06-05) — memory-safety subset of §12-EXT, prove-then-fix
Prove-then-fix on 3 deferred memory-safety findings (each proven real + pure-memory-safety +
consensus-neutral + deadlock-free BEFORE editing):
- FIXED legacy_import_service.c double-free on the sapling-note import error path (null after free).
- FIXED snapshot_sync_service.c g_snapshot_anchor UAF race (lock both accessors; non-recursive
  service mutex, no caller holds it → no deadlock).
- SKIPPED disk_block_io.c:231 — proven real but NOT pure-memory-safety (the real issue is in
  callers' cached-FILE* fseek usage, e.g. transaction_controller.c:85); needs a broader caller fix,
  deferred. Prove-gate correctly declined to blind-edit.
NEW latent finding (separate, deferred): legacy_import_scan.c:403 overwrites c->results with
zcl_realloc's return directly — leaks the old buffer on realloc failure and then derefs NULL at :409.
Still-deferred consensus-DECISION §12-EXT items (repro-on-copy/owner): coins_view_sqlite.c:1056
rollback, block_index_db.c:406 nFile guard, quorum_oracle_service.c:255 tally, block_index_loader.c:376
nChainTx recompute, sync_controller_blocks.c:287 batch cleanup, repair_controller_utxo.c:432 uninit read,
utxo_recovery_restore.c:126 system() hardening.

### Round 14 (2026-06-05) — defensive subset of §12-EXT, prove-then-fix (all CLEAN)
FIXED (proven real + defensive-only + consensus-neutral + no deadlock, each reviewed):
legacy_import_scan.c both realloc-into-self OOM NULL-derefs (c->results + ctx->hits twin),
repair_controller_utxo.c malformed-txid uninitialized hex-decode read, sync_controller_blocks.c
db_tx_save-failure batch-cleanup skip, utxo_recovery_restore.c system() datadir char-validation.

### FRONTIER (2026-06-05): autonomous-safe convergence is EXHAUSTED
Every remaining §12-EXT item is a genuine CONSENSUS-DECISION change → needs repro-on-copy
(tools/repro_on_copy.sh) proof + likely owner-gating; do NOT autonomously batch:
- coins_view_sqlite.c:1056 — commitment write failure not rolled back (torn write).
- block_index_db.c:406 — deserialization allows nFile<0 without HAVE_DATA/HAVE_UNDO guard.
- quorum_oracle_service.c:255 — asymmetric agreement counts in the quorum vote tally.
- block_index_loader.c:376 — incomplete nChainTx recompute in the flat-file loader.
Next step for each: read-only prove-or-refute triage (skeptical analysts + adjudicator) FIRST
(header_sync:575 proved this catches false positives), THEN repro-on-copy design only for the
confirmed-real ones.

### Round 15 (2026-06-05) — consensus-DECISION triage (read-only, 2 skeptical analysts + adjudicator each)
NONE of the 4 is a live consensus hazard:
- coins_view_sqlite.c:1056 commitment no-rollback — **REFUTED** (commitment is derivative/optional;
  canonical coins+best_block writes DO roll back; boot coins_reconcile_stale_anchor recomputes the
  commitment from the UTXO set). Optional nit: emit a low-sev event instead of stderr-only. No fix.
- block_index_db.c:406 nFile cast — **REFUTED** (nFile<0 fails closed at every file-open guard
  disk_block_io/process_block_index/blocks_mmap_reader; the ==0 usage harmlessly skips negatives).
  Optional: reject v>INT_MAX at deserialize. No fix.
- quorum_oracle_service.c:255 asymmetric tally — NOT a bug (max() correct for 3 sources) but real
  latent/clarity → **FIXED** (symmetric matrix, value-preserving).
- block_index_loader.c:376 incomplete nChainTx recompute — **REAL but latent/low-impact**: error is in
  the SAFE direction (stale-nonzero nChainTx with HAVE_DATA stays eligible at process_block_core.c:59,
  and connect_block re-validates before commit; the dangerous wrongly-excluded direction is NOT
  produced). DEFERRED: the clean DRY fix (call block_index_forward_pass instead of the 3 hand-rolled
  recomputes at loader.c:376 + boot.c:2138-2143) touches the NEVER-TOUCH frozen config/src/boot.c, so
  it needs an owner decision on how to fix without editing boot.c. Run boot-smoke on a copy before deploy.

### Round 16 (2026-06-05) — outbound persistence adapters (unswept infra)
Defensive+doc audit of adapters/outbound/persistence (11 findings). FIXED: 6 NULL self-guards
in utxo_snapshot_inmem port callbacks, unchecked CREATE TABLE exec in zslp_store_sqlite (port
contract), 3 NULL-after-prepare guards in wallet_backup_store_sqlite. Also pushed the
block_index_db nFile>INT_MAX deserialize guard. **Vetting REJECTED** utxo_snapshot_inmem:370
"script_pubkey leak" — FALSE: revert_tip MOVES the coin_entries (incl. script_pubkey ptrs) into
h->coins then frees only the array; the auditor's coin_entry_release-before-free would be a UAF
when h->coins is later closed. The comment ("now owned by h->coins, don't free") is correct.

### Round 17 (2026-06-05) — crypto/key-derivation frontier (secret-material hygiene)
Defensive audit of domain/wallet (BIP39/derivation), domain/encoding (base58/bech32), lib/crypto
(35 findings). FIXED (output-neutral, KAT-gated, per-cluster adversarial review — test_parallel
0/371 confirms no digest/sig/key/encode value changed): memory_cleanse of secret buffers in
mnemonic (entropy/seed/phrase/salt), base58check decode/encode (xprv/privkey + work buffers),
key_derivation (intermediate private ext_key), HMAC-SHA256/512 (key block + key-hash ctx + inner
digest — backs PBKDF2/BIP39), and ChaCha20-Poly1305 + curve25519 (one-time key, keystream, tag,
ladder state). Public buffers (SHA256d checksum, ext_pubkey, verification intermediates) correctly
NOT cleansed.
**REJECTED — NOT a bug**: sha512.c:114-115 "missing += in message schedule". The last TWO rounds
(K[78]/K[79]) use `w14 +`/`w15 +` instead of `+=`; the expression VALUE passed to Round() is
identical, and those schedule words are never read again (final rounds), so the dead writeback is
harmless — SHA-512 output is correct (KATs pass). Do NOT "fix" it.
DEFERRED (lower-value defensive, not this round): aes256 state cleanse, blake2s_final/sha3 hmac
cleanse, ed25519:277 (signature VERIFICATION — public, no secret, skip), the bech32/base58
attacker-length VLA-stack-exhaustion findings (separate class — needs a length cap that must not
reject valid bounded addresses), mnemonic checksum-bit DRY + buffer-cleansing API doc.

### Round 18 (2026-06-05) — Sapling shielded-key secret hygiene
Secret-cleanse audit of lib/sapling key/note files (25 findings). FIXED 16 (output-neutral,
KAT-gated — test_sapling PASS / 0/371): zip32 FVK fingerprint + serialized-FVK(ovk) buffers,
sapling.c derived ak/nk + h_star/redjubjub/crh_ivk hash contexts, prf sprout_prf/prf_expand
contexts, address sprout viewing-key intermediate, sprout_h_sig context, ff1 AES ctx + PRF/CBC-MAC
intermediates (pq/R/S/c) incl. an early-return path. Public verification keys and the returned
re-randomizer correctly left intact. (The negative-test logs — "validation FAILED", "Poly1305 tag
mismatch", "jub_from_bytes failed" — are EXPECTED reject-path output, not failures.)
DEFERRED: g_esk_ring ephemeral-secret-ring lifecycle wipe (note_encryption.c:54 — needs care about ring usage); the note KDF/value cleanse items below were DONE in round 19.
still output-neutral but re-verify before fixing): note.c value_le buffers (4×, note value),
note_encryption sprout_kdf/sapling_kdf/sapling_prf_ock dhsecret/ovk blocks (3×), g_esk_ring
ephemeral-secret lifecycle wipe, redjubjub_sign digest[64] parity cleanse, sapling_generate_r
memset→memory_cleanse (non-elidable). bn254/bls12_381/circuit/groth16 proof-math files NOT audited
(huge, strictest consensus bar).

### Round 19 (2026-06-05) — remaining note/sapling secret cleanse (prove-then-fix, KAT-gated)
FIXED (each proven output-neutral before edit; test_sapling PASS / 0/371): note_encryption sprout_kdf
/ sapling_kdf / sapling_prf_ock DH-secret/ovk block buffers; note.c value_le note-value buffers in all
5 sprout/sapling plaintext serialize/deserialize paths; redjubjub_sign digest[64] parity cleanse;
sapling_generate_r RNG-buffer cleanse on the failure path. STILL DEFERRED: g_esk_ring global
ephemeral-secret lifecycle wipe (needs ring-usage analysis); the bn254/bls12_381/circuit_gadgets/
groth16 proof-math files (huge, strictest consensus bar — not audited).

### g_esk_ring — REJECTED, not a fix (2026-06-05)
note_encryption.c:54 g_esk_ring is entirely under `#if ZCL_CRYPTO_SANITY` (disabled by default —
ZCL_CRYPTO_SANITY 0), so it does not exist in production builds (the #else path is a no-op inline).
It is a DELIBERATE debug ring that retains the last 64 esk values to detect an RNG-stuck repeat
(two-time-pad guard) and abort(); retention is the feature's whole point — wiping it would defeat it.
Not a secret-hygiene leak. No fix.

### Secret-hygiene sweep status: COMPLETE for tractable items
Rounds 17–19 cleansed all reachable secret-material buffers across lib/crypto, domain/wallet,
domain/encoding, and lib/sapling key/note files (KAT-gated, output-neutral). ONLY remaining secret
surface NOT audited: the zk-SNARK proof-math (bn254/bls12_381/circuit_gadgets/groth16_prover/
sapling_circuit/fr/pedersen/msm/jubjub) — huge, the strictest must-never-fork consensus bar, and the
proving witness/intermediates are the most consensus-critical code. Defer to an owner-gated, repro/KAT
-heavy pass; do NOT churn it for marginal hygiene.

### Round 20 (2026-06-05) — zk-SNARK proving-witness secret hygiene (KAT-gated, capstone)
Audited the proving path (groth16_prover/sapling_prover_c23/sapling_circuit/circuit_gadgets) under the
STRICTEST cleanse-only bar — 16 findings, ALL secret-witness cleanse, ZERO formula/constant findings
(the strict guard held). FIXED (output-neutral; snark_kat/groth16/proof_validate/test_sapling_crypto
all PASS / 0/371): spend+output witness structs (ak/nsk/ar/rcm/rcv/esk), derived nk/ivk, value-commit
randomness rcv, binding signing key, FFT/MSM heap buffers (raw_scalars, a/b/c/h_eval, R1CS witness),
circuit synthesis + gadget bit/byte buffers, AND the witness realloc-grow leak (replaced realloc with
hook-aware alloc+copy+cleanse+free — the gate caught my first attempt bypassing the test OOM hook,
fixed by routing through g_cs_realloc(NULL,...)). The error-diagnostic fprintfs are obs-ok (terminate
via goto cleanup). **The crypto/secret-hygiene sweep (R17–20) is now COMPLETE** across lib/crypto,
domain/wallet+encoding, and all of lib/sapling incl. the prover. bn254/bls12_381/fr field-arithmetic
inner-loop temps deliberately NOT cleansed (not the key; churn). g_esk_ring rejected (disabled-by-default
debug ring). The proof-math FIELD files (bn254/bls12_381/fr) hold only transient field elements — no
standing secret buffer — so no further cleanse target remains.

### Round 22 (2026-06-05) — must-never-fork consensus code (DOC + behavior-neutral memory-safety)
Strict-bar audit of domain/consensus + lib/validation + lib/chain (10 findings). FIXED:
- connect_block.c 3 unchecked zcl_malloc/zcl_realloc → NULL-deref SIGSEGV on OOM; now check + free +
  block_undo_free + REJECT_FATAL("out-of-memory") (non-DoS internal error, matching block_undo_alloc;
  temp-realloc avoids the lost-pointer leak). OOM-only path — valid-block validation unchanged (0/371).
- accept_block_header.c nSolution leak on block_map_insert failure (free before free(pindex)).
- DOC: process_block_revalidate.h now documents the real single-source local-authority pathway (header
  said ">=2 oracles"); script_standard.h extract_destination zero-fill contract corrected.
NOT-A-BUG: the upgrades.h vs coins_math.h "error-code collision" (both use 1301/1302) — domain/consensus
error codes are MODULE-SCOPED (interpreted per call site), so numeric overlap across unrelated modules
is by design, not a contract violation. No renumber (that would change API values). DEFERRED (consensus
-sensitive, repro-on-copy): the few consensus_sensitive findings the audit flagged. **This was the last
unswept surface — the whole codebase is now audited.**

### §12 update_coins OOM — FIXED (2026-06-05, repro-on-copy proven)
coins_from_transaction is now bool (false on OOM/over-cap); update_coins fails the connect on
false instead of silently accepting a block whose outputs were dropped from the UTXO set +
commitment. Behavior-neutral for valid blocks (returns true, identical path). Proven: full suite
0/371 + repro-on-copy boot-smoke floored to the documented 3132299 (no crash, no lower floor =
identical to HEAD). This clears the last actionable §12 consensus-decision item; remainder is
owner-gated (boot.c nChainTx via frozen boot.c, shutdown TU, peer-scoring enum).
