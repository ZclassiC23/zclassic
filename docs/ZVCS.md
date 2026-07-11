# ZVCS — the in-binary version-control system

ZVCS is zclassic23's own content-addressed version-control system,
implemented in `lib/vcs/` (Apache-2.0, `Copyright 2026 Rhett Creighton`). It
is not a git wrapper and does not shell out to git: `git` stays outside the
binary entirely, as the sha1-based GitHub publish bridge that lives under
`tools/scripts/` (source-tree publishing, PR review, the human-facing history
this repo already has). ZVCS is the layer *inside* the binary that turns "the
agent edited files and the dev loop went green" into a durable, self-hashing
commit — without ever invoking a subprocess.

This document is the canonical description of what ZVCS is, what it stores,
and what actually calls it today. See also
[`docs/work/HOTSWAP.md`](work/HOTSWAP.md) §"ZVCS auto-anchor" and §"Core
refusal" for how the dev loop integrates it, and
[`docs/adr/0002-sealed-consensus-core.md`](adr/0002-sealed-consensus-core.md)
for why the sealed `core/` tree and ZVCS's seal guard exist together.

## Why an in-binary VCS

Two invariants motivate keeping a VCS inside the node binary instead of
depending on the operator's git install:

1. **Sovereignty.** The node is meant to run unattended, potentially on
   hardware that never has git installed. An agent driving the dev loop
   needs a durable record of "what source tree produced this passing
   verdict" that does not depend on an external toolchain. ZVCS supplies
   that record using only stock libc and the in-tree SHA3-256
   (`lib/crypto/src/sha3.c`).
2. **"Code fearlessly, not recklessly."** A sealed-path change (see
   ADR-0002) must be structurally impossible to auto-commit. Git has no
   concept of a sealed path; ZVCS's seal guard (`lib/vcs/src/vcs_seal.c`) is
   built into the commit path itself, so a snapshot that would touch a
   sealed file is refused (`VCS_REFUSED`, exit code `3`) unless a one-shot
   unseal token is present.

The **hard gate** that keeps this true is
`tools/scripts/check_vcs_no_git.sh` (wired into `make lint` as
`check-vcs-no-git`): it fails the build if any file under `lib/vcs/`
references the word `git`, or calls `exec*`, `execve`, `system(`, `popen(`,
or `fork(`. `lib/vcs/` is sovereign by construction, not by convention — a
regression that adds a `system("git ...")` call anywhere in that directory
turns the build red.

## The model: a flat, path-sorted manifest — not git's blob/tree DAG

Git's tree object is a recursive directory DAG: one tree object per
directory, nested arbitrarily deep. ZVCS deliberately does **not** do that.
A ZVCS commit points at exactly **one manifest**: a flat, path-sorted list of
every tracked file (`lib/vcs/include/vcs/vcs_manifest.h`):

```
[1  version]
[8  entry_count]
repeated entry_count times, in ascending path order:
  [2  path_len][path bytes (no NUL)][4 mode][8 size][32 blob]
```

