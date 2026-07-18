export const meta = {
  name: 'sim-wire-and-hardening',
  description: 'Lane A: simnet_wire transport engine (step A); Lanes B/C: gossip blob hardening + printf-to-LOG. Haiku+codex implement, Sonnet verify.',
  phases: [
    { title: 'Implement', detail: 'haiku agent drives codex exec (gpt-5.5 xhigh) per lane in isolated worktree' },
    { title: 'Verify', detail: 'sonnet checks out branch, reruns gates, judges merge-readiness' },
  ],
}

const COMMON = `
You are an implementation-lane runner in an isolated git worktree of the zclassic23 repo (main repo: /home/rhett/github/zclassic23). You do the mechanical orchestration; the heavy code writing is done by the codex CLI (GPT-5.5, xhigh reasoning, already configured in ~/.codex/config.toml).

RUNBOOK (follow exactly):
1. You are already cd'd into a fresh worktree. Run: git checkout -b <BRANCH> (branch name given below).
2. Copy vendor prebuilt libs or the link will fail: cp -a /home/rhett/github/zclassic23/vendor/lib ./vendor/
3. Write the TASK SPEC below verbatim into a file OUTSIDE the repo tree, e.g. /tmp/claude-codex-task-<lane>.md — and append the 'Hard rules' block below to the end of that file so codex sees it too.
4. Run codex in the background (Bash run_in_background:true, since it can take 10-30 min):
   codex exec --dangerously-bypass-approvals-and-sandbox -C "$PWD" "$(cat /tmp/claude-codex-task-<lane>.md)"
   (Codex's own bwrap sandbox cannot nest inside this environment — the bypass flag is required and verified working. Codex must still confine its edits to the worktree; the task spec says so.)
   Wait for it to finish (you are re-invoked when the background command exits). Read its final output.
5. Run the gates yourself in the worktree:
   - make lint
   - make -j$(nproc)      (whole-program LTO; can take several minutes)
   - the focused test command(s) given in the task spec (test binary is build/bin/test_zcl; run ONLY the named groups, never the whole suite)
6. If a gate fails: run codex exec again (same recipe) with a short prompt containing the exact failing output and 'fix this in the current worktree, minimal diff'. Max 3 codex rounds total. If still red after 3, stop and report honestly.
7. When gates are green: git add -A && git commit with a concise user-facing message (what the change does for the node, not a dev diary). Do NOT push. Do NOT touch /home/rhett/github/zclassic23 itself.
8. Report via structured output: branch, gates status (exact pass/fail per gate), one-paragraph summary of what codex built, list of files changed (git diff --stat main..HEAD), any blockers.

Hard rules for zclassic23 (repeat these to codex if it strays):
- Consensus parity is inviolable: no changes to consensus validation logic, Equihash params, block/tx validity.
- Defensive coding gates: zcl_malloc(size,"label") for allocations, LOG_FAIL/LOG_ERR/LOG_NULL on error returns (util/log_macros.h), no raw sqlite3_step in app code.
- NEVER use python. Never run two deploys. No pushes.
- Work ONLY inside the current worktree; never modify /home/rhett/github/zclassic23 itself or anything outside the worktree (temp files under /tmp are fine).
`

