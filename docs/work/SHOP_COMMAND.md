# `zclassic shop` — one-command sovereign storefront

Owner-approved 2026-08-09 (after external review). Goal: run one command,
get a live private shop: stable onion identity + storefront + wallet +
content + discovery + payment config, ending with a printed "your shop is
live" verification. Compose existing primitives; never duplicate them.

## What already exists (do not rebuild)

- `app.store.*` — the canonical shop: products, per-order one-time Sapling
  z-address + binding memo, plan/commit pay, hash-verified atomic collect,
  token-gated downloads, `/store` web surface (onion-only POSTs), and a
  `<datadir>/store/products.json` provisioning loader
  (`app/controllers/src/store_controller_schema.c:94-102`).
- Onion hosting + `/directory.json` with an `apps` row consumers already
  parse (`lib/net/src/onion_service.c:597-606`).
- Worker-mode precedent for future isolation: same-image self-respawn
  (`app/services/src/build_fabric_worker.c:65-74`,
  `lib/util/src/spawn.c:128-259`) + `os_sandbox_enter()`
  (Landlock/seccomp/rlimits, `lib/platform/src/os_sandbox_linux.c:1165`).
  Shop design must not foreclose restricted wallet/onion/content worker
  modes later — same LTO binary, OS-enforced authority.

## Slices (in order)

### A. Persistent onion identity (the load-bearing primitive)

Today dynhost mints an EPHEMERAL service every start
(`lib/net/src/tor_integration.c:50-52,140-147`); `tor_write_torrc`
writes SocksPort+DataDirectory only (`:116-138`);
`read_onion_from_hostname_file()` (`:185-194`) reads a hostname file
nothing configures. Slice: opt-in persistent HiddenServiceDir under the
datadir (e.g. `-onion-persist` or auto when a shop is initialized),
ephemeral stays the default for non-shop nodes. Native surface to show
the stable address + a rotation path. The explorer dump
(`ops state --subsystem=explorer` → `data.state.onion_address`) must
report the persisted address across restarts. Tests: identity stable
across two boots, rotation changes it, ephemeral default unchanged.

Landed 2026-08-09: `-onion-persist` (opt-in; default stays ephemeral)
mints or reuses a seed-backed identity in
`<datadir>/tor_data/onion_service/` and installs it as the dynhost
service, so the explorer dump's `onion_address` is stable across
restarts; `-onion-rotate` (requires `-onion-persist`) archives the old
identity and logs the old+new addresses. Test group:
`test_onion_persistence`.

### B. `app shop init` / `app shop status` (the orchestration)

New native branch `app.shop` (rows in `config/commands/app_features.def`
or a new store-adjacent def, following existing patterns).

`app shop init` (plan/commit confirm idiom — it mutates):
1. Tor armed and real (not the stub) — else refuse with the exact
   `make tor-full` remedy.
2. Persistent onion identity ensured (slice A), address printed.
3. Wallet custody: encrypted at rest (credential or interactive
   passphrase path) — refuse with the named credential recipe, never
   silently mint plaintext keys on the canonical lane.
4. Store schema ensured; optional `--input` products.json provisioned
   through the existing loader.
5. `shop` app id published in the node's `/directory.json` apps row.
6. Print the verification block: onion URL of `/store`, product count,
   wallet state, discovery state, and the exact next command for a
   buyer to find it.

`app shop status` (read): the same verification block on demand, plus
each unmet prerequisite named with its remedy (never a silent partial).

Landed 2026-08-10: `app.shop.init` (READY_COMMAND, plan/commit) and
`app.shop.status` (READY_READ) in `config/commands/store.def`, handlers in
`app/controllers/src/shop_native_handler.c` with the datadir-local
probe/provision half in `shop_native_probes.c`. Commit refuses by name on
a non-encrypted wallet (WKS1/WKD1 envelope probe) before the Tor check,
and on the stub-Tor build; it ensures the slice-A identity, copies
`--input` products.json to `<datadir>/store/products.json`, runs
`store_ensure_schema` against the live `<datadir>/node.db`, and announces
via the new `<datadir>/directory/apps.csv` (ONION_DIR_EXTRA_APPS_REL),
which lib/net's register_self() folds into the node's own
`/directory.json` apps row each round. Test group: `test_shop`. (Fix
2026-08-10: the plan's commit instruction rides in `commit_input` /
`commit_command` data fields — a self-referential `next[]` entry made the
envelope's push_next_array drop the whole bare-plan reply to an empty
RESPONSE_BUDGET_EXCEEDED; both leaves declare ZCL_COMMAND_LIST_BUDGET for
remedy-string headroom, pinned by a registry-level budget regression.)

### C. `app shop reputation` (provable facts only)

Fold the existing commons projection per publisher: reproductions,
DISTINCT SIGNING IDENTITIES (never the word "independent" unless
independence is actually established), days observed, dependent
packages, paid fulfillments (when patronage settle lands), availability
challenge pass rate (when the challenge loop lands). Rule: never claim
more than the cryptography proves.

### D. Buyer-posted needs (follow-on, designed-for now)

No bounty/want-ad exists (`bounty` grep: zero). Closest shape:
`zswap_ads` (signed gossip ad → SQLite projection → browse endpoint) —
a "wanted" ad is the same shape with reversed terms. Shop surfaces must
leave room for a buyer-side request board; do not build it in A–C.

## Constraints

- ZC23 issuance stays simulation-only.
- No global moderation authority; shop curation rides the per-node
  moderation layer (already shipped).
- Defensive rules: AR lifecycle, LOG_FAIL, zcl_malloc, error bodies on
  every native handler.
- Every slice: focused tests + `make lint` + pre-push CI before push.
