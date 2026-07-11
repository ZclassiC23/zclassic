# core/ UNSEAL log — append-only owner ritual record

The top-level `core/` tree is the **sealed consensus core**: the predicates and
static, height-keyed parameter tables that decide whether a block/tx is valid.
Its byte-integrity is pinned by `core/MANIFEST.sha3` (a SHA3-256 manifest, see
`tools/core_seal.c`) and enforced by the `check-core-seal` lint gate.

**Sealed ≠ frozen.** Consensus-parity fixes still ship routinely — they just may
not go through the autonomous fast path. A deliberate change to a sealed file
requires the unseal ritual below; only the unattended/agent fast-path is
structurally refused.

## The unseal ritual

```
make core-unseal REASON="why this consensus-core change is needed"
#   → appends a dated entry to this file (old ROOT hash + reason)
#   → writes .core-unseal-token (gitignored) that `make core-seal-check`
#     honors for exactly one commit
# ... make the sealed-core edit ...
make core-seal        # re-freeze the manifest (also consumes the token)
make lint && make test_parallel   # must land green, incl. test_consensus_parity
git commit            # the reseal + the edit land together
```

The token authorizes the seal check to tolerate drift for the single commit that
introduces the change; `make core-seal` re-freezes and removes it. No agent can
mint this token as a normal source edit — it is an owner-run make target (v1.1
upgrades this to an ed25519 owner signature so consent cannot be forged).

`check-core-seal` is in **WARN/ratchet** mode until core-split wave W5, when a
later lane flips it HARD.

---

## Log

<!-- UNSEAL-ENTRIES (newest appended below; append-only, never edit past entries) -->
