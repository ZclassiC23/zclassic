# OS-A1 — Generalize the privileged-transition authority idiom

Status: DESIGN (executable recipe). Lane: WF-security, opus.
Plan of record: `~/.claude/plans/think-more-about-our-keen-crown.md` §2 SECURE / OS-A1;
Law 7 (`privileged transitions require independent receipts`).
Every anchor below was re-read on `main@6405cf48d` (2026-07-15).

Bar: **behavioral equivalence** — after the re-base the replay receipt writes the
byte-identical 344-byte payload with the byte-identical `receipt_digest`, and the
`consensus_state_snapshot_install` test group passes UNCHANGED. The only new
behavior is a lint gate.

---

## 1. What already exists (the idiom to extract)

`config/src/consensus_state_replay_receipt.c` implements Law 7 concretely. Four
reusable pieces are tangled into the consensus-specific derivation:

| Idiom piece | Current site | What it does |
|---|---|---|
| running-binary digest | `rr_verifier_binary_digest()` :106-138 | SHA3-256 of `/proc/self/exe`, race-free direct `open` (readlink reintroduces TOCTOU) |
| atomic keyed-file write | `rr_write_atomic()` :226-275 | tmp → `fsync(file)` → `rename` → `fsync(dir)` under the datadir |
| dirfd read at use time | `rr_read_file()` :279-305 | `openat(datadir_fd, NAME, O_NOFOLLOW)`, read **exactly** N bytes (read N+1, reject if ≠ N), then self-verify |
| use-time authority check | `consensus_state_replay_receipt_authority_available()` :599-641 | read via dirfd → self-consistency (`receipt_digest`) → running-binary MUST equal recorded → context digests MUST equal caller's → fail-closed `bool` |

Consumer: `config/src/consensus_state_snapshot_install_activate.c`
`activate_independent_authority_available()` :136-151 calls
`consensus_state_replay_receipt_authority_available()`; the
`CONSENSUS_INSTALL_VERIFIED_CONTAINED` latch is the `return activate_fail(...)`
at :953 (guard block opens at the `3b.` comment :947). This is the ACTIVATE
containment that A1 must not perturb.

The receipt-specific parts that STAY in the replay module (do not generalize —
they define the 344-byte layout and its bytes): the `struct rr_receipt` (:57-72),
`RR_OFF_*` offsets + `_Static_assert` (:37-55), `rr_receipt_digest()` :141-167
(domain `"…replay_receipt.v1/binding"`, 12 typed fields in fixed order),
`rr_serialize()` :169-187, `rr_deserialize()` :191-216, and the three SQL
derivations `derive_utxo/anchors/nullifiers`.

The running-binary idiom is duplicated verbatim in
`config/src/consensus_state_producer_receipt.c:106` (`running_binary_digest()`).
Both `consensus_state_replay_receipt.c` and `consensus_state_producer_receipt.c`
are PERMANENT-exemption entries in `tools/lint/proc_self_shim_baseline.txt`
(race-free `open("/proc/self/exe")`, not migration debt).

---

## 2. New module — `lib/util/authority_receipt.{c,h}`

Extract the four idiom pieces only. Do **not** move the field layout. Placement
in `lib/util` is legal: `check-lib-layering` forbids only `lib/ → app/` includes;
`lib/util → lib/crypto` (`crypto/sha3.h`) is permitted and the build already adds
`-Ilib/crypto/include` to every lib TU. `LIB_SRCS` globs `lib/util/src/*.c`, so
the new `.c` is picked up with zero Makefile source wiring.

### 2a. `lib/util/include/util/authority_receipt.h`

