#!/usr/bin/env bash
# Lint gate E13 — consensus-parity guard (HARD).
#
# zclassic23 MUST stay BIT-FOR-BIT consensus-compatible with zclassicd
# (the canonical C++ ZClassic daemon). The non-negotiable rule, stated in
# docs/CONSENSUS_PARITY_DOCTRINE.md:
#
#   Equihash (N,K) and EVERY network-upgrade activation resolve from a
#   STATIC, height-keyed table only — never from miner signaling, a
#   versionbits/BIP9/BIP8 deployment state machine, or any dynamic
#   per-height parameter override.
#
# zclassicd has no versionbits / signaling apparatus: it activates all
# upgrades purely by fixed height (nHeight >= nActivationHeight) and reads
# Equihash (N,K) from EquihashUpgradeInfo[CurrentEpoch(height)]. Introducing
# a miner-signaled override (the PR #6 "Equihash 200,9 sidegrade" class)
# would make nodes disagree on which (N,K) / which rules are valid at a
# height and FORK the chain away from zclassicd.
#
# This gate fails CI if any such mechanism token appears in the consensus
# source surface. It starts GREEN (every token is absent today).
#
# False positive? Add `// consensus-parity-ok:<reason>` to the line. The
# reason must explain why this is NOT a divergence from zclassicd.
set -euo pipefail

cd "$(dirname "$0")/../.."

# Consensus-critical source surface. docs/ and lib/test/ are intentionally
# NOT scanned — the doctrine and the parity test may name these tokens
# defensively without being a real mechanism.
PATHS=(core/params lib/validation lib/chain lib/mining app/jobs core/consensus)

# FAIL-LOUD preflight (never report "clean" off a silently-empty scan).
# If a consensus dir was renamed/moved/deleted, grep would scan a SMALLER
# surface and still exit 0 — a forbidden mechanism in the drifted dir would
# escape the single most sacred gate. So every PATHS entry MUST exist; a
# missing one means the consensus surface drifted and PATHS must be updated
# DELIBERATELY (and the parity boundary re-verified). A NEW consensus dir not
# in PATHS is likewise unscanned — when you add one, add it here.
for p in "${PATHS[@]}"; do
    if [ ! -d "$p" ]; then
        echo "check_consensus_parity: FATAL — consensus path '$p' is missing." >&2
        echo "  The consensus source surface drifted. Update PATHS in this gate" >&2
        echo "  deliberately and re-verify the zclassicd parity boundary before" >&2
        echo "  re-greening. Refusing to report 'clean' off a partial scan." >&2
        exit 2
    fi
done

# Forbidden mechanism tokens. Each denotes a miner-signaled / versionbits /
# dynamic Equihash-override apparatus that zclassicd does NOT have. The
# legitimate static getters chain_params_equihash_n / chain_params_equihash_k
# are NOT matched — they carry no "_at" suffix. EquihashUpgradeInfo and the
# height-keyed epoch lookup are the CORRECT parity mechanism and stay allowed.
FORBIDDEN='versionbits|VersionBitsState|ComputeBlockVersion|ehUpgrade|eh_upgrade|nSignalBit|vbits_|equihash_n_at|equihash_k_at|BIP9|BIP8'

# Run the scan and check grep's exit EXPLICITLY: 0=match, 1=no-match,
# >=2=real error (bad flag, unreadable file). The old `2>/dev/null || true`
# masked a >=2 error as an empty result and reported "clean" — a fail-silent
# hole. Only 0/1 are valid; >=2 fails the gate LOUD.
set +e
raw=$(grep -rnE "$FORBIDDEN" "${PATHS[@]}" --include='*.c' --include='*.h')
grc=$?
set -e
if [ "$grc" -ge 2 ]; then
    echo "check_consensus_parity: FATAL — scan grep failed (exit $grc) over the" >&2
    echo "  consensus surface; refusing to report 'clean' off a broken scan." >&2
    exit 2
fi

hits=""
if [ -n "$raw" ]; then
    hits=$(printf '%s\n' "$raw" \
        | grep -vE '(//|/\*) ?consensus-parity-ok:[A-Za-z][A-Za-z0-9_-]+' || true)
fi

if [ -n "$hits" ]; then
    echo "$hits"
    echo ""
    echo "FAIL: a non-zclassicd consensus mechanism appears in the consensus path."
    echo "  zclassic23 MUST stay bit-for-bit consensus-compatible with zclassicd."
    echo "  See docs/CONSENSUS_PARITY_DOCTRINE.md. Equihash (N,K) and every upgrade"
    echo "  activation resolve from the STATIC, height-keyed table ONLY — never from"
    echo "  miner signaling / versionbits / a dynamic per-height override."
    echo "  If this is genuinely not a divergence, mark the line:"
    echo "      // consensus-parity-ok:<reason>"
    exit 1
fi

echo "check_consensus_parity: clean — no non-zclassicd consensus mechanism in the consensus path"
exit 0
