export const meta = {
  name: 'os-o1-hardening',
  description: 'Sync-independent OS hardening: encrypt the P2P wire, adaptive DDoS puzzle, error-discipline ratchet, authority-receipt idiom',
  phases: [
    { title: 'Harden', detail: '4 isolated worktree lanes: Noise transport, adaptive client-puzzle, silent-error ratchet, authority-receipt idiom' },
  ],
}

const LANE_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['branch', 'verdict', 'files_changed', 'summary', 'gates'],
  properties: {
    branch: { type: 'string' }, verdict: { type: 'string', enum: ['MERGE_READY','NEEDS_WORK','BLOCKED'] },
    files_changed: { type: 'array', items: { type: 'string' } },
    summary: { type: 'string' }, gates: { type: 'string' }, caveats: { type: 'string' },
  },
}

const COMMON = `zclassic23 is one C23 binary, a full ZClassic node, consensus-parity with zclassicd. This workflow lands SYNC-INDEPENDENT OS-strength hardening (all unit/hermetic-testable now, no synced ledger needed). The OS is already strong in most dimensions (supervision tree, 42-condition self-heal, malloc/AR/clock lint gates, ~90 dumpstate subsystems, crash-only boot invariants, wallet-at-rest encryption) — do NOT re-open those. Design recipes exist under docs/work/os/.

HARD RULES: consensus parity inviolable (no tx-validity / PoW-consensus / Equihash change — the DDoS puzzle is an ANTI-ABUSE admission gate on request surfaces, NOT block PoW); every malloc via zcl_malloc, every error return LOG_*, every write via AR lifecycle; files < 800 lines (app/+config ENFORCED); every thread supervised; NEVER touch a live datadir / the mint producer / stop zclassicd. If your worktree cannot link, FIRST: cp -a /home/rhett/github/zclassic23/vendor/lib vendor/. You are in your OWN isolated git worktree — commit to THIS branch only; do NOT push/merge/touch others. Self-gate: make build-only + focused test group (make t-fast ONLY=<group>) + make lint (+ agent_impact_rules.def per changed .c; reconcile DOC-COUNTS / EXPECTED_DIAGNOSTICS_DUMPERS if you add a dumper). Final message = structured data, technical; report the branch name.`

phase('Harden')

const lanes = await parallel([
  () => agent(`${COMMON}

LANE O1-a: encrypt the P2P wire (Noise transport, TOP PRIORITY). The P2P wire is currently PLAINTEXT. Noise Phase-0 is already BUILT + TESTED (lib/session/src/noise_handshake.c + session_transport.c: Noise_XX handshake + ChaCha20-Poly1305 record layer) but never WIRED into the node — lib/net/src/v2_transport.c does not exist and there are zero non-test references to v2_transport/v2_enabled. Follow docs/work/os/A4-noise-transport-p1.md: create lib/net/src/v2_transport.c (+ header) that wires the Noise handshake + record layer onto the P2P connection path, negotiated/opt-in so it LANDS DARK — cleartext byte-parity is preserved for un-negotiated peers (no consensus/wire-format break for existing peers). Prove with: a socketpair byte-parity test (encrypted round-trip delivers identical plaintext both directions), a handshake KAT (deterministic transcript), and a p99 latency bench showing acceptable overhead. Files: lib/net/src/v2_transport.c/.h + the minimal P2P connection hook (the connman/peer path) + a focused test group. Do NOT touch consensus or message semantics — only the transport framing. Wire it as opt-in (a flag/negotiation), default-off if the recipe says land-dark, so no behavior change for the network yet.`,
    { isolation: 'worktree', model: 'opus', label: 'O1-a:noise-transport', phase: 'Harden', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE O1-b: adaptive client-puzzle / DDoS admission (A6). Two request surfaces are abusable: app/controllers/src/store_controller_pow.c uses FIXED 20-bit difficulty with a hand-rolled 4096-slot replay ring (no load response), and the P2P SNAPSHOT_REQUEST path (lib/net/src/fast_sync.c fast_sync_build_offer) is UNGATED by PoW so a flood triggers the O(n) UTXO-root fallback per request. Per docs/work/os/A6-adaptive-client-puzzle.md: extract lib/net/puzzle.{c,h} — a reusable adaptive client-puzzle (server-issued rotating challenge, single-use ring, EWMA load-adaptive difficulty bits) generalizing the existing gate in fast_sync.c:642-880 (fast_sync_pow_gate). Then make store_controller_pow.c use the adaptive puzzle (not fixed 20-bit), and add a PoW admission gate in front of the SNAPSHOT_REQUEST O(n) root build. This is an anti-abuse ADMISSION gate, NOT block/consensus PoW. Files: lib/net/puzzle.c/.h (new) + store_controller_pow.c + the fast_sync.c snapshot-request gate + a focused unit test (challenge issue/verify, single-use replay reject, difficulty rises under load, falls when idle). Keep edits to fast_sync.c minimal + additive so it does not conflict with the Noise lane's connman hook.`,
    { isolation: 'worktree', model: 'opus', label: 'O1-b:ddos-puzzle', phase: 'Harden', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE O1-c: error-discipline ratchet. tools/lint/silent_bool_errors_baseline.txt carries 55 grandfathered sites that return a bool/-1 error WITHOUT logging context (check_silent_bool_errors.sh is RATCHET, not yet HARD). Reduce the baseline: pick a batch of the 55 sites (PRIORITIZE files NOT under lib/net/ or lib/session/ — those are owned by the Noise + DDoS lanes this round — and NOT wallet/* which another workflow may touch), add the proper LOG_FAIL/LOG_ERR/LOG_NULL/LOG_RETURN context to each offending return, remove those entries from silent_bool_errors_baseline.txt, and confirm the gate still passes with the smaller baseline. Do the same for a few one-result-type sites if cheap. This is a pure, mechanical, behavior-preserving refactor — do NOT change control flow, only add the missing log-and-return context the macros provide. Files: the touched .c files + silent_bool_errors_baseline.txt (+ agent_impact_rules.def if a mapping is missing). Gate: make lint (esp. check_silent_bool_errors) + build-only + the focused test group for each touched file's owner. Report how many sites you cleared (55 -> N).`,
    { isolation: 'worktree', model: 'sonnet', label: 'O1-c:error-ratchet', phase: 'Harden', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE O1-d: generalize the authority-receipt idiom (A1, Law 7). The privileged-transition receipt pattern exists concretely only in config/src/consensus_state_replay_receipt.c; there is no reusable primitive or lint gate. Per docs/work/os/A1-authority-receipt-idiom.md: extract lib/util/authority_receipt.{c,h} — a reusable "a privileged transition must leave a durable, verifiable receipt naming who/what/why" primitive (create/append/verify a receipt record), refactor the consensus-replay consumer to use it (behavior-preserving), and add the check-privileged-transition-receipt lint gate (tools/lint/) that flags a privileged transition lacking a receipt. Add a hermetic test (mkdtemp-based: write a receipt, verify it, tamper-detect, verify a missing receipt is caught). Files: lib/util/authority_receipt.c/.h (new) + the gate script + config/src/consensus_state_replay_receipt.c (refactor to the primitive) + a focused test. Keep the consensus-replay behavior byte-identical (it is consensus-adjacent — the receipt content/format must not change what gets validated).`,
    { isolation: 'worktree', model: 'sonnet', label: 'O1-d:authority-receipt', phase: 'Harden', schema: LANE_SCHEMA }),
])

return { lanes: lanes.filter(Boolean) }