```c
/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * authority_receipt — the reusable Law-7 privileged-transition idiom.
 *
 * The contract, stated once: a self-asserted artifact never authorizes a
 * privileged state change. Authority requires a receipt that a prior pass
 * derived INDEPENDENTLY and bound to {artifact digest, context anchor, the
 * EXACT running-binary image}, re-checked fail-closed at use time through a
 * datadir capability fd (pathnames are locators, never authority).
 *
 * This header owns the transition-agnostic mechanics: the race-free running-
 * binary digest, the atomic keyed-file write, the exact-length dirfd read, and
 * a fixed canonical HEADER + verify skeleton future consumers bind onto. A
 * consumer with a rich typed payload (e.g. consensus_state_replay_receipt)
 * folds its fields into `detail_digest` and keeps its own field codec; the
 * generic contract binds the digest, so the header layout never changes. */
#ifndef ZCL_UTIL_AUTHORITY_RECEIPT_H
#define ZCL_UTIL_AUTHORITY_RECEIPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── Idiom primitives (the three pieces the replay receipt re-bases onto) ── */

/* SHA3-256 of the running executable image (/proc/self/exe), race-free direct
 * open (a path readlink reintroduces TOCTOU). false on any read/close error. */
bool authority_receipt_running_binary_digest(uint8_t out[32]);

/* Atomic keyed write under `datadir`: tmp -> fsync(file) -> rename -> fsync(dir).
 * Writes exactly `len` bytes to `<datadir>/<name>` (0600). On success copies the
 * final absolute path into final_out when non-NULL. Fail-closed bool. */
bool authority_receipt_write_atomic(const char *datadir, const char *name,
                                    const uint8_t *payload, size_t len,
                                    char *final_out, size_t final_cap);

/* Read EXACTLY `len` bytes of `<datadir_fd>/<name>` through the capability fd
 * (openat O_RDONLY|O_CLOEXEC|O_NOFOLLOW). Reads up to len+1 and returns true iff
 * precisely `len` bytes were present (a longer OR shorter file is rejected) —
 * byte-for-byte the rr_read_file length rule. Does NOT interpret the bytes. */
bool authority_receipt_read_fixed(int datadir_fd, const char *name,
                                  uint8_t *out, size_t len);

/* ── Canonical HEADER for NEW consumers (hot-swap/deploy/cartridge) ── */

#define AUTHORITY_RECEIPT_SCHEMA_FIELD 48u
/* schema[48] + artifact[32] + anchor[32] + detail[32] + verifier[32] + digest[32] */
#define AUTHORITY_RECEIPT_HEADER_BYTES 208u

struct authority_receipt_header {
    char    schema[AUTHORITY_RECEIPT_SCHEMA_FIELD]; /* NUL-terminated domain tag */
    uint8_t artifact_digest[32];         /* WHAT is being authorized */
    uint8_t context_anchor[32];          /* WHERE it sits (chain anchor / epoch) */
    uint8_t detail_digest[32];           /* SHA3 of the independent derivation */
    uint8_t verifier_binary_digest[32];  /* the exact image that verified */
    uint8_t receipt_digest[32];          /* domain-bound over the 5 fields above */
};

/* Domain-separated binding over {schema,artifact,anchor,detail,verifier};
 * domain = header->schema followed by the literal "/binding". */
void authority_receipt_header_digest(const struct authority_receipt_header *h,
                                     uint8_t out[32]);

/* Producer: caller has filled schema/artifact_digest/context_anchor/
 * detail_digest. Fills verifier_binary_digest (running image) + receipt_digest,
 * serializes the fixed 208-byte header, writes it atomically. */
bool authority_receipt_header_seal_and_write(struct authority_receipt_header *h,
                                             const char *datadir, const char *name,
                                             char *final_out, size_t final_cap);

/* Consumer (use-time authority), fail-closed. Reads the fixed header through
 * datadir_fd, re-verifies its self-binding receipt_digest, requires the running
 * binary to equal verifier_binary_digest, then requires schema/artifact/anchor/
 * detail to equal the caller's expected values. Any missing/tampered/foreign/
 * different-binary receipt -> false (the transition stays contained). */
bool authority_receipt_header_authority_available(
    int datadir_fd, const char *name, const char *expect_schema,
    const uint8_t expect_artifact[32], const uint8_t expect_context_anchor[32],
    const uint8_t expect_detail_digest[32]);

#endif /* ZCL_UTIL_AUTHORITY_RECEIPT_H */
```

### 2b. `lib/util/src/authority_receipt.c`

- `authority_receipt_running_binary_digest` — move the body of
  `rr_verifier_binary_digest` :106-138 verbatim (`open("/proc/self/exe", O_RDONLY|
  O_CLOEXEC)`, 32768-byte SHA3 loop, EINTR-safe, `close` checked). `#include
  "crypto/sha3.h"`, `"util/log_macros.h"`; subsystem tag `"authority_receipt"`.
