export const meta = {
  name: 'cured-node-to-tip',
  description: 'After the shielded import cures the wedge, make the node fold+validate the tail to network tip',
  phases: [
    { title: 'Diagnose+Fix', detail: '4 isolated worktree lanes: header-sync projection catchup, tail-fold validation, anchor-import completeness, post-import reindex withdrawal' },
  ],
}

const LANE_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['branch', 'verdict', 'files_changed', 'summary', 'gates'],
  properties: {
    branch: { type: 'string' },
    verdict: { type: 'string', enum: ['MERGE_READY', 'NEEDS_WORK', 'BLOCKED', 'DIAGNOSIS_ONLY'] },
    files_changed: { type: 'array', items: { type: 'string' } },
    summary: { type: 'string' },
    gates: { type: 'string' },
    root_cause: { type: 'string', description: 'is this a real code defect or a test artifact (torn live-copy, isolated node, cold projection)?' },
    caveats: { type: 'string' },
  },
}

const COMMON = `zclassic23 is one C23 binary, consensus-parity with zclassicd. Durable append-only log (progress.kv) folded by 8 reducer stages (header_admit -> validate_headers -> body_fetch -> body_persist -> script_validate -> proof_validate -> utxo_apply -> tip_finalize); each advances its cursor or names a typed blocker; H* = MIN over the contiguous ok=1 prefix; projections rebuild from the log.

PROVEN TODAY (2026-07-16, on main d27d6e603): the -import-complete-shielded importer is now FAST (7-9s, was intractable) and CORRECT. The O(anchors×Pedersen) blocker is fixed (bulk reader + inlined insert, tip-frontier-only Pedersen verify). On a COPY OF THE LIVE CANONICAL node (~/.zclassic-c23, wedged at H*=3,176,325), the import CLEARS the anchor_backfill_gap wedge: boundary=3,176,325 flipped, both activation cursors -> 0, coins_applied advanced to 3,176,326. This part WORKS — do not re-touch the importer's Pedersen path.

REMAINING TO REACH TIP (zclassicd oracle is at 3,183,592; the cured node must fold+validate the ~7,000-block tail 3,176,326→3,183,592): once booted (isolated copy on ports 18348/18349, addnode=127.0.0.1:8034 = local zclassicd which has all bodies) the cured node did NOT climb past 3,176,326. Observed blockers:
  - P2P header sync: "STALL DETECTED: accepted 160 headers but header tip=160 < chain tip=3176325 (peer at 3183599)" + "SLOW ADVANCE: sent 160 headers but tip only moved from 160 to 160" — even though the block index LOADED 3,184,371 entries at boot. So the header-sync tip source (a projection?) lags the loaded block index; the node won't fetch tail bodies.
  - utxo_apply.label_splice @3,176,326: "proof_validate_log block_hash MISSING != applying block 000001d2419b1c23..." — the tail block has no proof_validate receipt yet.
  - tip_finalize.validation_evidence @3,176,325: "utxo evidence=verified and exact verified script/proof receipts required".

CRITICAL FRAMING: some of these MAY be TEST ARTIFACTS, not code defects — a torn cp of a LIVE running node's block index ("tip hash maps to wrong height -1 ... Corrupt flat file — reloading from SQLite"), an isolated single-peer node, or a cold projection that self-heals as it catches up. DIAGNOSE FIRST. Only change code for a REAL defect. NEVER fake a green; if it's a test artifact, say so (verdict DIAGNOSIS_ONLY) and name the artifact. The prove datadir is /home/rhett/.zclassic-c23-COPY-20260716173455-canonical (a cured canonical copy; you MAY read it, and MAY boot your OWN copy of it on unique ports, but NEVER touch ~/.zclassic-c23, ~/.zclassic-c23-mint, or stop zclassicd).

HARD RULES: consensus parity inviolable; every malloc via zcl_malloc, every error return LOG_*, every write via AR lifecycle; files < 800 lines (app/+config ENFORCED); every thread supervised; recovery-apply paths stay CONTAINED (respect check-no-new-borrowed-seed / check-no-new-repair-rung). If your worktree cannot link, run make worktree-init first. You are in your OWN isolated git worktree — commit to THIS worktree's branch only; do NOT push/merge/touch others. Self-gate: make build-only + focused test group (make t-fast ONLY=<group>) + make lint (+ agent_impact_rules.def per changed .c). Final message = structured data, technical; report the branch name and root_cause (real-defect vs test-artifact).`

phase('Diagnose+Fix')

