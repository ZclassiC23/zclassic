# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# sticky_fault_inject.sh — the corruption LIBRARY for the stickiness
# fault-injection matrix (sticky-node-plan §4). SOURCED by
# sticky_matrix.sh; never executed directly.
#
# Each `inject_<row>` function mutates a COPY of a datadir (or mints a
# fresh/foreign one) in place, then returns:
#   0  — fault injected, the row is ATTEMPTABLE (caller restarts + gates)
#   2  — KNOWN-BLOCKED: precondition unmet (e.g. no durable seed). The
#        caller counts the row as BLOCKED (NOT pass, NOT fail) so a
#        regtest-durability gap can never read as a vacuous green.
#
# CONTRACT: $1 is ALWAYS a datadir under /tmp owned by the harness — the
# functions here NEVER touch the live datadir (the matrix copies first,
# and isolated_node_env.sh's refuse-set/LISTEN preflight is the upstream
# guard). Every mutation is on a throwaway copy.

# Require a seeded chain of at least $2 blocks in datadir $1, else BLOCKED.
# Mirrors reindex_smoke.sh's known_blocked(): regtest `generate` is not yet
# durable across restart, so rows needing a restart-surviving seed SKIP loud.
_sti_require_seed() {
    local dd="$1" want="$2" h
    h="$(ZCL_DATADIR="$dd" ZCL_RPCPORT="$ISO_RPCPORT" "$ISO_RPC_BIN" \
            getblockcount 2>/dev/null | tr -dc '0-9-')"
    [ -n "$h" ] && [ "$h" -ge "$want" ] && return 0
    echo "sticky-inject: KNOWN-BLOCKED (regtest seed h=${h:-?} < $want; generate not durable across restart, see reindex_smoke.sh)" >&2
    return 2
}

# Pick the sqlite DB the node uses for chainstate inside $1 (best-effort:
# the harness only mutates files it finds; absent file => BLOCKED).
_sti_pick_db() {
    local dd="$1" f
    for f in "$dd"/node.db "$dd"/coins.db "$dd"/chainstate.db; do
        [ -f "$f" ] && { echo "$f"; return 0; }
    done
    return 1
}

# Flip a span of bytes at a deterministic offset of FILE (poor-man's
# corruption; dd seek/notrunc, no shell arithmetic on the bytes). Used by
# several rows. $1 file, $2 offset, $3 count.
_sti_garble() {
    local f="$1" off="$2" cnt="$3"
    [ -f "$f" ] || return 1
    dd if=/dev/urandom of="$f" bs=1 seek="$off" count="$cnt" \
        conv=notrunc status=none 2>/dev/null || return 1
    return 0
}

# Row 1 — fresh/empty datadir: nothing to inject; the copy is empty.
inject_fresh_empty() { rm -rf "$1"/* 2>/dev/null || true; return 0; }

# Row 2 — foreign datadir: the caller seeded a DIFFERENT regtest chain into
# $1; booting the binary against it must converge, not wedge. No mutation.
inject_foreign() { return 0; }

# Row 3 — mid-write kill-9 is handled by the driver (spawn + SIGKILL during
# a write window), not a file mutation. This is the FULL-binary complement
# to test-crash-bootstrap. Requires a durable seed to prove CLIMB.
inject_kill9_window() { _sti_require_seed "$1" "$STI_SEED_BLOCKS"; }

# Row 4 — corrupt sapling tree: zero the sapling-tree state. Prefer a
# dedicated file; else garble a span of the chainstate DB header region.
inject_corrupt_sapling() {
    local dd="$1" f
    for f in "$dd"/sapling_tree.dat "$dd"/sapling.dat; do
        [ -f "$f" ] && { : > "$f"; return 0; }
    done
    f="$(_sti_pick_db "$dd")" || return 2
    _sti_garble "$f" 4096 256 && return 0
    return 2
}

# Row 5 — corrupt coins-view: garble UTXO rows in the chainstate DB. The
# boot-time coins_view atomicity check + auto-rewind must heal (the exact
# surface test_coins_view_atomicity / test_kill9_recovery cover at unit level).
inject_corrupt_coinsview() {
    local f; f="$(_sti_pick_db "$1")" || return 2
    _sti_garble "$f" 65536 512 && return 0
    return 2
}

# Row 6 — torn index: truncate the chainstate DB tail (drops a contiguous
# span of block-index rows). Boot must re-derive / re-pull, not FATAL-loop.
inject_torn_index() {
    local f sz; f="$(_sti_pick_db "$1")" || return 2
    sz="$(stat -c %s "$f" 2>/dev/null)" || return 2
    [ "$sz" -gt 8192 ] || return 2
    truncate -s "$((sz - 4096))" "$f" 2>/dev/null && return 0
    return 2
}

# Row 7 — equal-work-corrupt incumbent (3157647 class): install an
# equal-work sibling with a zeroed sapling root as the active tip. This
# needs the sibling-adopt cure (#3, branch acute/header-repair-cure) AND a
# fixture builder; until both land, the row is BLOCKED (flagged).
inject_equalwork_corrupt() {
    echo "sticky-inject: KNOWN-BLOCKED row7 (equal-work-corrupt incumbent needs sibling-adopt cure #3 + an equal-work fixture builder)" >&2
    return 2
}

# Row 8 — truncated header solution: garble the Equihash solution bytes of a
# stored header so its bytes no longer hash to a PoW-valid header. The S8
# re-derivation invariant must reject + re-pull. File mutation.
inject_truncated_header() {
    local f; f="$(_sti_pick_db "$1")" || return 2
    _sti_garble "$f" 131072 1344 && return 0   # ~Equihash 200,9 solution span
    return 2
}

# Row 9 — oracle absent: regtest NEVER has a co-located zclassicd oracle, so
# this is the default condition. No mutation; the driver asserts CLIMB with
# no oracle process reachable (S4: peer/self path, never the RPC oracle).
inject_oracle_absent() { return 0; }

# Row 10 — peers absent then returned: handled by the driver (spawn isolated
# with a dead -connect sink, then add a live regtest peer mid-run). No file
# mutation; requires the two-node arrangement (model: two_node_peer_tip.sh).
inject_peers_absent_then_returned() { return 0; }

# Row 11 — disk-full then freed: needs an operator-mountable size-capped
# tmpfs as the datadir. PARTIAL-hermetic: the disk_full_pause CONDITION is
# fully exercised by test_sticky_conditions; the spawn row is BLOCKED until
# a mount helper exists.
inject_disk_full() {
    echo "sticky-inject: KNOWN-BLOCKED row11 (needs a size-capped tmpfs mount helper; disk_full_pause condition proven in test_sticky_conditions instead)" >&2
    return 2
}

# Row 12 — deep reorg at finality floor: mine a competing branch deeper than
# ZCL_FINALITY_DEPTH. Needs durable regtest blocks.
inject_deep_reorg() { _sti_require_seed "$1" "$STI_SEED_BLOCKS"; }

# Row 13 — clock jump: the binary clock is not injectable from outside the
# process, so this is proven hermetically by test_sticky_conditions
# (platform_clock_set_source). BLOCKED in the spawn matrix.
inject_clock_jump() {
    echo "sticky-inject: KNOWN-BLOCKED row13 (binary clock not externally injectable; clock_skew_reconcile proven in test_sticky_conditions)" >&2
    return 2
}