- `authority_receipt_write_atomic` — body of `rr_write_atomic` :226-275,
  parameterized on `name` (was the fixed `CONSENSUS_STATE_REPLAY_RECEIPT_NAME`)
  and `len` (was `RR_PAYLOAD_BYTES`). Uses `<datadir>/<name>.tmp.<pid>`.
- `authority_receipt_read_fixed` — body of `rr_read_file` :279-305 minus the
  final `rr_deserialize` call: read into a `len+1` local via a caller-provided
  `out` of `len` (read one extra into a scratch byte, reject if a `len+1`th byte
  exists). Keep the exact `got != len` rejection.
- `authority_receipt_header_digest` / `_seal_and_write` /
  `_authority_available` — new code modeled 1:1 on `rr_receipt_digest`,
  `rr_serialize`+`rr_write_atomic`, and
  `consensus_state_replay_receipt_authority_available` respectively, over the
  208-byte header. `put_le64`/`get_le64` are not needed (header has no integer
  fields); serialize = memset 0, memcpy the 6 fixed fields at pinned offsets.
  Add a `_Static_assert(48 + 5*32 == AUTHORITY_RECEIPT_HEADER_BYTES, …)`.

---

## 3. Re-base the replay receipt (behavioral equivalence — the merge bar)

Edit `config/src/consensus_state_replay_receipt.c` ONLY. Three surgical swaps;
the typed struct, offsets, `rr_receipt_digest`, `rr_serialize`, `rr_deserialize`,
and the SQL derivations are UNTOUCHED, so the on-disk bytes and `receipt_digest`
are identical.

1. Add `#include "util/authority_receipt.h"`.
2. **Delete** `rr_verifier_binary_digest()` (:106-138). Replace its two call
   sites — :568 `if (!rr_verifier_binary_digest(r.verifier_binary_digest))` and
   :617 `if (!rr_verifier_binary_digest(running) || …)` — with
   `authority_receipt_running_binary_digest(...)`.
3. **Delete** `rr_write_atomic()` (:226-275) and its `rr_receipt_path` helper if
   now unused (:218-223 — keep only if still referenced; after the swap it is
   not). Replace the :574 call
   `rr_write_atomic(datadir, buf, final_path, sizeof(final_path))` with
   `authority_receipt_write_atomic(datadir, CONSENSUS_STATE_REPLAY_RECEIPT_NAME,
   buf, RR_PAYLOAD_BYTES, final_path, sizeof(final_path))`.
4. **Delete** `rr_read_file()` (:279-305). At its call site inside
   `consensus_state_replay_receipt_authority_available` (:609), replace with:
   ```c
   uint8_t buf[RR_PAYLOAD_BYTES];
   if (!authority_receipt_read_fixed(datadir_fd,
           CONSENSUS_STATE_REPLAY_RECEIPT_NAME, buf, RR_PAYLOAD_BYTES) ||
       !rr_deserialize(buf, &r)) { … stays contained; return false; }
   ```
   (`rr_deserialize` moves to the caller — it was previously called inside
   `rr_read_file`; the self-binding check it performs is preserved.)

After the swap, `consensus_state_replay_receipt.c` no longer contains the string
`"/proc/self` or a bespoke `rename`/`fsync` write — it is now clean of both
idioms.

**Producer receipt (immediate second re-base, same commit):**
`config/src/consensus_state_producer_receipt.c:106` `running_binary_digest()` is
byte-identical to the extracted helper — replace its body with a call to
`authority_receipt_running_binary_digest`. Its digest layout is unchanged, so
`consensus_state_producer_receipt` tests stay green. This proves the extraction
serves ≥2 consumers on day one and lets both files leave the proc-self baseline.

---

## 4. Files touched

**New:**
- `lib/util/include/util/authority_receipt.h`
- `lib/util/src/authority_receipt.c`
- `lib/test/src/test_authority_receipt.c`
- `tools/lint/check_privileged_transition_receipt.sh`
- `tools/lint/privileged_transition_receipt_baseline.txt`
- `docs/work/os/A1-authority-receipt-idiom.md` (this file)

