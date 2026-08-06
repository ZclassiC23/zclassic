# S7.2.1 — Metaverse usability and consolidation

## Status

**IN PROGRESS (zclassic23-s7-2-1)**

**Branch:** `lane/s7-2-1-metaverse`

**Base:** current `origin/main` at
`fec2376bff365f32b7111c6f31cfb9f39cacd887`; the two lane commits were
rebased conflict-free from the frozen candidate source base before push.

**Authority:** owner directive dated 2026-08-06. Work is isolated from the
exact-candidate fold, challenger, stable node, canonical datadirs and wallets.
No deploy, restart, promotion, live-port use or heavyweight build is authorized
while the production proof is active. A later owner directive authorizes green
source slices to stay synchronized with and push to `main`; it does not widen
the production-runtime boundary.

## Goal

Refine the existing Sovereign Space and Scout path into a predictable,
idempotent ordinary-user workflow without changing any canonical wire or root
and without adding a second state, identity, storage or transport system.

The audited workflow is:

`space plan -> commit -> show -> publish -> discover -> show -> scout plan -> run -> show`

## Scope

- Existing Space/Scout command definitions and native command handlers.
- Existing metaverse Space/Scout services and canonical `lib/vcs` objects.
- Existing DHT/provider/swarm/policy composition needed by those handlers.
- Existing sovereign-property projection and its adapters.
- Focused Space/Scout/property tests, one hermetic acceptance proof, a
  five-minute quickstart, and a prepared-but-never-started cross-server soak.

## Hard boundaries

- Add at most one public read command: prefer `metaverse.space.status`.
- Preserve every `service_descriptor.v1`, `space_manifest.v1`, mission,
  evidence-map, attestation, DHT record and package root.
- No doorbells, boards, mailboxes, payments, wallet access, remote service
  invocation, executable descriptors, new CAS, database, identity system or
  network stack.
- Scout may hydrate and verify descriptors but may never invoke their verbs.
- READ commands remain literally non-creating on an absent datadir.
- Cross-server soak work is preparation only; it uses scratch datadirs,
  wallet-disabled isolated processes and non-live ports after separate owner
  authorization.

## Tasks

1. Audit every command and reply in the ordinary-user workflow; record exact
   ambiguity, pending-state and idempotency gaps in this document.
2. Add one read-only composed status command with readiness, blockers and the
   next safe command, while exposing no secret, address, policy rule or path.
3. Close discovery state to `present|pending|blocked|not_found|invalid` with
   retryability, phase, bounded progress and safe next action.
4. Hydrate advertised service descriptors during Scout within existing caps;
   root-verify and project protocol roots, read-only verbs, public roots,
   capability roots, evidence, local policy and typed failures.
5. Project Space manifests and service descriptors through the existing
   property catalog, and make corrupt/unreadable objects explicit rather than
   absent or silently omitted.
6. Standardize Space/Scout result fields without changing canonical objects.
7. Add the five-minute quickstart, hermetic two-node acceptance target and the
   prepared cross-server soak harness.
8. Add focused malformed/replay/churn/cycle/clock/cancel/restart/cap tests.

## Verification while the fold is active

Only focused low-impact checks may run. Full lint, sanitizer, uncached suite,
LTO and reproducibility gates are explicitly deferred until the production
proof is banked. Source slices may integrate after their focused checks; no
artifact from them may deploy or promote before the full gates pass.

## Completion

Append coherent slice commits, focused receipts, deferred gates and honest
remaining blockers here. Handoff records the lane and exact commit IDs; the
latest owner direction separately authorizes synchronization and push to main.

## Evidence ledger

### Slice 1 — property/CAS integrity

- Born-red audit: `mv_manifest_read()` returned one boolean for ENOENT,
  unreadable, oversized and malformed bytes. The CONTENT show path therefore
  reported corruption as determined absence, while CONTENT list silently
  skipped it. ZCODE could similarly claim `local_content_hash` without a valid
  matching manifest when its release envelope was unavailable.
- The read path now carries typed `ok|absent|io_error|invalid` results.
  Pagination and source integrity are independent adapter-list facts; catalog
  JSON exposes checked/ok/gap-count/first-reason per kind and in aggregate.
  Missing remains honestly absent, corruption is undetermined, and malformed
  rows cannot disappear behind a plausible empty total.
- The reloadable property island now includes `manifest_read.c` and the ZSLP
  adapter, preventing old/new function-pointer ABI mixing when this slice is
  compiled as one hot-swap unit.
- Focused low-impact receipts while the production fold remains frozen:
  changed production/test translation units pass `-Wall -Wextra -Werror`
  syntax checks; allocation, silent-bool and result-discard ratchets pass;
  `git diff --check` passes; the identity-bound metaverse property island
  compiled and linked on the pre-rebase lane at source id
  `b37477fbbb9abcac578d032ddf187238d51916a71f1aad16fd0bf537bddb26e5`.
  The first island attempt compiled but was correctly refused after isolated
  vendor bootstrap changed its source identity; the stable rerun passed.
  After the conflict-free rebase onto current `origin/main`, every changed
  production and test translation unit passed the same warning-as-error syntax
  gate again using only the vendored SQLite header extracted to a temporary
  directory.
- `metaverse_catalog` execution and the heavyweight full gates remain deferred
  until they can run without competing with the exact-candidate fold.
- First synchronized push attempt was blocked by the remote branch's optional
  `Xrandr.h` compile dependency. Existing self-contained presentation fix
  `6e379786c` was transplanted as `00359de6e`; its vendor-provenance proof
  passes. The next mandatory pre-push gate caught a return-macro mismatch in
  the new typed manifest reader before publication; it is corrected to return
  the exact typed I/O state, and `check_log_macro_return_type` plus the focused
  warning-as-error syntax gate pass.
- The next pre-push run built cleanly and passed lint, then ran 895 cold groups:
  891 passed and four failed only because `test-parallel-fast-active` did not
  build the fixed in-tree `zclassic23-package-verify` helper that those groups
  execute. Both active test runners now own that prerequisite, matching the
  existing public `test-parallel` contract. After building the exact helper,
  fresh focused runs of `test_zcode_verify`, `test_zcode_add`,
  `test_zcode_dev_objects`, `test_build_fabric`, and the changed
  `test_metaverse_catalog` group all pass with zero failures and zero cache
  hits.
