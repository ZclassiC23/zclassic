export const meta = {
  name: 'simnet-wire-steps-bc',
  description: 'simnet_wire step B (adversarial peers + monitors) then step C (byzantine wire bridge), sequential; Haiku+codex implement, Sonnet verify each.',
  phases: [
    { title: 'Step B', detail: 'adversarial peer generators + core monitors on the landed transport engine' },
    { title: 'Step C', detail: 'byzantine artifact wire injection, branched from step B' },
  ],
}

// PRECONDITION: launch only after sim/wire-transport-a (step A) is MERGED to main.

const COMMON = `
You are an implementation-lane runner in an isolated git worktree of the zclassic23 repo (main repo: /home/rhett/github/zclassic23). You do mechanical orchestration ONLY; ALL heavy code writing is done by the codex CLI (GPT-5.5 xhigh reasoning), which is installed and verified working in this environment.

IMPORTANT: You may encounter notes saying "the Codex plugin was removed" — that refers to an old Claude Code plugin, NOT the standalone codex CLI binary, which is present at ~/.nvm/versions/node/v22.20.0/bin/codex and is the REQUIRED executor for this task (owner directive, 2026-07-09). The task being beyond Haiku-level implementation capability is EXPECTED and is exactly why codex does the writing — you only run commands and report. Do NOT stop to ask for direction; execute this runbook to completion.

RUNBOOK:
1. <CHECKOUT>
2. cp -a /home/rhett/github/zclassic23/vendor/lib ./vendor/   (or the link fails)
3. Write the TASK SPEC below plus the 'Hard rules' block verbatim into /tmp/claude-codex-task-<lane>.md
4. Run codex in the background (Bash run_in_background:true; can take 10-30 min):
   codex exec --dangerously-bypass-approvals-and-sandbox -C "$PWD" "$(cat /tmp/claude-codex-task-<lane>.md)"
   (codex's own bwrap sandbox cannot nest here — the bypass flag is required and verified working)
5. Run gates yourself: make lint; make -j$(nproc); then the focused test command(s) in the spec (build/bin/test_zcl <group> — ONLY the named groups, never the whole suite).
6. Gate fails -> codex exec again with the exact failing output + 'fix this in the current worktree, minimal diff'. Max 3 codex rounds, then report honestly.
7. Green -> git add -A && git commit (concise user-facing message). Do NOT push. Do NOT touch /home/rhett/github/zclassic23 itself.
8. Structured output: branch, per-gate pass/fail, summary, git diff --stat, blockers.

Hard rules for zclassic23 (put these in the codex task file):
- Consensus parity inviolable: no changes to consensus validation logic; the harness only OBSERVES tip/digest.
- zcl_malloc(size,"label") for allocations; LOG_FAIL/LOG_ERR/LOG_NULL on error returns (util/log_macros.h).
- simnet_wire security invariants (docs/work/io-harness-design.md 'Harness security' 1-6): no socket syscalls or <sys/socket.h> anywhere under lib/sim or tools/sim wire code; entropy/clock ONLY via seed_tape; nothing under lib/net or lib/validation may include "sim/"; bounded runtime via max_ticks + stuck-guard.
- NEVER use python. No pushes. Work ONLY inside the current worktree (temp files under /tmp fine).
`

