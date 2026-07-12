# Canonicalization backlog — sloppy/version-suffix naming survey

Owner naming standard (restated): **no version-suffix or sloppy names in
CODE** — never introduce or keep `_v1`/`_v2`/`_v3`, `_new`/`_old`/`_ex` as
**public API**. A file **format** may keep a version byte on disk for
backward-read-compat, but the **code** is ONE canonical function versioned
by DATA (a parameter, or the on-disk/wire header byte) — not by a name
suffix. `legacy_`-prefixed modules are the one INTENTIONAL, lifecycle-tracked
exception (see `docs/LEGACY_LIFECYCLE.md`); leave those alone.

This is Lane B of a two-lane sweep: Lane A is separately canonicalizing the
`coins_kv`/`coins_ram` snapshot writers (`*_snapshot_write_v2`/`_v3`) as part
of the v3 shielded-frontier work (see recent `wf/snapshot-shielded-frontier-v3`
merges). This document does not duplicate that — it surveys the **whole
tree** and files it for the record, then gives the **rest of the tree's**
cleanup backlog.

Scope surveyed: everything under the repo root except `vendor/`, `build/`,
`.git/`, `.claude/`. Patterns swept: `_v[0-9]`, `_new\b`, `_old\b`, `_ex\b`,
`_impl[0-9]?`, `_tmp[0-9]`, numbered-duplicate identifiers/files
(`foo2`, `do_thing_2`), and near-duplicate function pairs.

**No renames were made by this survey** — read-only sweep, plan only.

## Totals

| Pattern swept | Unique identifiers matched | Real rename candidates found |
|---|---:|---:|
| `_v[0-9]` | 70 | 0 (all format/protocol-version or test-local) |
| `_new\b` | 36 | 0 (constructor idiom, ported zcash naming, or external lib) |
| `_old\b` | 15 | 0 (Sprout `vpub_old` protocol field, or external lib) |
| `_ex\b` | 13 | **6** (see backlog below) |
| `_impl[0-9]?` | 36 | 0 (static vtable-implementation idiom) |
| `_tmp[0-9]` | 0 | 0 |
| numbered dup (`foo2`, file basenames) | ~15 | 0 (arity-suffixed helpers, algorithm names, test-local) |

**Actionable rename backlog: 6 items**, all in the `_ex` cluster. Everything
else classified as (a) FORMAT/WIRE VERSION or (c) INTENTIONAL — see the
classification tables below for the reasoning per cluster, so this doesn't
get re-litigated later.

---

## Backlog — rank order (blast radius small → large, in order of attack)