**Edited (with the exact seam):**
- `config/src/consensus_state_replay_receipt.c` — re-base (§3), seams :106/:226/:279/:568/:574/:609/:617.
- `config/src/consensus_state_producer_receipt.c` — re-base `running_binary_digest` body at :106.
- `tools/lint/proc_self_shim_baseline.txt` — ADD `lib/util/src/authority_receipt.c`
  (permanent-exemption class, same reason block); REMOVE
  `config/src/consensus_state_replay_receipt.c` and
  `config/src/consensus_state_producer_receipt.c` (shrink-only ratchet — both no
  longer read `/proc/self` directly).
- `Makefile` — add `check-privileged-transition-receipt` target stanza + append
  it to the `lint:` target list (`:4259`).
- `app/controllers/include/controllers/agent_impact_rules.def` — add a rule (Trap A)
  mapping the new files to the new focused group (§6c).
- `lib/test/src/test.c` (`:1010` neighborhood) + `lib/test/src/test_parallel.c`
  (`X(...)` list `:283` neighborhood) — register the `authority_receipt` group.
- `lib/test/src/test_make_lint_gates.c` — the `[lint-gate] E11 doc gate list
  matches Makefile lint: target` assertion (`:2002`) enumerates the gate list;
  add `check-privileged-transition-receipt`.