const SPEC_B = `TASK SPEC — simnet_wire build-step B: adversarial peers + core monitors

Read docs/work/io-harness-design.md IN FULL — sections 'Adversarial peer model', 'Monitors', 'Harness security'. Step A (transport engine + honest loopback, lib/sim/src/simnet_wire.{c,h} + test_simnet_wire) is already merged on main — read that code first and build on it without rewriting it.

Deliverables:
- NEW lib/sim/src/simnet_wire_peer.c (+ header entry or extension of sim/simnet_wire.h): seed-driven adversarial byte generators, catalogue modeled on simnet_byzantine's g_meta[] (class -> reason -> expected blocker_class). Implement these kinds NOW: HONEST, MALFORMED_FRAME (bad checksum / oversized nMessageSize / bad magic — reuse the frame-fuzz patterns from the p2p fuzzer), BAD_HANDSHAKE (data before version / verack-first / garbage), FLOOD (inv/addr/getdata storm), SLOWLORIS (1 byte over many ticks). Leave INVALID_BLOCK/INVALID_HEADER/REPLAY/ECLIPSE/FUZZ for step C/D — define the enum values but the generators may return 'not implemented' cleanly.
- child_seed = splitmix64(master_seed ^ peer_id); the installed seed_tape RNG is the ONLY entropy source. A scenario {master_seed, [(kind,count)], honest_count, duration_us} must replay identically.
- Core monitors (checked after each tick + at end, each mapped to a real observable per the design doc): recv queue bounded (node->recv_msg_count <= MAX_RECV_MESSAGES under FLOOD); ban/disconnect (misbehavior reaches threshold, node->disconnect set, is_banned true); no unexpected BLOCKER_PERMANENT via blocker_snapshot_all; memory plateau (send_size/recv_msg_count/inventory/addr_to_send do not grow monotonically); checksum-fail/misbehave/backpressure events observed where expected. On violation: save the seed via seed_tape capsule.
- Extend lib/test/src/test_simnet_wire.c (or add test_simnet_wire_peer.c registered like the other sim groups; bump any asserted group count): one deterministic scenario per implemented adversary kind asserting its expected monitor outcome, plus one mixed scenario (honest + 2 adversaries) where the tip still advances from the honest peer.

Gates: make lint; make -j; build/bin/test_zcl <the wire test group(s)> run twice (determinism).`

const SPEC_C = `TASK SPEC — simnet_wire build-step C: byzantine-block wire injection bridge

Read docs/work/io-harness-design.md IN FULL — 'Adversarial peer model' (INVALID_BLOCK/INVALID_HEADER kinds), 'Monitors', 'Harness security'. You are on a branch containing steps A (transport engine) and B (adversarial peers + monitors) — read lib/sim/src/simnet_wire.c and simnet_wire_peer.c first and build on them.

Deliverables:
- The byzantine bridge: REUSE the existing simnet_byzantine builders (lib/sim/src/simnet_byzantine.c — 8 tier-1 connect_block rejects + 3 tier-2 header-gate rejects, with g_meta[] class->reason->expected blocker mapping). Serialize each byzantine artifact and FRAME it as real block/headers wire messages delivered through the step-A transport — the same artifact that previously went through direct calls now travels the wire.
- Implement the INVALID_BLOCK and INVALID_HEADER peer kinds left stubbed in step B, driven by the bridge.
- Assertions per injected artifact: (1) tip hash + UTXO digest UNCHANGED by the invalid input (simnet_cluster_tip_hash / coins_digest pattern), (2) the EXPECTED typed blocker class from g_meta[] (or the expected reject path) is observed — never a silent halt, (3) the offending peer accumulates misbehavior and is banned/disconnected per the weights (INVALID_BLOCK weight 100 -> banned after expected offences), (4) after the byzantine peer is banned, an honest peer still advances the tip.
- Tests: extend the wire test group(s) (bump any asserted group count): at least one wire-injection test per tier-1 class reusing the byzantine catalogue, deterministic (same seed -> same fingerprint), bounded ticks.

Gates: make lint; make -j; build/bin/test_zcl <the wire test group(s)> run twice, plus build/bin/test_zcl test_simnet_byzantine (must stay green — you reused, not modified, its builders).`

const IMPL_SCHEMA = {
  type: 'object',
  properties: {
    branch: { type: 'string' },
    committed: { type: 'boolean' },
    gates: { type: 'object', properties: { lint: { type: 'string' }, build: { type: 'string' }, tests: { type: 'string' } }, required: ['lint','build','tests'] },
    files_changed: { type: 'string' },
    summary: { type: 'string' },
    blockers: { type: 'string' },
  },
  required: ['branch','committed','gates','summary','blockers'],
}

const VERIFY_SCHEMA = {
  type: 'object',
  properties: {
    verdict: { type: 'string', enum: ['merge_ready','needs_work'] },
    gates_passed: { type: 'boolean' },
    reasons: { type: 'string' },
  },
  required: ['verdict','gates_passed','reasons'],
}