| # | Canonical target | Current name(s) | Call sites | Consensus-adjacent? | Proving gate |
|---|---|---|---:|---|---|
| 1 | `coins_view_sqlite_batch_write` | `coins_view_sqlite_batch_write_ex` (the *only* variant — no plain sibling exists; the `_ex` suffix is pure vestige) | 13 | Storage/coins write path, not a consensus **rule** — but touches the UTXO write lane under the LOCK-ORDER LAW. Compiler-checked rename (no signature change). | `make build-only` (catches every call site) + `make t-fast ONLY=storage` (or the coins_view_sqlite test group) + copy-prove on a fixture datadir before calling it done, per the "recovery paths get copy-proven" rule — this file is one hop from `coins_kv`. |
| 2 | `wallet_get_new_address_with_key_id` (or fold `key_id_out` as an optional out-param on the base `wallet_get_new_address`) | `wallet_get_new_address_ex` vs `wallet_get_new_address` | 4 (`_ex`) + 11 (base) | Wallet-only (HD key generation), not chain-consensus. | `make build-only` + `make t-fast ONLY=wallet` |
| 3 | `sapling_keystore_new_address_transactional` (or fold `undo_out` as an optional out-param on the base) | `sapling_keystore_new_address_ex` vs `sapling_keystore_new_address` | 6 (`_ex`) + 25 (base) | Wallet key management, not consensus rules — but Sapling note/undo bookkeeping is concurrency-sensitive (see memory note "Sapling notes concurrency contract"). | `make build-only` + the sapling wallet test group + copy-prove a receive-address-creation flow on a fixture wallet. |
| 4 | `db_sapling_note_mark_spent` should **become** the tri-state (currently `_ex`); the current bool-returning `db_sapling_note_mark_spent` is explicitly documented in-header as "Legacy bool wrapper... do NOT use this in catchup" — rename that one to a `legacy_`-prefixed or `_bool_compat`-suffixed name instead, not the reverse | `db_sapling_note_mark_spent_ex` (tri-state, correct one) vs `db_sapling_note_mark_spent` (bool, doc-flagged legacy) | 5 (`_ex`) + 2 (base) | **Yes** — nullifier/projection catchup over confirmed-chain data. | `make build-only` + `test_parallel` sapling projection/catchup groups + copy-prove a catchup replay on a fixture datadir (the header comment says the bool wrapper "wedged the whole backfill on the first not-our-note spend" historically — treat this rename as also an opportunity to confirm nothing still calls the legacy bool path in a catchup loop). |
| 5 | Same shape as #4: `node_db_sync_sapling_spend` should become the tri-state (currently `_ex`); demote the bool wrapper | `node_db_sync_sapling_spend_ex` (tri-state) vs `node_db_sync_sapling_spend` (bool) | 7 (`_ex`) + 3 (base) | **Yes** — same nullifier-projection subsystem as #4; do #4 and #5 together, one PR, since they're the same pattern in sibling files (`app/models/include/models/wallet_tx.h` and `app/controllers/include/controllers/sync_controller.h`). | Same as #4. |
| 6 | `thread_registry_spawn_joinable` (or fold `out_tid` as an optional out-param on the base `thread_registry_spawn`) | `thread_registry_spawn_ex` (dominant — 48 call sites, more than double the plain form's 7) vs `thread_registry_spawn` | 48 (`_ex`) + 7 (base) | No — lifecycle/thread-registry utility, no chain-consensus or wallet data involved. Purely mechanical, compiler-verified rename. | `make build-only` (48 sites is exactly where the compiler earns its keep) + `make t-fast ONLY=util` or the shutdown/thread-registry test group. Largest blast radius in this backlog but the lowest domain risk — good "prove the process works" first rename if the team wants to warm up on this backlog before #4/#5. |

**Recommended order:** #1 (smallest, single-variant, easy win) → #6 (biggest
diff but safest domain) → #2/#3 (wallet, moderate) → #4+#5 together last
(only pair that's consensus-adjacent — do it as one deliberate PR with a
fixture-replay proof, not folded into a mechanical sweep).

**Naming convention going forward:** when a function grows an "extended"
overload (extra out-param, richer return type), do NOT suffix `_ex`. Either
(a) fold the new parameter into the base signature as an optional
pointer (pass `NULL` at call sites that don't need it — zero-cost, no
second symbol), or (b) if the two variants have genuinely different call
contracts (e.g. "legacy compatibility shim" vs "the real API"), name the
**shim** descriptively (`_legacy_bool`, `_compat`, or the `legacy_` module
prefix if it truly talks to an external `zclassicd`) and give the primary
implementation the **plain, unsuffixed name**.

---

## Classification tables (everything surveyed, not just the backlog)

### (a) FORMAT/WIRE VERSION — keep the version byte/symbol, it's load-bearing

These are byte-for-byte or symbol-name contracts across a process/file
boundary. The version *is* the data; renaming the code identifier would
either break dlopen symbol resolution or silently reinterpret old bytes.