- `docs/DEFENSIVE_CODING.md` — document the gate (next free number after Gate #47,
  e.g. **Gate #48**) so `check-doc-counts` / the E11 doc list agree.

---

## 5. The lint gate — `check-privileged-transition-receipt` (baseline-ratchet)

**Contract (Law 7, mechanized):** every native command leaf whose spec is
`ZCL_COMMAND_AUTH_OWNER` **and** effect `ZCL_COMMAND_EFFECT_MUTATE` or
`ZCL_COMMAND_EFFECT_DESTRUCTIVE` is a candidate privileged transition. Each such
leaf must have a **disposition** in
`tools/lint/privileged_transition_receipt_baseline.txt`. A new owner-mutating
leaf with no disposition FAILS — forcing a conscious Law-7 review. The gate never
requires the contract be present today (no current leaf calls it); it ratchets
future ones.

**Baseline format** (one leaf per line; `#` comments/blank ignored):
```
<leaf.path>  receipt:<relative_file>   # transition; <file> MUST grep authority_receipt_*_available(
<leaf.path>  exempt:<one-line reason>  # not an artifact-install transition
```
For `receipt:<file>` lines the gate additionally asserts `<file>` contains a call
matching `authority_receipt_.*_available(` **or**
`consensus_state_replay_receipt_authority_available(` (the pre-generalized name,
so ACTIVATE's existing consumer qualifies) — a positive check that the wired
consumer actually gates on the contract.

**Enumeration algorithm** (build-free; parse `config/commands/*.def`): walk each
`ZCL_COMMAND_{READY,PLANNED,COMPAT,DEV}_COMMAND(` macro by paren depth; a spec
qualifies iff its text contains `ZCL_COMMAND_AUTH_OWNER` and
(`ZCL_COMMAND_EFFECT_MUTATE` | `ZCL_COMMAND_EFFECT_DESTRUCTIVE`); the leaf path is
the first `"…"` token. (READ-form macros hard-code `EFFECT_READ` and are excluded
by construction — verified against the macro defs in
`config/src/command_catalog.c:44-216`.)

**Today's universe (26 leaves — the initial baseline, all `exempt`):** verified
by the parser on `main@6405cf48d`:
```
app.account.add / role / suspend / unsuspend          exempt: App-principal AR write, no artifact install
core.consensus.block.invalidate / reconsider          exempt: in-memory validity mask, no state bundle install
core.wallet.address.new / import                       exempt: keystore write, guarded by wallet AR + AT_REST
core.wallet.transaction.send / shielded.send           exempt: signs+broadcasts, no privileged datadir install
core.wallet.shielded.address / backup.now / rescan / replay   exempt: wallet-local, rebuildable projection
dev.app.scaffold / app.publish / change.apply          exempt: dev-lane, publication already contained
dev.vcs.revert / vcs.seal.grant                        exempt: ZVCS source epoch, seal ritual owns authority
dev.hotswap.apply / loop.ensure / loop.stop            exempt: verify-only dev loop, no live generation swap
dev.generation.rollback / generation.compact           exempt: dev generation store, contained
ops.recovery.rebuild                                    exempt: refold reset, gated by copy-prove doctrine
```
The gate is GREEN at landing (all 26 dispositioned). Any 27th owner-mutating leaf
fails until dispositioned.

**Future consumers get `receipt:` (the point of the gate), in this order:**
1. **bundle ACTIVATE (re-base — done at the library level).** When the ACTIVATE
   path is surfaced as an OWNER native leaf, its handler file's baseline line is
   `receipt:config/src/consensus_state_snapshot_install_activate.c` (already calls
   the contract at :149).
2. **hot-swap Phase-3 reopen** (`lib/hotswap/…`) — the durable publish
   transaction resolves a source epoch; its OWNER leaf must bind
   `authority_receipt_header_*` over {generation artifact digest, epoch anchor,
   binary}.
3. **`make deploy` generation publish** — the release-digest publish step binds a
   header receipt {release digest, `anchor_publish` height, binary} before relink.
4. **ADR-0004 App cartridge activation** (`docs/adr/0004-*`, §4) — a cartridge
   checkpoint activates only against a header receipt {cartridge CAS digest,
   platform-control journal anchor, appd binary}.

**Gate script shape** (`tools/lint/check_privileged_transition_receipt.sh`, model
on `tools/lint/check_proc_self_shim.sh` — same `set -euo pipefail`, `SCRIPT_DIR`/
`ROOT`, baseline-map load, clean/violation exit, actionable stderr):
```bash
# enumerate owner-mutating leaves from config/commands/*.def (awk paren-depth)
# for each leaf not in baseline: violation (print + fail, with the disposition
#   template to add). for each `receipt:<file>` baseline line: assert <file>
#   greps authority_receipt_.*_available( ; else fail "wired consumer lost its gate".
# clean => "check_privileged_transition_receipt: clean — N owner-mutating leaves,
#   all dispositioned (M receipt, K exempt)"
```
Self-test immunity: exclude the gate's own script/baseline from any source scan
(follow `tools/lint/scan_exclusions.sh`), matching the repo's other gates.

**Makefile stanza** (mirror `check-proc-self-shim` at `:3953`):
```make
.PHONY: check-privileged-transition-receipt
check-privileged-transition-receipt:
	@./tools/lint/check_privileged_transition_receipt.sh
```
then append `check-privileged-transition-receipt` to the `lint:` list at `:4259`.

---

## 6. Tests + registration

### 6a. `lib/test/src/test_authority_receipt.c` (new group `authority_receipt`)
Hermetic, uses a `mkdtemp` datadir; opens it with `open(dir, O_DIRECTORY)` for the
dirfd path. Cases:
1. **round-trip:** `authority_receipt_write_atomic(dir,"t",p,len,…)` then
   `authority_receipt_read_fixed(dirfd,"t",q,len)` returns the same bytes.
2. **exact-length rule:** write `len+1` bytes → `read_fixed(…,len)` returns false;
   write `len-1` → false. (Pins the rr_read_file semantics.)
3. **header seal/verify:** fill artifact/anchor/detail, `_seal_and_write`, then
   `_authority_available(dirfd,name,schema,artifact,anchor,detail)` == true;
   flip any one expected input → false; flip one on-disk byte (via a raw write)
   → false (self-binding `receipt_digest` catches it).
4. **binary-mismatch:** overwrite the stored `verifier_binary_digest` field →
   `_authority_available` false (the running binary no longer matches).
5. **running-binary determinism:** `authority_receipt_running_binary_digest`
   returns a stable non-zero 32 bytes across two calls.

### 6b. Register the group
- `lib/test/src/test.c` near `:1010`: `failures += test_authority_receipt();`
  (with the matching `extern int test_authority_receipt(void);`).
- `lib/test/src/test_parallel.c` near `:283`: add `X(authority_receipt) \` to the
  X-macro list.

### 6c. Impact rule (Trap A — else push is BLOCKED)
Add to `app/controllers/include/controllers/agent_impact_rules.def`:
```
AGENT_IMPACT_RULE(
  "lib/util/src/authority_receipt.c|lib/util/include/util/authority_receipt.h|"
  "lib/test/src/test_authority_receipt.c|tools/lint/check_privileged_transition_receipt.sh|"
  "tools/lint/privileged_transition_receipt_baseline.txt|config/src/consensus_state_replay_receipt.c",
  "authority_receipt consensus_state_snapshot_install make_lint_gates")
```
This keeps every re-base edit mapped to BOTH the new group and the unchanged
`consensus_state_snapshot_install` group (the behavioral-equivalence proof).

---

## 7. Acceptance bar

1. `make build-only` clean; full `make -j$(nproc)` links (new lib TU auto-globbed).
2. **`consensus_state_snapshot_install` group passes UNCHANGED** — the replay
   receipt's `verify_and_write_receipt` / `authority_available` cases at
   `lib/test/src/test_consensus_state_snapshot_install.c:2411-2446` are green with
   no edit to that test (behavioral equivalence). `consensus_state_producer_receipt`
   green too.
3. New `authority_receipt` group green (`make test-parallel`, read the
   `ALL TESTS PASSED — 0/N` line).
4. `make lint` green including `check-privileged-transition-receipt` (26 leaves,
   0 undispositioned), `check-proc-self-shim` (baseline shrunk by 2, new util
   file added), `check-lib-layering` (util→crypto is allowed), and the E11 doc-gate
   assertion + `check-doc-counts` reconciled for the new gate + new test group.
5. **Negative proof of the gate:** temporarily add a throwaway
   `ZCL_COMMAND_READY_COMMAND(... ZCL_COMMAND_AUTH_OWNER ... ZCL_COMMAND_EFFECT_MUTATE ...)`
   leaf to a `.def` → `check-privileged-transition-receipt` FAILS naming it;
   remove it → green. (Prove the gate fires; do not commit the throwaway.)
6. Grep proof of equivalence: `grep -c '"/proc/self' config/src/consensus_state_replay_receipt.c`
   → 0 after re-base; the receipt file basename + payload constants are unchanged.

---

## 8. Collision + parity risk

**In-flight lanes (WF-contract): B1 (command spec+validator), A2 (sandbox), B3b
(REST route-parity), D2 (projection scaffold).**

- `config/src/consensus_state_replay_receipt.c` and
  `config/src/consensus_state_producer_receipt.c`: **NOT touched** by B1/A2/B3b/D2
  (B1 edits `command_registry.h`/`command_catalog.c`/`.def`/`native_command.c`; A2
  edits `os_sandbox_linux.c`/`boot.c`; B3b edits REST route table + a new gate;
  D2 edits projection files). Confirmed — no source overlap on the re-base files.
  The new `lib/util/authority_receipt.*` and `tools/lint/*` files are net-new,
  disjoint from all four.
- **Known shared reconcile points (mechanical, per plan §3b):** (a) the Makefile
  `lint:` list at `:4259` — B1 (`check-command-contract`), A2 (extends
  `check-sandbox-wired`), B3b (`check-route-command-parity`) and A1
  (`check-privileged-transition-receipt`) all append here; append order is
  order-independent. (b) `test_make_lint_gates.c` E11 gate-list assertion — every
  new gate appends. (c) `test_parallel.c` X-macro list + `test.c` — each new group
  (A1 `authority_receipt`, D2's projection group) appends a distinct line. (d)
  `check-doc-counts` test-group count + `DEFENSIVE_CODING.md` gate list. All four
  are the exact collision class the merge phase reconciles; A1 introduces no NEW
  collision surface beyond them.
- **Consensus-parity risk: none.** The receipt is not consensus data (ZClassic
  headers commit none of it) and the re-base is byte-preserving: the replay
  receipt's serialization (`rr_serialize`), binding digest (`rr_receipt_digest`),
  and 344-byte layout are untouched, so the `receipt_digest` and the ACTIVATE
  `VERIFIED_CONTAINED` decision are bit-identical. The behavioral-equivalence bar
  (item 2) is the guard.
