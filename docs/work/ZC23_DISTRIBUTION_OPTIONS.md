# ZC23 distribution — options for the owner decision (2026-08-09)

Phase C1 of [`MARKETPLACE_NEXT.md`](./MARKETPLACE_NEXT.md). This document
**presents options; it decides nothing.** C2 is the owner picking among
them. Everything below stays inside the frozen covenant
([`ZC23_LIVING_COMMONS.md`](./ZC23_LIVING_COMMONS.md)): no live token, no
GENESIS/MINT/SEND, no payout, no consensus change until the owner
explicitly promotes — and the issuance promise is already fixed:

> Every policy-valid atomic unit of ZC23 exists because the public C23
> commons grew, was tested, was independently reproduced, or was preserved.

Owner questions this answers: (a) do we call it Proof of Participation?
(b) how is ZC23 distributed? (c) can I earn some for putting z23
code on it?

## 0. What is already fixed (not options)

- ZC23 is a **ZSLP token** on the ZCL chain — application layer, zero
  consensus surface. ZCL remains the fuel (mint/send fees).
- Issuance is **creation-backed**: units exist only against evidence
  (creation, test, reproduction, preservation) — never for staking,
  hashing, or holding.
- **ZCODE Score** (nontransferable evidence score) and **ZCODE Credit**
  (local reciprocity quota) already exist and stay separate from the
  transferable token.
- v1 issuance is **owner-reviewed plan/commit** — a human signs every
  mint batch; automation proposes, the owner disposes.

## 1. Name: "Proof of Participation"?

| Option | Meaning | Trade-off |
|---|---|---|
| **A. Creation-backed issuance** (current covenant language) | Units mint against commons-growth evidence | Accurate, no confusion with PoS/PoW; dry |
| **B. Proof of Participation** | Same mechanics, friendlier public name | Invites "participation = show up" misreading; must always be qualified ("participation = verified contribution") |
| **C. Proof of Contribution** | Same, sharper | Closest honest name; less catchy |

Recommendation: **C as the technical name, B allowed as marketing** —
"Proof of Participation (verified contribution)". The mechanism is
identical either way; this is a naming decision, not a protocol one.

## 2. Distribution model

| Option | How units reach people | Trade-off |
|---|---|---|
| **A. Evidence-scheduled emission** (recommended) | A public schedule maps evidence classes to weights: new package > reproduction > review/repair > preservation. A daily/weekly batch proposes mints from the verified evidence graph; owner commits. | Fully auditable against the covenant; schedule changes are public and versioned. Slower to bootstrap. |
| **B. Fixed genesis + patronage pool** | Mint a fixed supply at genesis to a transparent commons treasury; distribution is commissioning/patronage flows from the pool. | Simple accounting, but genesis mint violates "exists because the commons grew" unless the pool is provably locked to future evidence. |
| **C. Hybrid** | Small bootstrap pool (option B) for commissioning, with all ongoing emission by evidence (option A). | Pragmatic; needs the pool's lock rules written before any mint. |

## 3. "Can I earn ZC23 for putting z23 code on it?"

Yes under every option — that is the core case. Concretely, under 2A:

- **Publish** a policy-valid C23 package (exact retrievable source, tests)
  → creation evidence → the largest weight.
- **Independently reproduce** someone else's package (the O5 three-party
  protocol in [`ZC23_REPRODUCTION_RUNBOOK.md`](./ZC23_REPRODUCTION_RUNBOOK.md))
  → reproduction evidence → second weight. This is what stops self-minting
  spam: unverified publication earns Score, not ZC23.
- **Review, repair, port, preserve** → smaller weights.
- **Hosting/serving** (keeping packages available, the swarm) earns ZCODE
  Credit (reciprocity quota), not ZC23 — hosting is not creation. (Option:
  a preservation-evidence class for long-horizon availability proofs —
  flagged as an open sub-decision.)

Self-dealing guardrail to decide at C2: does owner-published code earn at
the same weights as everyone else's? (Recommendation: yes, same evidence
rules — the covenant already says human and AI contributors use the same
factual rules; the owner is not excepted.)

## 4. Supply

| Option | Shape | Trade-off |
|---|---|---|
| **A. Uncapped, rate-limited** | Emission bounded per epoch by schedule, no hard cap | Matches an ever-growing commons; inflation transparency required |
| **B. Hard cap with taper** | e.g. 21M-style cap, emission weights decay per epoch | Scarcity story; late contributors get less for equal work |
| **C. Cap = commons-size function** | Cap derives from measured commons growth (packages × reproductions) | Most covenant-faithful; hardest to explain |

Recommendation: **A with a public per-epoch budget** in v1 (simplest to
audit, matches the owner-reviewed batch flow), revisit at scale.

## 5. The v1 mechanics (already built, simulation-proven)

The simulation lane is done (`make metaverse-verify` members MM4/MM8):
patronage plan/fund/settle/refund with proof-conditioned release,
continuity epochs, commons attribution — all CAS-stored, simulation-only.
Going live = swapping the simulated receipt store for ZSLP mint/send
behind the same plan/commit commands. That swap is the C3 implementation
plan, written only after C2.

## 6. What C2 must actually decide (the short list)

1. Name: A / B / C (§1).
2. Distribution: A / B / C (§2); if hybrid, the bootstrap pool size and
   lock rules.
3. Evidence weights per class (§3) — first schedule can be one line each.
4. Supply: A / B / C (§4); if A, the per-epoch budget number.
5. Owner self-dealing: same rules, or abstain (§3 guardrail).
6. Preservation/hosting: Credit-only, or a ZC23 preservation class (§3).
