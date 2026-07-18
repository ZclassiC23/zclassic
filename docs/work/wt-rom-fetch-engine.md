# wt-rom-fetch-engine — ROM fetch engine (the CLIENT side of ROM delivery)

> **Status: IN PROGRESS (wt-romfetch)** — lane branch `lane/rom-fetch-engine`,
> worktree `.claude/worktrees/wf_rom-fetch-engine`. Scope: NET lane only.
> Do **not** touch `core/chainparams` (the unmerged `lane/rom-keystone`
> keystone bake owns that file; see Conflicts below).

## Mission

Build the missing **client** half of [`docs/ROM_DELIVERY.md`](../ROM_DELIVERY.md):
a fresh node pulls the two-builder-verified consensus-state bundle
(`consensus-state-bundle-3056758.sqlite`, 513 MB, utxo_root == compiled
checkpoint `5817f0ec…` @ 3,056,758) from any seeding peer in minutes instead of
paying the ~15–20 h from-genesis fold. The **serving** side is already on main;
this lane is the fetch half.

## Recon verdict (file:line evidence, read on 2026-07-18)

**What the serving side serves today:**

- Artifact registry + digests: `lib/net/src/rom_seed.c` — registration
  re-derives `whole_sha3`, per-chunk `chunk_sha3[129]`, and `chunk_root`
  (SHA3 over concatenated per-chunk digests) from disk bytes
  (`rom_seed_register`, rom_seed.c:171-331). Chunking: 4 MB
  (`ROM_SEED_CHUNK_SIZE`, lib/net/include/net/rom_seed.h:34); a 513 MB bundle
  is 129 chunks. Registry cap 8 artifacts, artifact cap 4 GB.
- Wire protocol: a ROM chunk request rides the existing file-service transport
  (direct TCP, default port `FS_PORT` 18034, SHA3-CTR session keyed off an
  all-zero utxo_root — see server side `fs_handle_client_fd`,
  lib/net/src/file_service.c:984-986). Request = one `FS_REQUEST` frame whose
  39-byte body is `["ROM"(3)][chunk_root(32)][chunk_index(4 LE)]`
  (`FS_ROM_REQUEST_SIZE`, lib/net/include/net/file_service.h:187-189; parser
  `fs_parse_rom_request`, lib/net/src/file_service.c:674-689).
- Serve path: `fs_serve_rom_chunk` (lib/net/src/file_service.c:929-976) —
  free tier (no payment, no PoW gate), bounded by the `rom_seed` per-peer
  concurrency + per-peer/global byte-rate caps; answers with
  `fs_send_chunk_fast` = RAW stream `[4-byte size LE][data][32-byte MAC]`
  where `MAC = SHA3(key || send_counter || chunk_sha3 || data)`
  (lib/net/src/file_service.c:346-378). On refusal it sends an `FS_DONE`
  *frame* instead (file_service.c:935,941,946,954,960,969).
- Discovery: price-0 `file_offer` gossip (`rom_seed_build_offer`,
  rom_seed.c:664-681 → `file_market_add_offer`; gossip handler
  `handle_zfilelist`, lib/net/src/msgprocessor.c:761-) AND the onion
  `/directory.json` `artifacts` array
  (`rom_seed_directory_json`, rom_seed.c:683-720; wired at
  lib/net/src/onion_service.c:565-570) carrying
  `{kind, digest=chunk_root, whole_sha3, size, chunk_size, chunks}`.
- Operator surface: `ops.debug.rom_seed.{status,enable,disable,artifacts}`
  (config/commands/ops.def:318-380, app/controllers/src/rom_seed_controller.c).

**What is missing (the whole fetch side):**

- No code anywhere sends an `FS_ROM` request: `FS_ROM_REQUEST_SIZE` is
  referenced only by the server parser (grep: file_service.c + file_service.h
  only). No `rom_fetch` module exists (`lib/net/src/rom_fetch*` absent;
  `rom_fetch` appears only in comments/registry strings).
- No fetch-side manifest handling: nothing parses the `/directory.json`
  `artifacts` array or validates a downloaded file against a committed
  `whole_sha3`/`chunk_root`.
- No download driver (chunk loop, `.part` staging, resume, multi-seeder
  parallelism), no `ops.debug.rom_fetch.*` commands, no `dumpstate rom_fetch`
  subsystem (the name is reserved in comments — rom_seed_controller.c:20 —
  but unregistered).
- HANDOFF.md:132-134 confirms: "`lane/rom-fetch2` — the ROM **fetch** side …
  NOT done."

## Trust model (inviolable — do not create a third activation door)

