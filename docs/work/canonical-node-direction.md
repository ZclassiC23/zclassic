# ZClassic23 → Canonical Node: direction & roadmap

Owner direction (2026-06-17): make the single-binary C node solid enough to
become the owner's **canonical zclassic node**. Four strands, one invariant.
Researched + adversarially verified by the `zcl23-strategic-direction` workflow.

## The invariant (the MUST-MUST-MUST, the floor everything stands on)

zclassic23 stays **bit-for-bit consensus-compatible with `zclassicd`**. A
consensus change NEVER ships to zclassic23 first. Enforced by E13 lint gate
`check-consensus-parity` (mechanism) + `test_consensus_parity` goldens (values)
+ the live `utxo_parity` cross-check vs co-located zclassicd. Everything below
layers **on top**: P2P/RPC/memory/crash/perf hardening, wallet/features,
tooling, metadata — **never a validity rule.** Litmus test for any change: if it
isn't one of those, it doesn't ship. Run `make lint` + `make ci` after each.

Verified-clean separation: consensus resolves ONLY inside the E13-scanned
surface (`lib/consensus lib/validation lib/chain lib/mining app/jobs
domain/consensus`); MCP/REST controllers reach it only through RPC indirection.

## Beta7 finding (settles "what must we match?")

`ZclassicCommunity/zclassic` PR #132 "Beta7 daemon candidate" + the whole series
**#121–#131 contain ZERO consensus changes** — mechanically verified (no
`src/consensus/`, `src/primitives/`, `src/pow`, `src/coins`, `src/chainparams`;
zero E13 banned tokens). It's hardening + features. The real future consensus
targets are *other* open PRs (#134 checkpoint-at-header, #135 ZIP-209 mainnet
turnstile, #138 IBD contextual checks) — **track only; mirror ONLY after
zclassicd releases them network-wide.**

| Beta7 PR | Verdict |
|----|---------|
| #122 mempool-once | **PORT NOW** — the one real C-node P2P gap (`lib/net/src/msg_tx.c:process_mempool` has no per-peer guard) |
| #123 deser overflow guards | **PORT NEXT** — audit `lib/primitives`/serialize for size_t-overflow + end-of-data guards; replay-canary-green BEFORE tightening any bounded predicate |
| #121 flush crash-safety | **VERIFY/PORT NEXT** — confirm coin-DB flush fails loud (AbortNode-equiv), not assert |
| #124 RPC/HTTP hardening | **AUDIT-ONLY** — already present (10 MiB body cap, JSON depth 256, constant-time auth); batch-cap MOOT (no batch surface — do NOT add one) |
| #127 perf | **MOSTLY N/A** — SQLite not leveldb; only check onion/REST workqueue depth |
| #125/#128 -addnode | **LATER** — outbound peer policy |
| #126/#129/#130/#131/#132 | **FEATURE BACKLOG** — reimplement on our MVC+MCP layer (coin-control send, wallet perf, native ZSLP/NFT indexer), default-off + off-chain, NOT ported C++ |

## Roadmap

**NOW** (non-consensus, low-risk, high-payoff):
1. ✅ **100% C on GitHub** — `.gitattributes` + purge the stray `.js` (shipped `c47a59075`).
2. **Port the once-per-peer `mempool` rate guard** → `lib/net/src/msg_tx.c:process_mempool` (S). The one genuine beta7 P2P-DoS gap.
3. **Enable the replay-canary timer** on the canonical host (S) — the from-genesis "history validates identically" replay is the strongest parity proof but is opt-in; only scheduling is missing.

**NEXT** (hardening + the parity-depth gap + arch foundation):
4. Audit deserialization paths for PR#123-class overflow/end-of-data guards (M) — *reject only impossible sizes, never a real-chain object* (the h=478544 125811-byte-tx lesson).
5. Confirm coin-DB flush fails loud (S).
6. **Close the one material parity gap (MVP C8):** add a zclassic23-format SHA3 UTXO-commitment RPC to the C++ reference daemon, then flip `utxo_reference_source_zclassicd` to `exact=true` (L). Today the live diff proves block-hash agreement, not byte-exact UTXO-set agreement.
7. Liveness Condition on the parity observer — page if the check goes silent or drift latches (M).
8. **Unified service registry** — one declaration per service → RPC + MCP + REST-CRUD (L). Today REST is GET-only (`api_controller.c:497`); this is the foundation for the AI-as-interface vision and must precede per-service CRUD.

**LATER** (feature surface + AI):
9. Beta7 feature backlog on the MVC+MCP layer: coin-control send (#126), wallet perf (#129), native ZSLP/NFT indexer + marketplace (#130/#131/#132) — default-off, IBD-guarded, off-chain (OP_RETURN/memo only).
10. **In-node AI agent (AI is the interface, not a GUI wallet)** — LLM strictly optional, off the onion, behind the same RPC indirection so it can never touch consensus.
11. Verify `-addnode` precedence over bootstrap seeds (#125/#128, S).
12. Widen E13 PATHS to `app/controllers`+`app/services` as a defense-in-depth ratchet (stays green today).

## Risk ledger
- Parity is the big one: #4 deser guards must reject only arithmetically-impossible sizes — replay-canary-genesis green before tightening any bounded predicate.
- #6 exact-SHA3 RPC goes on the C++ *reference*, never zclassic23 consensus; its byte layout MUST match the pinned `test_consensus_parity` goldens.
- Do NOT add a JSON-RPC batch surface just to cap it (#124) — that adds attack surface.
- NFT/ZSLP overlay (#9) stays default-off, IBD-guarded, rate-limited, off-chain.
- #12: never relax the FORBIDDEN regex to pass.
- Net consensus impact across the entire roadmap: **NONE.**

## Discipline
No Docker. CI is local (`make test`/`make ci`). Normal FF pushes to
`git@github.com:ZclassiC23/zclassic.git` — never flatten. `rm -f
build/bin/zclassic23` + verify `build_commit` before any deploy (stale-.o
footgun). Copy-prove on a datadir COPY before any live mutation. Never weaken a
gate (honest messengers stay).