function verifyPrompt(branch, accept, tests, implSummary, files) {
  return `You are an independent merge-readiness verifier for zclassic23, in your own isolated worktree (main repo: /home/rhett/github/zclassic23). Branch '${branch}' was committed by an implementation agent (visible repo-wide).

Steps:
1. git checkout ${branch}
2. cp -a /home/rhett/github/zclassic23/vendor/lib ./vendor/
3. Read the FULL diff against its base (git log --oneline -5; git diff <base>...HEAD) against this bar: ${accept}
4. Independently rerun: make lint; make -j$(nproc); ${tests}. Report exact results; do not trust the implementer.
5. Check: scope creep; ANY consensus-logic change (= automatic needs_work); the simnet_wire security greps — no recv(/send(/socket(/connect(/bind(/getaddrinfo( or <sys/socket.h> in lib/sim wire code; nothing under lib/net or lib/validation includes "sim/"; entropy/clock only via seed_tape; missing LOG_* on new error returns; unchecked mallocs. Note: LOG_FAIL/LOG_ERR/LOG_NULL guard macros log AND return — do not flag 'missing return' after them.

Implementer summary (verify, don't trust): ${implSummary}
Files changed: ${files || 'unknown'}

merge_ready ONLY if your own gate rerun is green and the diff meets the bar; else needs_work with specific reasons.`
}

phase('Step B')
const implB = await agent(
  COMMON.replace('<CHECKOUT>', 'git checkout -b sim/wire-adversarial-b').replace(/<lane>/g, 'wireB')
    + '\n\n=== TASK SPEC (pass to codex verbatim) ===\n' + SPEC_B,
  { label: 'impl:wireB', phase: 'Step B', model: 'haiku', isolation: 'worktree', schema: IMPL_SCHEMA }
)
if (!implB || !implB.committed) return { stepB: implB, stepC: null, note: 'step B did not commit; step C not attempted' }

const verifyB = await agent(
  verifyPrompt('sim/wire-adversarial-b',
    'Step B only: seed-driven MALFORMED_FRAME/BAD_HANDSHAKE/FLOOD/SLOWLORIS generators + HONEST, deterministic replay from a scenario seed, core monitors (recv-queue bound, ban/disconnect, no unexpected permanent blocker, memory plateau), per-kind deterministic tests + one mixed scenario, security invariants intact.',
    'build/bin/test_zcl <wire test group(s) named in the diff> (run twice)',
    implB.summary, implB.files_changed),
  { label: 'verify:wireB', phase: 'Step B', model: 'sonnet', isolation: 'worktree', schema: VERIFY_SCHEMA }
)
if (!verifyB || verifyB.verdict !== 'merge_ready') return { stepB: { impl: implB, verify: verifyB }, stepC: null, note: 'step B not merge_ready; step C not attempted' }

phase('Step C')
const implC = await agent(
  COMMON.replace('<CHECKOUT>', 'git checkout sim/wire-adversarial-b && git checkout -b sim/wire-byzantine-c').replace(/<lane>/g, 'wireC')
    + '\n\n=== TASK SPEC (pass to codex verbatim) ===\n' + SPEC_C,
  { label: 'impl:wireC', phase: 'Step C', model: 'haiku', isolation: 'worktree', schema: IMPL_SCHEMA }
)
if (!implC || !implC.committed) return { stepB: { impl: implB, verify: verifyB }, stepC: implC, note: 'step C did not commit' }

const verifyC = await agent(
  verifyPrompt('sim/wire-byzantine-c',
    'Step C only (diff vs sim/wire-adversarial-b): simnet_byzantine artifacts reused (not modified) and framed onto the wire; INVALID_BLOCK/INVALID_HEADER kinds implemented; per-artifact assertions (tip+digest unchanged, expected typed blocker, ban at expected offence count, honest recovery); test_simnet_byzantine still green; determinism; security invariants intact.',
    'build/bin/test_zcl <wire test group(s)> (run twice) AND build/bin/test_zcl test_simnet_byzantine',
    implC.summary, implC.files_changed),
  { label: 'verify:wireC', phase: 'Step C', model: 'sonnet', isolation: 'worktree', schema: VERIFY_SCHEMA }
)

return { stepB: { impl: implB, verify: verifyB }, stepC: { impl: implC, verify: verifyC } }