const lanes = await parallel([
  () => agent(`${COMMON}

LANE header-sync-projection-catchup. Diagnose why a freshly-import-cured node reports "header tip=160 < chain tip=3176325" in P2P header sync (lib/net/src/msg_headers.c process_headers ~line 677/866) even though the block index loaded 3,184,371 entries. Find the tip source header-sync compares against (a block_index_projection rebuilt from event_log? a cached header tip?) and whether it is lagging the loaded block index at cold boot. If it self-heals as the projection catches up (a cold-boot transient), prove that (boot your own copy, watch the header tip climb over a few minutes) and report DIAGNOSIS_ONLY naming the transient. If it is a REAL stall (the header-sync tip source never syncs to the block-index tip, so tail bodies are never requested), fix it minimally so header sync recognizes the existing chain and requests bodies for 3176326→tip. Files: lib/net/src/msg_headers.c and/or the header-tip/projection source it reads. Focused test if a real fix.`,
    { isolation: 'worktree', model: 'opus', label: 'diag:header-sync', phase: 'Diagnose+Fix', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE tail-fold-validation. Diagnose the utxo_apply.label_splice blocker at the wedge+1 boundary (h=3,176,326): "proof_validate_log block_hash MISSING != applying block 000001d2419b1c23...". This fires when utxo_apply tries to apply a tail block whose proof_validate receipt is absent. Determine: on a cured node folding forward from the wedge with bodies available, do script_validate + proof_validate actually RUN and stamp receipts for 3176326+ so utxo_apply and tip_finalize can advance — or is there a real seam bug at the import boundary (e.g. the reconcile that clamped tip_finalize 3176326->3176325 leaves a one-block hole, or the stages skip validation for blocks above the imported cursor)? If it self-resolves once bodies arrive (blocked only by the header-sync/body-fetch lane above), report DIAGNOSIS_ONLY. If there is a real boundary defect (validation not re-armed after the cursor flip/clamp), fix it minimally. Files: app/jobs/src/stage_repair.c (the clamp) and/or the utxo_apply / tip_finalize stage files — pick the ONE that owns the boundary. Focused test for a real fix.`,
    { isolation: 'worktree', model: 'opus', label: 'diag:tail-fold', phase: 'Diagnose+Fix', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE anchor-import-completeness. Verify the import's anchor set is COMPLETE vs zclassicd's chainstate. The import staged 366,169 Sapling + 271,291 Sprout anchors, 443,879 Sapling + 1,055,998 Sprout nullifiers. On the tipdemo copy a frontier spend hit "shielded_anchor_missing / anchor-membership-not-found @3182786". Determine whether the importer misses any anchors: open a READ-ONLY COPY of zclassicd's chainstate (cp the LevelDB to a scratch dir, never the live handle; zclassicd holds a lock) and count the 'Z' (sapling anchor) + 'A' (sprout anchor) + 'S'/'s' (nullifier) keys; compare to the imported counts. If they match, the import is complete and the missing-anchor is a fold-tail effect (an anchor created by a tail block 3151582..tip that the node must fold itself) — report DIAGNOSIS_ONLY explaining it. If zclassicd has MORE anchors than imported, the reader misses some (a keyspace/prefix/boundary bug in lib/storage/src/chainstate_legacy_reader.c) — fix it and add a completeness test. Files: a small verify tool or test + (only if incomplete) the reader. Do NOT touch the reader's _bulk Pedersen path.`,
    { isolation: 'worktree', model: 'sonnet', label: 'diag:anchor-completeness', phase: 'Diagnose+Fix', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE post-import-reindex-withdrawal. After the shielded import cures the wedge, the stale on-disk auto_reindex_request (armed by sticky_escalator's rung_reindex_default while the node was wedged) is NOT withdrawn, so the NEXT boot consumes it and does a pointless O(chain) -reindex-chainstate (observed live: had to rm the auto_reindex_request file by hand). withdraw_stale_reindex_request() exists in app/services/src/sticky_escalator.c (~line 577) but the consume beat the withdraw on the post-import boot. Make the shielded import (or the very-early boot path) withdraw/clear a pending non-terminal auto_reindex_request once the shielded wedge is cured, BEFORE the crash-only reindex consume fires — so a cured node boots into an O(delta) resume, never an O(chain) reindex. Keep it fail-safe (only withdraw a NON-terminal request; never suppress a genuine transparent-tear reindex). Files: app/services/src/sticky_escalator.c and/or the import service + a focused test (pending request present + wedge cured -> withdrawn; genuine tear -> preserved).`,
    { isolation: 'worktree', model: 'sonnet', label: 'fix:reindex-withdraw', phase: 'Diagnose+Fix', schema: LANE_SCHEMA }),
])

return { lanes: lanes.filter(Boolean) }
