# Arch Quest Board — play the game to 100/100

**You are the executor. Your job: drive `make arch-score` from 20 → 100.**
This board is the whole game. Read `docs/ARCHITECTURE_NORTH_STAR.md` once for
the theory, then live here.

## How to play (the loop)

```
1. make arch-score            # see the score + the ✗/◐ quests, highest weight last
2. pick the highest-weight ✗  # that's your target this round
3. open its quest below       # exact files, exact move, exact win-proof
4. implement the MOVE         # in a worktree; copy-prove; keep consensus frozen
5. run the WIN-PROOF          # the command that flips the quest green
6. make arch-score            # confirm the number went UP; if not, you didn't win
7. gates: make lint && make test-parallel   # must be "ALL TESTS PASSED"
8. commit; go to 1
```

## The three rules (breaking any = the run is void)

1. **No cheating the scorer.** You win a point ONLY by the real proof. Editing
   `arch_score.sh`, `arch_frontier_owners.tsv`, or a KPI's threshold to make a
   number go up without the underlying fix is an instant loss. The score file
   is the referee, not a lever. (You MAY *add* a new KPI row when a genuinely
   new invariant becomes checkable — never inflate an existing one.)
2. **Consensus is frozen.** Never change Equihash params, activation heights,
   block/tx validity, or the frozen verifiers. Call them; don't touch them.
   `check-consensus-parity` must stay clean. Replay-gate any bounded-predicate
   change (the h=478544 lesson).
3. **Copy-prove before live.** Prove every state/boot fix on a datadir COPY (a
   `/tmp` or `~/.local/state` fixture), never a live/soak lane. Gate on real
   progress (H* climbs / bundle installs), never "booted without FATAL".

## Scoreboard (weights)

| pts | quest | status | boss? |
|----:|-------|--------|-------|
| 20 | Q1 instant-on-e2e | ✗ | 🏆 BOSS |
| 20 | Q2 single-writer-per-frontier | ◐ 1/2 | |
| 15 | Q3 readers-read-the-fold | ✗ (= Q1) | |
| 15 | Q4 stay-synced | ✗ | |
| 10 | Q5 observability | ✓ DONE | |
| 10 | Q6 no-silent-stall | ✗ | |
| 10 | Q7 no-O(chain)-boot | ✗ | |
|  0 | Q8 enforcement gate | ✗ | ceiling |

Dependency order (do these first, they unlock the boss): **Q7 → Q6 → Q2 → Q3
→ Q1**, with **Q4** last (needs Q1). Q8 anytime (locks in Q2).

---

## Q1 — instant-on-e2e · 20 pts · 🏆 BOSS
- **📍 now:** a wiped node autodetects the bundle, tries to install, then
  DEFERS FOREVER and folds from genesis (D8).
- **🏁 win-proof:** `make mvp-coldstart-to-tip-stopwatch` emits a `pass` with a
  real `WALL_CLOCK_SECONDS`, recorded in the c3 ledger.
- **🔧 move:** this boss falls once Q7 + Q3(D8) are done. Then: rebuild the
  fixture via bundle-install (`-nolegacyimport`, bundle staged in
  `<datadir>/bundles/`, headers imported), it installs + climbs to tip
  observably (Q5 already lets you watch), stopwatch PASSes.
- **🚫 no-cheat:** the peer must be at true tip; the ledger must carry
  `final_hstar`/`final_network_tip` (audit A9). A near-instant PASS against a
  below-checkpoint peer does not count.

## Q2 — single-writer-per-frontier · 20 pts (10 banked)
- **📍 now:** 1/2 frontiers clean. `header_frontier` has a second writer:
  `--importblockindex` writes `pindex_best_header->nHeight` directly instead of
  appending to the validated-header ledger.
- **🏁 win-proof:** `arch_frontier_owners.tsv` — every listed frontier shows 0
  writers outside its owner (the KPI prints `N/N clean`).
- **🔧 move:** demote every non-owner writer to a read-only VIEW. For
  `header_frontier`: the importer/artifact must append a verified fact to the
  validate_headers ledger; `pindex_best_header` becomes a derived view of that
  fold. Then add more frontier rows (shielded anchor/nullifier frontiers) and
  clean each. **This is the same fix as Q3/D8** — do them together.
- **🚫 no-cheat:** don't shrink the manifest to win; grow it as you enforce.

