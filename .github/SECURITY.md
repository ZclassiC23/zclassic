# Security Policy

## Supported status

ZClassic23 is **pre-v1 and in active stabilization** — it is not
production-ready, and there are no supported release lines yet. Only the
current `main` branch receives fixes. Do not rely on this build as a
mainnet node until the v1 acceptance criteria in
[`docs/MVP.md`](../docs/MVP.md) are met.

Known soft spots are stated plainly in the README status section and in
the audit disposition below (for example: off-chain ZMSG P2P messages
are currently plaintext on the wire).

## Safety and integrity model

ZClassic23 is security-sensitive full-node software, not offensive-security
tooling. The repository contains Tor, wallet/key handling, P2P networking,
native operator commands, fuzzers, and crash harnesses because those are required to
run, inspect, and harden an operator-owned node.

The safety boundary, scanner context, local gates, command authorization, release
integrity checks, and reviewer checklist are documented in
[`docs/SECURITY_AND_INTEGRITY.md`](../docs/SECURITY_AND_INTEGRITY.md).

## Automated review of pull requests

Every pull request — including from forks — is automatically security-reviewed
before it can be merged, by two workflows in `.github/workflows/`:

- **`pr-security-review.yml`** runs `tools/scripts/pr_security_scan.sh` over the
  PR diff: consensus divergence from `zclassicd`
  ([`docs/CONSENSUS_PARITY_DOCTRINE.md`](../docs/CONSENSUS_PARITY_DOCTRINE.md)),
  supply-chain execution (fetch-and-run / decode-and-run / dynamic load / remote
  installs / new submodules + workflows), committed secrets, and dangerous C
  calls. A **HIGH** finding fails the check and blocks the merge.
- **`pr-security-comment.yml`** posts the verdict as a friendly PR comment.

This is intentionally **fork-safe**. The scan runs on the `pull_request` trigger
with a **read-only token, no repo secrets, and never executes PR code** (it only
diffs and greps), so a hostile PR has nothing to steal. The comment is posted by
the separate `workflow_run` workflow in the trusted base context, which likewise
**never runs PR code**. We do **not** use `pull_request_target`, which would
expose secrets to untrusted code. The pass/fail check is the gate and works on
every PR regardless of origin.

## Reporting a vulnerability

Please report vulnerabilities **privately** via GitHub security
advisories on
[ZclassiC23/zclassic](https://github.com/ZclassiC23/zclassic/security/advisories/new),
rather than filing a public issue. If you cannot use advisories, contact
the maintainer privately.

There is no bug-bounty program.

## Past audits

A third-party security and cryptographic audit was received and triaged
in June 2026. The point-by-point disposition record — what was fixed,
what was refuted (with citations), and what is deferred and tracked — is
[`docs/work/security-audit-response-2026-06-09.md`](../docs/work/security-audit-response-2026-06-09.md).