const LANES = [
  {
    key: 'wire',
    branch: 'sim/wire-transport-a',
    spec: `TASK SPEC — simnet_wire build-step A (transport engine + honest loopback)

Read docs/work/io-harness-design.md IN FULL first — it is the authoritative, code-grounded design with exact seams and line references. Implement ONLY build-step A from its 'Build order' section. Do NOT implement adversarial peers (step B) or the byzantine bridge (step C).

Deliverables:
- NEW lib/sim/include/sim/simnet_wire.h + lib/sim/src/simnet_wire.c: an in-memory transport carrying REAL wire frames between a real 'struct p2p_node' (the node-under-test) and N in-memory peers, scheduled deterministically. Follow the design doc's module sketch (wire_link byte rings, wire_event queue, scheduler cloned from simnet_cluster_enqueue/deliver_one in lib/sim/src/simnet_cluster.c, virtual clock + rng ONLY via seed_tape).
- The NUT is created like fuzz_p2p.c does it (p2p_node_create with ZCL_INVALID_SOCKET) — study lib/fuzz or tools fuzz_p2p.c for the threadless ingress pattern, frame building, and reset. Spawn NO threads. Make NO socket syscalls.
- Ingress: deliver bytes via p2p_node_receive_bytes (lib/net/src/net.c:428) applying the SAME recv gates connman applies (recv cap via connman_recv_cap_for_queue, defer when recv queue full, disconnect on false return) — see design doc 'Pump per tick'.
- Egress: drain node->send_head segments directly (like fuzz_p2p.c does), never socket_send_data.
- Honest loopback test: NEW lib/test/src/test_simnet_wire.c registered exactly like test_simnet_cluster.c (find how the test group registry works; if a test asserts the total group/doc count, bump it). The test must: (1) complete a REAL version/verack handshake between NUT and one honest in-memory peer, (2) exchange one valid message round-trip (e.g. ping->pong) captured from send_head, (3) prove determinism: same seed run twice yields an identical FNV fingerprint of delivered bytes (fingerprint pattern in simnet_cluster.c), a different seed still completes, (4) bounded runtime via max_ticks + stuck-guard.
- Security constraints from the design doc section 'Harness security', items 1-6: simnet_wire.c must not include <sys/socket.h> or call recv/send/socket/connect/bind/getaddrinfo; no wall clock or entropy outside seed_tape; nothing under lib/net or lib/validation may include "sim/".
- Hook the new .c files into the build the same way the other lib/sim sources are built (check the Makefile).

Gates you must leave green: make lint; make -j; build/bin/test_zcl test_simnet_wire (run twice to confirm determinism).`,
    tests: 'build/bin/test_zcl test_simnet_wire (run it twice)',
    accept: 'Step A only: in-memory deterministic transport, real version/verack handshake + one message round-trip through the real p2p_node seams, determinism fingerprint test, no sockets/threads/wall-clock, security greps clean, test group registered.',
  },
  {
    key: 'blob',
    branch: 'security/gossip-blob-reads',
    spec: `TASK SPEC — peer-gossip blob memcpy hardening (next-wave lane #3)

next-wave-plan.md section 3 (removed from the tree; recover with
\`git log --follow -- docs/work/archive/next-wave-plan.md\`) covered this: several models copy fixed-size fields (16/32/43-byte hashes, scripts) out of SQLite rows that are populated from PEER GOSSIP, using memcpy without checking sqlite3_column_bytes — a malformed/short blob row causes an out-of-bounds read.

Files (this is the whole scope; do not expand it):
- app/models/src/swap_contract.c (redeem_script memcpy around line 152, plus any sibling fixed-size blob reads in the file)
- app/models/src/file_offer.c
- app/models/src/znam.c
- app/models/src/zmsg.c
- lib/znam/src/znam.c and lib/net/src/zmsg.c (reader sides), if they do the same unchecked pattern.

Fix: mechanical swap to the existing checked macro AR_READ_BLOB (app/models/include/models/activerecord.h:281) where the ActiveRecord layer is in scope, or an equivalent explicit length check where it is not. A length mismatch must fail CLOSED: skip/reject the row with a LOG_ERR/LOG_FAIL naming the table+column, never partially copy. No behavior change for well-formed rows. This is gossip/market/messaging surface — zero consensus risk, keep it that way.

Gates: make lint; make -j; then find and run the existing focused test groups covering these models (grep lib/test/src for swap_contract/file_offer/znam/zmsg test groups) via build/bin/test_zcl <group>.`,
    tests: 'focused groups for swap/file_offer/znam/zmsg found in lib/test/src',
    accept: 'Every fixed-size blob read from gossip-writable rows in the listed files is length-checked and fail-closed with logging; diff is minimal and mechanical; no consensus files touched.',
  },
  {
    key: 'printf',
    branch: 'dry/printf-to-log',
    spec: `TASK SPEC — printf-to-LOG_* in boot/recovery/validation + native JSON-output protection (next-wave lane #4)

next-wave-plan.md section 4 (removed from the tree; recover with
\`git log --follow -- docs/work/archive/next-wave-plan.md\`) covered this: raw stdout writes from library/service code corrupt native command JSON output and hide diagnostics from node.log.

Files (whole scope; do not expand):
- lib/validation/src/process_block_core.c (~33/50/82 printf sites)
- app/services/src/block_index_loader.c (~24 sites)
- app/services/src/chain_restore_repair.c (~7 sites)
- lib/sapling/src/params_init.c (the highest-volume offender)
- app/views/src/explorer_stats_view.c

Convert raw printf/fprintf(stdout|stderr) DIAGNOSTIC output to the appropriate LOG_* macros from util/log_macros.h (study how neighboring code in the same subsystems logs; preserve message content and severity intent). STRICTLY NO LOGIC CHANGE — this must be a pure output-channel swap. In explorer_stats_view.c convert only diagnostics; do NOT touch HTML/page emission paths. process_block_core.c is consensus-adjacent: only the print statements may change, nothing else on any line that affects control flow or values.

After the swap, verify with grep that none of the five files still write diagnostics to stdout.

Gates: make lint; make -j; run the focused test groups that cover these files (grep lib/test/src, e.g. block index loader / process block / explorer stats groups) via build/bin/test_zcl <group>.`,
    tests: 'focused groups covering block_index_loader / process_block / explorer stats',
    accept: 'Pure output-channel swap: all diagnostics in the five files go through LOG_* macros, zero logic change (diff shows only print-site rewrites), stdout reserved for native command JSON.',
  },
]

