# ZC23 portable reproduction runbook

Status: simulation and protocol acceptance only. This procedure does not
authorize or perform ZC23 genesis, minting, payout, custody, wallet access,
deployment, service changes, or live-datadir access.

## What this proves

The Living Commons reproduction path has three roles:

- **A — requester** prepares the exact public package and canonical challenge.
- **B — reproducer** starts from roots, fetches the public inputs, rebuilds in
  confinement, and signs the exact reproduction work receipt with B's existing
  contributor/ZID-bound worker key.
- **C — observer** starts from roots, fetches both sides, independently reloads
  every canonical object, applies the approved-reproducer policy, and rebuilds
  the Commons projection.

Canonical semantic wires travel as ordinary one-chunk `content.v2` objects.
Package manifests and chunks travel through the existing package store and
`zpkgswm`; signed provider records and root-only discovery use the existing
ZCODE DHT. There is no second CAS, transport, worker ledger, identity system,
or database of truth.

The local protocol acceptance is:

```sh
make zcode-reproduction-acceptance
```

It composes exact permanent groups for the three-process policy path, real
`zpkgswm` frames, signed DHT discovery, corruption refusal, provider fallback,
restart/resume, cancellation races, challenge expiry/replay bounds, and
byte-identical projection rebuild. Its honest verdict is:

```text
distinct_signer_simulation=true
approved_fixture_policy=true
actual_off_host_credit=false
```

Separate processes, workspaces, package stores, and signing keys on one
physical machine prove protocol separation, not physical independence. They
never clear the independent-reproduction unit or the real SHA3 gate.

## Requester A: public challenge material

A commits the simulation-only reproduction request to an explicit isolated
scratch workspace. The request binds the exact task, candidate, package,
release, recipe, dependency lock, toolchain capsule, reference build report,
output manifest, fixed reproduction action, challenge nonce, budgets,
confinement grade, requester binding, and expiry.

A may publish only root-addressed public material:

- the package manifest and every package chunk;
- the canonical request and referenced public evidence wires;
- the reference build report and expected artifact manifest;
- the simulation policy candidate and approved-reproducer set.

A must not export wallet data, private keys, API or SSH credentials, canonical
datadir paths, absolute source paths, environment secrets, or mutable database
rows. A root without its verified bytes is not evidence, and a DHT/provider
record is a discovery hint rather than object truth.

## Reproducer B: exact confined rebuild

On a genuinely separate owner-approved machine, B obtains the package root,
package-store path, lock root, reference report, and empty output/work
directories through an authenticated out-of-band handoff. B then runs the
existing verifier directly:

```sh
build/bin/zclassic23-package-verify "$PACKAGE_ROOT" \
  --store="$PACKAGE_STORE" \
  --emit="$OUTPUT_DIR" \
  --lock-root="$LOCK_ROOT" \
  --reproduce-against="$REFERENCE_REPORT" \
  --work="$WORK_DIR" \
  --require-full-isolation
```

Exit zero proves that this invocation re-derived the package under full local
confinement and produced a canonical build report byte-identical to the
reference. It does **not** by itself prove that B is approved, distinct, or
off-host, and an unsigned build report earns no independent-reproduction
credit.

B must next bind the exact request root, reproduced build-report root and
artifact-manifest root in the existing signed `work_receipt.v1` REPRODUCE
action, using B's already-approved contributor/ZID-bound worker key, and place
that receipt in the existing proof-set path. The repository does not yet ship
one end-to-end remote command that performs this receipt/admission handoff;
until that small adapter exists, the genuine off-host gate remains blocked and
must not be cleared by copying a fixture key or self-reporting a hostname.

## Observer C: independent admission

C starts with semantic roots and ordinary discovery records, fetches the
canonical bytes and package, and re-derives all roots. C must independently
reload and validate the task, candidate, proof policy/set, PROVEN lane, Score
receipt, request, reference and reproduced reports, artifact manifest, package
release/recipe/lock/capsule, contributor bindings, approved-reproducer set,
policy candidate, challenge time/epoch, and active fixture branch.

Admission fails closed on missing bytes, altered reports, an unapproved or
related signer, stale/replayed challenge, wrong task/candidate/policy, partial
package, corrupt provider, contradiction, reorg, or duplicate event. Only an
owner-approved second host plus the signed receipt/admission path may set the
future off-host evidence fact. Protocol code does not infer physical
independence from process IDs, paths, network addresses, or operator claims.

C may then derive only the simulation-only shadow attribution and epoch and
rebuild the local projection from canonical CAS objects. The projection is a
cache, not authority. All resulting surfaces continue to report
`token_exists=false`, `funds_moved=false`, `custody_used=false`, and
`genesis_gate_satisfied=false`.