The fetch engine is an **untrusted-transport downloader**. It:

1. **commits to digests before fetching** — `(chunk_root, whole_sha3, size)`
   come from operator input or a discovered manifest; they are the only
   values carried into verification;
2. downloads chunks (transport-MAC-verified per chunk, fail-closed);
3. after the whole file lands, re-hashes it: per-chunk SHA3 fold ==
   `chunk_root` AND whole-file SHA3 == `whole_sha3` AND size == manifest —
   any mismatch **unlinks the file, no partial trust**
   (docs/ROM_DELIVERY.md:20-23);
4. hands the verified path to the **existing** installer
   (`-install-consensus-bundle=PATH` →
   `boot_install_consensus_bundle`, config/src/boot_install_consensus_bundle.c:231)
   whose RECEIPT / CHECKPOINT_CONTENT authority is the only activation door.
   This lane never calls the activate path itself, never touches consensus
   validation, and never installs bytes the operator did not commit to.

Note: per-chunk content digests are NOT on the wire today (the serve side
binds the true per-chunk SHA3 into the chunk MAC; the client learns it only
by hashing the received bytes). V1 therefore verifies content at whole-file
granularity — exactly the documented trust model. A per-chunk digest manifest
message is a future serve-side addition, out of this lane's budget.

## Implementation plan

1. **`lib/net/include/net/rom_fetch.h` + `lib/net/src/rom_fetch.c`** (this
   session):
   - `struct rom_fetch_manifest` — committed `(chunk_root, whole_sha3,
     filename, size_bytes, chunk_size, num_chunks)`;
   - `rom_fetch_parse_directory(json, out, max)` — pure, bounded parser of the
     `/directory.json` `artifacts` array (peer-supplied: every field
     range-checked, sizes consistent with `ROM_SEED_*` bounds);
   - `rom_fetch_chunk(peer, port, chunk_root, idx, buf, cap, out_sz)` — one
     verified chunk over `fs_session` (handshake with zero utxo_root, matching
     the server; `FS_REQUEST` "ROM" frame; raw `[size][data][MAC]` read with
     timeouts; MAC checked against `SHA3(key||recv_counter||sha3(data)||data)`);
   - `rom_fetch_verify_file(path, manifest)` — one streaming pass: size,
     per-chunk fold → `chunk_root`, whole-file → `whole_sha3`;
   - `rom_fetch_download(peer, port, manifest, out_dir, cb, ctx)` — sequential
     V1 driver: chunks → `<out_dir>/<filename>.part` (pwrite at offsets),
     verify, atomic rename to `<out_dir>/<filename>`; digest mismatch unlinks
     the `.part`. No threads owned — the caller drives.
2. **Test** `lib/test/src/test_rom_fetch.c` + `X(rom_fetch)` in
   `lib/test/src/test_parallel.c`: directory parse (valid/malformed/bounds),
   verify_file (pass, wrong whole digest, wrong chunk_root, short file,
   multi-chunk fold) — all on `mkdtemp()` fixtures, no network.
3. **Next session (not done yet):**
   - loopback E2E test: `fs_server_start` on a mkdtemp datadir +
     `rom_seed_register` a synthetic bundle + `rom_fetch_download` from
     127.0.0.1 — proves the client against the real serve path;
   - `ops.debug.rom_fetch.*` native commands (status/fetch with explicit
     `--peer/--root/--whole-sha3/--size` commit input; later a discovery mode
     over `file_market` offers / onion directory) + `dumpstate rom_fetch`
     per the "Adding state introspection" convention;
   - resume (per-chunk presence/SHA3 spot-check like
     `fs_client_sync`'s skip logic), parallel multi-seeder chunk scheduling,
     progress counters.
4. **Explicitly out of scope:** `core/chainparams/*` (keystone bake conflict —
   the unmerged `lane/rom-keystone` commit 767bd652f owns
   `core/chainparams/src/checkpoints.c`; this lane takes the expected digest
   as input instead of baking it), any change to the merged serve path, any
   install/activation call, consensus validation semantics.

## Verification

- `make build-only -j32`: green (rom_fetch.o compiled into the node objects).
- `build/bin/test_parallel --only=rom_fetch`: PASS (3/3 test fns — manifest
  sanity, directory parse, whole-file verify vs `rom_seed_register` digests
  incl. all fail-closed mismatch paths).
- `build/bin/test_parallel --only=rom_seed`: PASS (3/3 neighbor groups —
  rom_seed, rom_seed_policy, rom_seed_ledger — unbroken).
- No live-network fetch in this session; the E2E loopback test is step 3.