| Cluster | Where | Why it's a real wire/format version |
|---|---|---|
| `struct zcl_hotswap_manifest_v2`, `hotswap_manifest_v2_validate`, `ZCL_HOTSWAP_HOST_STRUCT_SIZE_V2`/`_V3` | `lib/hotswap/include/hotswap/hotswap.h` | `zcl_hotswap_manifest_v2` is the literal **exported data symbol** every generation `.so` must define (`dlsym`-resolved by name). The host struct's v2/v3 split is an `offsetof` freeze so an old-generation `.so` compiled against the v2 prefix stays ABI-safe when the host struct grows. Renaming breaks dlopen. |
| `struct zcl_app_manifest_v1`, `zcl_app_manifest_v1_fn`, `zcl_app_host_v1`, `zcl_app_route_v1`, `zcl_app_route_handler_v1`, `zcl_app_topic_v1`, `zcl_app_request_v1`, `zcl_app_migration_v1` | `sdk/include/zclassic23/app.h` | Same shape as above — this is the **public SDK ABI** for third-party app plugins. The version is the dlopen contract with external code we don't control. |
| `coins_kv_snapshot_write_v2`/`_v3`, `coins_ram_snapshot_write_v2`/`_v3` | `lib/storage/src/coins_kv_snapshot_write.c`, `coins_ram.c`, headers | On-disk `ZCLUTXO` snapshot format version. **Lane A is actively extending this to v3** (Sapling+Sprout+nullifier frontier) per the recent `wf/snapshot-shielded-frontier-v3` merge — do not touch, cross-referenced not duplicated here. |
| `tools/mint_v2_snapshot.c` | tool | Operational tool that mints a v2-format `ZCLUTXO` snapshot specifically (reads `sapling_tree` frontier, writes via `coins_kv_snapshot_write_v2`). Once Lane A's v3 minting path is proven, this may need a v3 sibling or a `--format=` flag — flagged for Lane A, not a rename target for this survey. |
| `HODL_VIEW_DISK_CACHE_MAGIC "zcl_hodl_snapshot_v1"`, `HODL_VIEW_DISK_CACHE_FILE "hodl-current-v1.cache"` | `app/views/src/explorer_pages_hodl.c` | On-disk cache-file magic string, literal file-format tag. |
| `struct persisted_evidence_record_v1` alongside the unversioned `struct persisted_evidence_record` (current, `version` field = 2) | `app/services/src/chain_evidence_persistence_service.c` | **This is the model example of the correct pattern the owner standard asks for.** The canonical struct is unversioned; only the *old, no-longer-written* byte layout keeps a `_v1` name, read only for backward compat (`chain_evidence_state_set_retry` path deserializes both by length). Cite this as the pattern to copy when doing #4/#5 above. |
| `ZCL_Groth16_Batch_v1` domain-separation tag | `lib/sapling/src/bls12_381.c` | Cryptographic domain-separation string baked into a Blake2b transcript — this is consensus-adjacent hash input, must never change bytes; the `v1` is part of the domain tag itself, not a code-naming choice. |
| `MINT_ANCHOR_IN_PROGRESS_KEY "mint_anchor_in_progress_v1"` | `config/include/config/mint_anchor_progress.h` | Persisted `node_state`/`progress_meta` key string — on-disk key name. |
| `not_supported_by_znam_v1`, `append_only_v1` | `app/controllers/src/name_controller.c`, `agent_interface_controller.c` | API response string literals naming the **ZNAM protocol version** / **agent-contract version** they describe — external-facing protocol labels, not internal code names. |
| `is_sapling_v4`, `is_overwinter_v3` | `lib/primitives/src/transaction.c` | These ARE the Zcash consensus transaction version numbers (`tx.nVersion == 3` Overwinter, `== 4` Sapling) — matches upstream zcashd/zclassicd naming verbatim. Renaming would break parity-doc cross-references. |
| `is_valid_onion_v3`, `slice_is_valid_onion_v3` | `lib/test/src/test_onion_bootstrap_slice.c` (+ prod code referenced in comment) | Tor onion address format version — an external protocol's own name ("v3 onion"), not ours to rename. |
| `ip_v4_or_v6` | `lib/storage/src/peers_projection.c` | IPv4 vs IPv6 — external internet-protocol version. |
| `db_mig_seed_v20_wallet_notes_db`, `t_v20_wallet_notes_upgrade_adds_source` | `lib/test/src/test_db_migration_idempotent.c` | Test names mirroring the actual numbered DB **schema migration** (migration #20) they exercise — migrations are legitimately numbered. |

### (c) INTENTIONAL — leave, with the reason on record

| Cluster | Where | Why it's fine |
|---|---|---|
| `cvp_*_impl`, `cvs_*_impl`, `cvdb_*_impl`, `ckv_*_impl` (get_coins/have_coins/get_best_block/batch_write/get_anchor/get_stats, ×4 backends) | `lib/storage/src/coins_view_{sqlite,projection,db,kv}.c` | Standard C "static implementation assigned into a vtable field" idiom — `.get_coins = cvs_get_coins_impl` etc. File-scoped statics, never called by name outside their own translation unit. Not a version scheme. |
| `check_block_impl`, `check_block_header_impl`, `contextual_check_block_header_impl` | `lib/validation/src/check_block.c`, referenced as "legacy" from `core/consensus/` | Same impl-vs-public-wrapper idiom, currently mid-port to `core/consensus/`; the `core/consensus/` headers call the old monolith "legacy" in prose comments but the symbol itself isn't `legacy_`-prefixed (it's a private static). No action — this is the known, tracked refactor-in-progress (`docs/REFACTOR_STATUS.md`), not sloppy naming. |
| `coins_view_cache_modify_new` alongside `coins_view_cache_modify` | `lib/coins/include/coins/coins_view.h` | Not an "old vs new" successor pair — two **different semantics** ("modify an existing coin" vs "modify/create a brand-new coin"), directly mirroring upstream zcashd's `CCoinsViewCache::ModifyCoins` / `ModifyNewCoins`. C23-only-port-with-attribution doctrine says keep upstream-parity names. |
| `vpub_old`, `vpub_new`, `k_vpub_old`, `k_vpub_new`, `total_vpub_old`, `total_vpub_new` | Sprout JoinSplit handling (primitives/validation) | These are the **Zcash protocol's own field names** for the Sprout JoinSplit value-pool deltas (`vpub_old`/`vpub_new` in the Zcash protocol spec and zcashd source). Not a code-history leak. |
| `SSL_new`, `SSL_CTX_new`, `X509_new`, `EVP_PKEY_new`, `EVP_CIPHER_CTX_new`, `gtk_*_new`, `sqlite3_prepare_v2`, `sqlite3_open_v2`, `sqlite3_wal_checkpoint_v2`, `sqlite3_trace_v2`, `dup2`, `PBKDF2`, `OP_PUSHDATA2`, `AVX2` | Various — these are **external library/syscall/opcode APIs** (OpenSSL, GTK, SQLite, POSIX, Zcash script opcodes, x86 ISA) | We don't own these names; they're called verbatim as documented by the library/spec. |
| `eh_solver_new`, `bi_new`, `pindex_new`, `privkey_make_new`, `r_new`, `idx_new`, `cap_path_new`, `node_db_topup_fill_new`, `clear_new`, `modify_new` | Various local variables / one static helper each | `_new` here is the common C **constructor/allocation idiom** (`foo_new()` returns a freshly built object) or a plain local-variable qualifier ("the newly computed X"), largely mirroring bitcoin/zcash reference variable names (`pindexNew`). Not a successor-naming pattern — no coexisting non-`_new` predecessor of the same object exists to be confused with. |
| `sql_query_row_i64_2` / `sql_query_row_i64_3` | `app/controllers/include/controllers/explorer_internal.h` | Suffix is **arity** (2-column vs 3-column result row helper), not version — same pattern as `printf`/`scanf` family naming, not a `_v[0-9]` match (it's `_2`/`_3` directly, deliberately not `_v2`). No confusion risk. |
| `run_gate_script_with_env2`, `ace_hash2`, `h_echo2`, `build_file_cb2` | `lib/test/src/test_make_lint_gates.c`, `test_active_chain_extend.c`, `test_mcp_router.c`, `lib/codeindex/src/codeindex_build.c` | File-scoped `static` test/build helpers distinguishing "the second callback/env-count/handler variant used later in the same file" — zero public-API exposure, zero cross-file callers. `run_gate_script_with_env2` even has a doc comment: "Like run_gate_script_with_env but exports TWO env vars." Cosmetic at most; not worth a backlog slot given zero blast radius. |
| `offer_v0/v1/v2`, `manifest_v0..v3`, `bmanifest_v0..v3`/`bmanifest_bad_v0`, `pass_v1/v2/v3`, `hsim_call_v0/v73`, `hsim_catalog_v0/v73`, `g_call_v0/v73`, `g_catalog_v0/v73`, `zcl_test_uss_v2` | `lib/test/src/test_net.c`, `test_hotswap_simnet.c`, `test_cookie_rotation.c`, `test_utxo_snapshot_loader.c` | All **local variables inside single test functions** capturing sequential values of a real monotonic counter/generation API under test (e.g. `offer_v0 = msg_processor_offer_cache_version()` then asserting `offer_v1 > offer_v0`), or simulated hotswap "generation 0" vs "generation 73" route stubs. Correctly named for what they represent; no public surface. |

---

## What was NOT found

- No file-basename numbered duplicates (`foo2.c` beside `foo.c`, `foo_v2.c`
  beside `foo.c`) anywhere in the tree outside the already-covered
  `mint_v2_snapshot.c` / `test_snapshot_shielded_v3.c` (both format-tied, see
  above).
- No `_tmp[0-9]` matches at all.
- No `TODO`/`FIXME` comments self-flagging a rename/dedupe/consolidation
  debt (`grep -i 'TODO.*duplicate\|rename\|consolidat'` across `.c`/`.h` — 0
  hits).
- No near-duplicate function **pairs** beyond the six `_ex` cases in the
  backlog — the `_impl` and constructor-`_new` clusters are single-owner
  idioms, not "two versions of the same thing" pairs.

## Gate run for this doc

`make lint` — docs-only change, ran clean (see commit below for the exact
gate output referenced in the branch).