const IMPL_SCHEMA = {
  type: 'object',
  properties: {
    branch: { type: 'string' },
    committed: { type: 'boolean' },
    gates: { type: 'object', properties: { lint: { type: 'string' }, build: { type: 'string' }, tests: { type: 'string' } }, required: ['lint','build','tests'] },
    files_changed: { type: 'string', description: 'output of git diff --stat main..HEAD' },
    summary: { type: 'string' },
    blockers: { type: 'string', description: 'empty string if none' },
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

log('3 lanes: sim/wire-transport-a (mission), security/gossip-blob-reads, dry/printf-to-log')

const results = await pipeline(
  LANES,
  (l) => agent(
    COMMON.replace(/<BRANCH>/g, l.branch).replace(/<lane>/g, l.key) + '\n\n=== TASK SPEC (pass to codex verbatim) ===\n' + l.spec,
    { label: 'impl:' + l.key, phase: 'Implement', model: 'haiku', isolation: 'worktree', schema: IMPL_SCHEMA }
  ),
  (impl, l) => {
    if (!impl || !impl.committed) return { lane: l.key, impl, verify: null }
    return agent(
`You are an independent merge-readiness verifier for the zclassic23 repo, in your own isolated worktree (main repo: /home/rhett/github/zclassic23). An implementation agent committed branch '${impl.branch}' (visible repo-wide; worktrees share the object store).

Steps:
1. git checkout ${impl.branch}
2. cp -a /home/rhett/github/zclassic23/vendor/lib ./vendor/   (needed to link)
3. Read the FULL diff: git diff main...HEAD. Judge it against this acceptance bar: ${l.accept}
4. Independently rerun the gates: make lint; make -j$(nproc); then ${l.tests}. Report exact results — do not trust the implementer's claims.
5. Check for: scope creep beyond the listed files; any change to consensus validation logic (inviolable — any such change = needs_work); missing LOG_* on new error returns; unchecked mallocs; raw sqlite3_step in app code; for lane 'wire' also grep lib/sim/src/simnet_wire.c for recv(/send(/socket(/connect(/bind(/getaddrinfo( and <sys/socket.h> (must be absent) and confirm nothing under lib/net or lib/validation includes "sim/".
6. Note (do not flag as bugs): LOG_FAIL/LOG_ERR/LOG_NULL guard macros in util/log_macros.h log AND return — a 'missing return' after them is usually wrong.

Implementer's summary (verify, don't trust): ${impl.summary}
Files changed: ${impl.files_changed || 'unknown'}

Verdict merge_ready ONLY if all gates pass under your own rerun and the diff meets the bar. Otherwise needs_work with specific, actionable reasons.`,
      { label: 'verify:' + l.key, phase: 'Verify', model: 'sonnet', isolation: 'worktree', schema: VERIFY_SCHEMA }
    ).then((v) => ({ lane: l.key, impl, verify: v }))
  }
)

return { lanes: results.filter(Boolean) }
