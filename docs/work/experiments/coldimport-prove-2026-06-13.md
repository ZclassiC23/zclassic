# Experiment: cold-import tear reproduction probe (2026-06-13)

**Status:** RUNNING (linger service `zcl-coldimport-prove.service`, user scope).
**Owner:** autonomous (Claude), this session.
**Gates:** the Phase-1 import-correctness-gate design (see
`docs/work/FORWARD_PLAN.md` / the recurring-wedge root cause).

## Question

The live main node is forward-wedged at 3,145,594 by a **genuine cold-import
coin tear**, not a benign log-hole (root-caused 2026-06-13; see memory
`project_recurring_anchor_collapse_wedge_2026-06-13`). The cold import installed
a UTXO set seeded from a **competing orphan block** around h=3,145,486 (canonical
coinbase `60fc6f43a630b5b7:0` absent), and the canonical spend at h=3,145,595
then fails `prevout_unresolved`, collapsing the trusted anchor → I4.3 HOLD.

**Does a FRESH two-step cold import on the current HEAD binary reproduce this?**

- **If it re-wedges at h=3,145,595** → a *deterministic* import/connect bug. The
  Phase-1 gate must both refuse the torn set AND the import bug gets fixed.
- **If it cleanly reaches tip** → the live tear was a *one-off* (an orphan was
  briefly the active tip during a reorg when the original sync ran, and our
  connect path recorded it). The Phase-1 gate is then purely *preventive* — it
  refuses such a torn set at write time so it can never become "our" chain.

Either outcome is decisive for the gate's design. The import path under
suspicion is the **boot-path legacy import** (`utxo_recovery_restore.c`, the
SHA3 "verify-later" branch at :312-315), NOT `--importchainstate` (strict
checkpoint gate).

## Method

`tools/experiments/coldimport_prove.sh`, run as a persistent user service:

1. `--importblockindex ~/.zclassic /tmp/zcl-coldimport-prove/node.db` (headers;
   LOCK-safe copy of the source block index).
2. Normal boot on a **throwaway** datadir `/tmp/zcl-coldimport-prove`, unused
   ports **P2P=8053 / RPC=18252** (cannot collide with main 8023/18232, soak
   8043/18242, zclassicd 8033/8232). The boot triggers the legacy UTXO import
   from `~/.zclassic`.

**Safety:** writes only the throwaway datadir; reads `~/.zclassic` read-only;
refuses to run against any real datadir. Does not touch the main/soak/zclassicd
nodes.

## How to read the result

- Results log: `/tmp/zcl-coldimport-prove/PROBE_RESULTS.txt`
- Node log: `/tmp/zcl-coldimport-prove/node.log`
- Service: `systemctl --user status zcl-coldimport-prove` ·
  `journalctl --user -u zcl-coldimport-prove`

Verdict signal — watch the served tip vs h=3,145,595:
- **REPRODUCED:** node.log shows `block-not-finalized-by-reducer` /
  `prevout_unresolved` at ~3,145,595 and the served tip pins below it; the block
  at h=3,145,486 may differ from canonical `00000adaf0…`.
- **CLEAN:** served tip advances past 3,145,595 toward zclassicd's tip; also
  yields a fresh-sync wall-clock datapoint for MVP-C3 (<10 min cold-sync).

## Teardown

```
systemctl --user stop zcl-coldimport-prove 2>/dev/null
rm -rf /tmp/zcl-coldimport-prove
```
The throwaway datadir is disposable; nothing depends on it.
