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
