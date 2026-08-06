# S7.2.1 — Metaverse usability and consolidation

## Status

**IN PROGRESS (zclassic23-s7-2-1)**

**Branch:** `lane/s7-2-1-metaverse`

**Base:** `13d0d255b09548a21ccd05936186f8efc849394a`

**Authority:** owner directive dated 2026-08-06. Work is isolated from the
exact-candidate fold, challenger, stable node, canonical datadirs and wallets.
No deploy, restart, promotion, merge, push, live-port use or heavyweight build
is authorized while the production proof is active.

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

Only focused low-impact groups may run. Full lint, sanitizer, uncached suite,
LTO and reproducibility gates are explicitly deferred until the production
proof is banked. Nothing from this lane may merge or push before those gates
can run on the integrated tree.

## Completion

Append coherent slice commits, focused receipts, deferred gates and honest
remaining blockers here. Handoff is the lane branch plus exact commit IDs; it
is never a push.