The reason is the diff primitive it buys: "what changed between two
snapshots" becomes a **linear merge-join** of two sorted manifests
(`vcs_manifest_diff()`) — one pass over both lists, no tree recursion, no
recursive object fetches. That is the O(n) primitive an agent (or the
dev-loop status check) actually needs, and it is simple enough to read,
grep, and step through in `gdb` (Law 3: "if gdb cannot step it, it is not
beautiful").

`tree_hash` — the manifest's own content address — is:

```
tree_hash = SHA3(0x22 || concat over sorted entries of
                 SHA3(0x21 || path || 0x00 || mode_le || size_le || blob))
```

## SHA3-256 with domain tags, everywhere

Every hash ZVCS computes is SHA3-256 (the same in-tree FIPS-202
implementation the sealed `core/` manifest and the node's other integrity
checks use — `lib/crypto/src/sha3.c`), and every hashed preimage starts with
a one-byte **domain tag** so that a blob, a manifest entry, a manifest, and a
commit can never collide even if their raw bytes happen to match
(`lib/vcs/include/vcs/vcs_object.h`):

| Tag | Value | Meaning |
|-----|-------|---------|
| `VCS_TAG_BLOB` | `0x20` | raw file content |
| `VCS_TAG_ENTRY` | `0x21` | one manifest entry (path/mode/size/blob) |
| `VCS_TAG_MANIFEST` | `0x22` | a serialized path-sorted manifest |
| `VCS_TAG_COMMIT` | `0x23` | a commit preimage, addressed by `commit_id` |
| `VCS_TAG_SEALSET` | `0x24` | the sealed-path set commitment |
| `VCS_TAG_ANCHOR` | `0x25` | reserved for a later wave (anchor binding) |

Tag values are **permanent**: once assigned, a tag is never reused for a
different object kind — that is what makes the object store's addressing
collision-proof by construction rather than by convention.

Every object read **recomputes** the hash and rejects a mismatch
(`vcs_object_get()`); ZVCS never trusts a stored hash without re-deriving it.
The same "recompute, never trust" discipline used elsewhere in this codebase
(`seal_kv`, the sealed `core/MANIFEST.sha3`).

## The three stores

A ZVCS repo lives beside `.git/` in the working copy, at
`<repo_root>/.zvcs/`:

- **`objects/`** — the sharded content-addressed object store
  (`lib/vcs/src/vcs_object.c`). Objects live at `objects/<2-hex>/<62-hex>`
  where the 64-hex name is the object id. Writes are atomic (tmp file,
  fsync, rename) and idempotent — putting already-present content is a
  no-op. Two addressing modes exist: content-addressed (`vcs_object_put`,
  used for blobs and commits — the id *is* the hash of the tagged content)
  and explicitly-addressed (`vcs_object_put_addressed`, used for manifests,
  which are addressed by their *structural* `tree_hash` rather than the raw
  serialized bytes — the reader re-derives and checks the address on load).
- **`commits.log`** — an append-only, self-verifying commit log. This is not
  a bespoke format: it is `lib/storage/event_log.c`, the same durable,
  fsync'd, CRC'd fact log the chain reducer itself uses, with one new event
  type, `EV_VCS_COMMIT = 25` (`lib/storage/include/storage/event_log.h`).
  Each commit record is a fixed-layout struct serialized to
  `VCS_COMMIT_RECORD_BYTES` (496) bytes and appended verbatim.
- **`index.kv`** — a dedicated SQLite WAL file, one handle per repo, one
  writer, writes framed in `BEGIN IMMEDIATE`
  (`lib/vcs/include/vcs/vcs_index.h`). This is deliberately **outside** the
  node.db ActiveRecord lifecycle, following the same kernel-store doctrine
  as `progress.kv` (`storage/progress_store.h`) and `seal_kv`: a small
  dedicated single-writer store below the AR layer, marked with the
  `// raw-sql-ok:vcs-index-kernel-store` lint-gate exemption. Every row in
  `index.kv` is **derived** — regenerable from the worktree plus
  `commits.log` by `vcs_index_rebuild()` ("recompute, never repair"). Tables:
  `stat_cache` (mtime/size/ctime → blob hash, the warm-path cache that lets
  `vcs_status`/`vcs_manifest_build` skip re-reading unchanged files),
  `refs` (currently just `HEAD`), `seal_pin` (the currently-accepted
  sealset hash), `anchor` (per-commit generation/verdict binding), and
  `meta` (free-form key/value).

## The commit record

A commit is a self-hashing, fixed-layout, little-endian struct
(`lib/vcs/include/vcs/vcs_commit.h`). The canonical preimage, in order:

```
[4   version]
[32  parent]              (all-zero for the first commit)
[32  tree_hash]            the manifest this commit points at
[32  sealset_hash]         SHA3 commitment over sealed-path entries
[32  generation_sha256]    the binary generation this cycle produced
[4   verdict_status]       0 = passed dev-cycle; nonzero = not passed
[24  phase]                e.g. "resident_commit", "transactional_reload"
[8   elapsed_ms]
[32  failure_hash]
[64  agent_id]             from ZCL_AGENT_ID
[64  session_id]           from ZCL_SESSION_ID
[128 task_ref]             from ZCL_TASK_REF
[8   committed_at]         unix seconds
```

```
self_sha3  = SHA3(preimage)              (self-verify discipline, seal_kv-style)
commit_id  = SHA3(0x23 || preimage)      (the object-store address)
```

The record (`preimage || self_sha3`) is appended to `commits.log` **and**
stored as an object addressed by `commit_id`, so `vcs_object_get(commit_id,
VCS_TAG_COMMIT)` round-trips it. `vcs_commit_deserialize()` deliberately
distinguishes "hard parse failure" (bad length/version → `false`) from "the
record parses but its self-hash doesn't recompute" (`true`, `*self_ok =
false`) — a reader can step past a corrupted record instead of aborting the
whole log walk.

`generation_sha256` is what binds a source snapshot to the *binary* it
produced: a raw SHA-256 digest of the exact hot-swap `.so` (`artifact_sha256`
from `zcl_agent_hotswap`) or the exact reload generation
(`candidate_sha256` from the `zcl.agent_dev_deploy.v1` state file), decoded
by `vcs_devloop_hex32_decode()`. A `check` cycle (docs-only, no build) has no
generation to bind, so it commits with an all-zero `generation_sha256` —
the source snapshot still lands, honestly labeled as build-less.

## Auto-anchor: every green dev cycle gets a commit

`lib/vcs/src/vcs_devloop.c` is the one piece of glue between the dev loop
and ZVCS: `vcs_devloop_anchor_cycle()`, called from
`tools/dev/devloop_cycle.c:finish_cycle()` after every `zcl.dev_cycle.v1`
verdict — whether that cycle resolved to a Tier-1 hot-swap
(`resident_commit`), a native reload (`transactional_reload`), or a
docs-only `check`. No "remember to commit" step exists; a passing cycle
*is* a ZVCS snapshot.

This call site is **fail-open by design**, stated directly in the header
comment: a ZVCS failure here must never fail the dev cycle or crash the
loop. `finish_cycle` still returns `passed`/`rejected` exactly as it would
without ZVCS; the verdict JSON gains one extra field —
`"vcs_commit":"<64-hex commit id>"` on success, or `"vcs_error":"<message>"`
on failure. The one status that is surfaced loudly (not just logged) is
`VCS_DEVLOOP_ANCHOR_REFUSED` — a sealed-path snapshot refusal — which adds
`"vcs_sealed_refusal":true`. As of this wave that refusal is **advisory
only** at this integration point: by the time `finish_cycle` runs, the
cycle's own hot-swap/reload publish has already happened, so a refused
anchor means "this cycle's source snapshot did not land," not "the running
binary was blocked." The pre-publish refusal that actually blocks the
running binary lives earlier in the pipeline — see ADR-0002 and
`docs/work/HOTSWAP.md` §"Core refusal."

The very first call on a fresh `.zvcs/` walks the whole tracked worktree
once (logged via `LOG_INFO`); every call after that tracks only the change
set, because the object store dedupes unchanged blobs and the `stat_cache`
skips re-reading files whose mtime/size/ctime match the cached row.

## Seal integration — a snapshot cannot silently touch consensus

`lib/vcs/src/vcs_seal.c` is ZVCS's own sealed-path guard, independent of (but
aligned with) the physical `core/` seal described in ADR-0002. On every
`vcs_snapshot()`:

1. Load the sealed glob set — from `.zvcs/sealed_paths` if present, else the
   compiled default: `core/`, `domain/consensus/`, `lib/consensus/`,
   `lib/validation/`, `lib/chain/`, `lib/mining/`, `app/jobs/`.
2. Compute `sealset_hash` — `SHA3(0x24 || concat over the bytewise-sorted
   entry hashes of every manifest entry matching a sealed glob)`. Sorting
   makes it order-independent and stable.
3. Compare against the pinned `sealset_hash` in `index.kv`'s `seal_pin` row.
   Unchanged (or no pin yet, i.e. the first snapshot) → `VCS_SEAL_OK`. Changed
   with a valid one-shot unseal token authorizing exactly that new sealset →
   the token is **consumed** and the snapshot proceeds. Changed with no valid
   token → `VCS_SEAL_REFUSED`, which propagates out of `vcs_snapshot()` as
   `VCS_REFUSED` (exit code `3`).

v1 unseal is a one-shot token (`vcs_seal_grant_unseal()` /
`VCS_SEAL_TOKEN_KEY`); the header marks a v1.1 upgrade to an ed25519 owner
signature as future work, so consent cannot be forged by a token an agent
could mint itself.

## Revert semantics — forward-only, never rewrites history

`vcs_revert()` does **not** rewrite `commits.log` or move `HEAD` backward.
It:

1. Loads the target commit's manifest and diffs it against the current
   worktree manifest (a merge-join, same primitive as `vcs_status`).
2. Rewrites every file that differs and deletes every tracked file absent
   from the target, atomically (`write_file_atomic`: tmp file, fsync,
   rename).
3. Records the restoration as a **new forward commit** — `task_ref` is
   stamped `revert:<first 16 hex chars of the target commit id>` — so the
   append-only log gains one more entry rather than losing any.

`relink_generation` (rebinding the *running binary* to the reverted
generation, not just the source tree) is **not wired in v1** (tracked for
Wave 3.3): passing `true` still performs the source revert and forward
commit, then returns `VCS_ENOTIMPL` to signal that the relink half did not
run. Callers that only need the source-tree revert can ignore the
`ENOTIMPL` and treat the commit as landed.

## Module map (`lib/vcs/`)

| File | Owns |
|------|------|
| `vcs.c` / `vcs.h` | The façade: `vcs_open`/`vcs_close`, `vcs_snapshot`, `vcs_status`, `vcs_log`, `vcs_revert`. Composes everything below. |
| `vcs_object.c` / `vcs_object.h` | The sharded content-addressed store under `.zvcs/objects/`. |
| `vcs_manifest.c` / `vcs_manifest.h` | The flat path-sorted manifest: build from the worktree, serialize/parse, `tree_hash`, merge-join diff. |
| `vcs_commit.c` / `vcs_commit.h` | The self-hashing commit record: serialize/deserialize, `commit_id`. |
| `vcs_index.c` / `vcs_index.h` | The derived `index.kv` SQLite WAL store (stat cache, refs, seal pin, anchor, meta) + `vcs_index_rebuild()`. |
| `vcs_seal.c` / `vcs_seal.h` | The sealed-glob guard: glob loading/matching, `sealset_hash`, unseal-token grant/check. |
| `vcs_devloop.c` / `vcs_devloop.h` | The dev-loop ↔ ZVCS glue: `vcs_devloop_anchor_cycle()`, fail-open by design. |
| `vcs_walk.c` / `vcs_walk.h` (private) | Tracked-set traversal shared by the manifest builder and index rebuild: the ignore predicate (`.git/`, `.zvcs/`, `build/`, `vendor/lib/`, `*.db`, `node.db*`, `test-tmp*`) and per-file blob hashing. |
| `vcs_priv.h` | Private helpers shared across the `.c` files (hex encoding, etc — not part of the public API). |

Test coverage: `make t ONLY=vcs_core` covers the object store, manifest,
index, and seal guard in isolation; `make t ONLY=vcs_devloop` covers the
dev-loop glue (`vcs_devloop_anchor_cycle`, hex decoding, and the
sealed-refusal path). Both are registered in `lib/test/src/test_parallel.c`.

## The `dev vcs` / MCP surface — planned, not built

**As of this writing there is no command-line or MCP surface for ZVCS.**
`config/commands/*.def` (the native command registry — see
[`docs/NATIVE_COMMAND_INTERFACE.md`](NATIVE_COMMAND_INTERFACE.md)) declares
no `vcs` branch or leaf in `root.def`, `core.def`, `apps.def`, `ops.def`, or
`dev.def`, and no controller under `tools/mcp/controllers/` exposes a
`zcl_vcs_*` tool. The only caller of `lib/vcs/` today is the internal
`vcs_devloop_anchor_cycle()` glue described above — there is no way for an
operator or agent to run `vcs status`, `vcs log`, or `vcs revert` directly
yet; those verbs exist in the façade (`vcs.h`) but are not wired to any
transport. Adding that surface (a `dev vcs` registry branch and/or a
`zcl_vcs_status` / `zcl_vcs_log` MCP tool) is future work, not a documented
current capability — do not assume it exists when writing runbooks or other
docs.