## Q3 — readers-read-the-fold · 15 pts · (proven by Q1)
- **📍 now:** the install gate reads a side header cursor, sees 0, defers (D8).
  File: `config/src/consensus_state_install_runtime.c:447` ("checkpoint bundle
  deferred: validated header chain has not yet reached checkpoint height").
- **🏁 win-proof:** tied to Q1's PASS — you cannot claim readers read the right
  authority until a wiped node actually installs and reaches tip.
- **🔧 move (D8, the key unlock):** make the install-ready predicate
  (`consensus_state_checkpoint_header_ready`, :59-76) bind on the PoW-verified
  header frontier that `--importblockindex` / the A3 header-seed artifact
  already produced (they run `block_row_verify` = frozen CheckProofOfWork +
  check_equihash_solution). Headers imported ARE verified — record that
  verification into the frontier the gate reads
  (`validate_headers_stage_ensure_pass_record` at
  `app/jobs/src/validate_headers_validator.c:659` already does this for ONE
  header — extend so the gate opens without folding to the checkpoint). The D8
  lane `wf/install-header-frontier` is in flight — check it first.
- **🚫 no-cheat:** the checkpoint header MUST still pass the frozen validator.
  Never accept an unvalidated header frontier.

## Q4 — stay-synced · 15 pts · (needs Q1)
- **📍 now:** no soak PASS, no disruption-recovery PASS.
- **🏁 win-proof:** `make c3-stopwatch-report` → `VERDICT=PASS` AND a `pass` in
  the netdisrupt ledger.
- **🔧 move:** after Q1, stand a dedicated upstream fixture and run
  `network_disruption_recovery_stopwatch.sh` (SIGSTOP an upstream fixture —
  NEVER a live lane, audit A8); let the c3 soak accrue via the installed timer.

## Q5 — observability · 10 pts · ✓ DONE
- Kept green by: all 3 stage dumpers use `progress_store_tx_trylock` (A2). Do
  not regress — any new dumper that touches the progress store must trylock.

## Q6 — no-silent-stall · 10 pts
- **📍 now:** the Sapling-tree rebuild LIVELOCKS on the legacy path
  (`app/controllers/src/sync_controller_sapling_tree_persist.c:165`), logging
  "deferring height=… foreign open transaction" forever with NO named blocker
  (D7). Violates "a stall is always a named blocker with a height".
- **🏁 win-proof:** that retry loop raises a typed blocker (name + height) after
  a bounded number of deferrals (the KPI greps for it).
- **🔧 move:** two parts — (a) after N deferrals raise a TRANSIENT blocker
  naming the height + the foreign-transaction cause; (b) fix the livelock so it
  actually resolves (persist off the reducer connection, or a bounded persist
  window when the reducer returns to autocommit). Same class as memory
  `project_wallet_nodedb_busy_lock`.

## Q7 — no-O(chain)-boot · 10 pts
- **📍 now:** the bundle install path can still trigger the boot-time Sapling
  rebuild (the D7 O(chain) replay). A bundle SHIPS the shielded tree — nothing
  should rebuild.
- **🏁 win-proof:** the KPI finds the install path skips the Sapling rebuild
  when the bundle provided the tree.
- **🔧 move:** ensure `consensus_state_install_from_bundle` installs the
  bundle's Sapling frontier/tree so boot detects it present and does NOT arm
  `sapling_tree_rebuild_start_deferred`. Verify the bundle carries a v3
  shielded section (it does); wire the install to land it.

## Q8 — enforcement gate · 0 pts, but it LOCKS Q2
- **📍 now:** the single-writer invariant is a convention, not a law — a future
  executor can re-clone a ledger and nothing stops them.
- **🏁 win-proof:** `tools/scripts/check_frontier_single_writer.sh` exists AND
  is wired into `make lint` (the score prints `enforcement: PRESENT`).
- **🔧 move:** a grep-class gate (same family as `check-consensus-parity`) that
  reads `arch_frontier_owners.tsv` and FAILS the build if any frontier has a
  writer outside its owner. Wire into the `lint` target.

---

## Winning the game

`make arch-score` = 100/100 means: a wiped datadir boots, installs the bundle,
reaches tip on a stopwatch in minutes, stays synced through a network cut, has
one canonical writer per frontier (lint-enforced), never silently stalls, and
never does O(chain) work at boot. That is the whole mission, made countable.
