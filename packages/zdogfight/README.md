# zdogfight

Deterministic headless 2-team aerial dogfight match core, in strict
C23 — the authoritative simulation for cross-node, byte-identical
replays.

Fixed planes (up to 4 per team, red vs blue) fly a toroidal 2 km
world with bank-to-turn controls, guns, kills, respawns, and a match
that ends at 10 kills or 10 minutes. Per-tick controls in, one tick
advanced per call.

**Determinism contract:** same seed + same per-tick controls produce
byte-identical state on any machine and compiler. Integer arithmetic
only (no floating point — trig is a 256-entry int16 Q1.15
quarter-wave table), no heap, no clock, no `rand()`, no I/O; all
state lives in the caller-provided `zdog_match`, and the only entropy
is the embedded zprng xoshiro256**, drawn solely by respawns in
ascending plane index order.

- `zdog_match_init` / `zdog_tick` — fixed-seed match setup and the
  60 Hz tick
- `zdog_observe` / `zdog_obs` — pilot ABI observation (nearest-enemy
  sighting, toroidal-aware)
- `zdog_state_encode` — canonical little-endian field-by-field state
  serialization (always exactly `ZDOG_STATE_WIRE_MAX` = 2163 bytes);
  hash those bytes for the caller's state root
- `zdog_state_checksum` — built-in FNV-1a/64 over the canonical
  encoding for cheap in-test checks
- `zdog_obs_encode/decode`, `zdog_ctl_encode/decode` — exact-size
  wire codec for the process pilot ABI (82 and 7 bytes)

`zdogfight selftest` runs a fixed-seed match between two built-in
trivial pilots and prints the final state root (FNV-1a/64) as hex.

Depends only on zprng. MIT licensed.
