#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# cross-machine-reproduce.sh — CROSS-MACHINE REPRODUCTION harness for a
# published C23 Commons package, with scrupulously honest labeling.
#
#   tools/dev/cross-machine-reproduce.sh <package-name> [ssh-host]
#
# <package-name> is the scopes.def name (zhkdf/zhkdf) or its unique short
# form (zhkdf). With no [ssh-host] the harness runs the CONTROL TWICE on
# this machine (no network needed) and labels the evidence
# physical_machines=1. With [ssh-host] the exact same inputs (verifier
# binary, source tree, recipe wire, locked dep install trees) are
# transferred to the remote and rebuilt there (physical_machines=2).
#
# Resolution reuses the factory's own authorities, read-only:
#   - corpus/scopes.def:            package name -> published root + store
#   - `zcode package dev prepare`   (risk: read) re-derives package root,
#                                   dependency lock root and recipe wire
#                                   from packages/<short>; the derived root
#                                   MUST equal the published root or the run
#                                   is BLOCKED (source drift)
#   - corpus/factory/<short>.report.json: published recipe root; the
#                                   re-derived recipe wire is byte-compared
#                                   against <store>/zcode/recipes/<root>
#   - packages/*/zcode-package.json: dependency pins, walked transitively
#                                   (post-order, deduped); each dep must be
#                                   installed at <store>/zcode/installed/
#                                   <root>/ with a build-report
#
# Both builds run the confined verifier in candidate mode
# (build/bin/zclassic23-package-verify, profile=standard, cpu=120 — the
# factory's own reproduction parameters) with --emit + --plan. The emitted
# output trees are compared per path + SHA3-256 + byte count; the
# toolchain capsule identity (zcl.dep_plan.v1 toolchain block: compiler
# id/version, target, capsule_root) is compared SEPARATELY. If toolchains
# differ the verdict is "toolchain_mismatch" — honest evidence, never a
# silent pass/fail.
#
# Evidence: .cache/zcl-cross-machine/<short>-<label>.json, schema
# zcl.cross_machine_repro.v1, carrying a MANDATORY independence block:
# same-operator machines are NOT independent operator groups. No
# wall-clock fields.
#
# Exit codes: 0 verdict=reproduced; 2 usage; 3 resolution/preflight
# BLOCKED; 4 ssh BLOCKED (evidence still written, verdict=blocked);
# 5 a confined build run failed; 6 verdict=mismatch (outputs diverged on
# equal toolchains); 7 verdict=toolchain_mismatch.
#
# Controls: ZCL_XMR_KEEP=1 keeps the scratch tree (printed at the end).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

VERIFY_BIN="$ROOT/build/bin/zclassic23-package-verify"
NODE_BIN="$ROOT/build/bin/zclassic23"
SIGN_BIN="$ROOT/build/bin/zclassic23-package-sign"
SCOPES="$ROOT/corpus/scopes.def"
OUTDIR="$ROOT/.cache/zcl-cross-machine"
KEEP="${ZCL_XMR_KEEP:-0}"

log()
{
    printf '[cross-machine-reproduce] %s\n' "$*"
}

fail() # exit 2: usage / internal
{
    printf '[cross-machine-reproduce] FATAL: %s\n' "$*" >&2
    exit 2
}

blocked() # <exit-code> <reason-name> <detail>
{
    printf '[cross-machine-reproduce] BLOCKED[%s]: %s\n' "$2" "$3" >&2
    exit "$1"
}

usage()
{
    printf '%s\n' \
        'Usage: tools/dev/cross-machine-reproduce.sh <package-name> [ssh-host]' \
        '' \
        'Cross-machine (or local double-control) reproduction of a published' \
        'C23 Commons package under the confined verifier. Evidence JSON:' \
        '.cache/zcl-cross-machine/<short>-<label>.json (zcl.cross_machine_repro.v1).' \
        '' \
        'Exit: 0 reproduced | 2 usage | 3 resolution BLOCKED | 4 ssh BLOCKED' \
        '| 5 build run failed | 6 outputs mismatch | 7 toolchain_mismatch.' \
        '' \
        'Controls: ZCL_XMR_KEEP=1 keeps the scratch tree.'
}

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
    usage
    exit 0
fi
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    usage >&2
    exit 2
fi
PKG_ARG="$1"
SSH_HOST="${2:-}"

# ── preflight ──────────────────────────────────────────────────────────

for bin in "$VERIFY_BIN" "$NODE_BIN" "$SIGN_BIN"; do
    [ -x "$bin" ] || fail "missing $bin — the build tree must provide it"
done
command -v python3 >/dev/null 2>&1 ||
    fail "python3 is required (JSON + SHA3-256 tree comparison)"
[ -f "$SCOPES" ] || fail "missing $SCOPES"
if [ -n "$SSH_HOST" ]; then
    command -v ssh >/dev/null 2>&1 || blocked 4 ssh-tool-missing \
        "ssh client not installed; cannot attempt host $SSH_HOST"
    command -v scp >/dev/null 2>&1 || blocked 4 ssh-tool-missing \
        "scp not installed; cannot attempt host $SSH_HOST"
fi

# ── resolve the published package (corpus/scopes.def) ──────────────────

SCOPE_LINE="$(awk -v n="$PKG_ARG" \
    '$1 == "package" && $2 == n { print; exit }' "$SCOPES")"
if [ -z "$SCOPE_LINE" ]; then
    # short form: exactly one scopes.def name "*/<arg>" may match
    SCOPE_LINE="$(awk -v n="/$PKG_ARG" \
        '$1 == "package" && index($2, n) == length($2) - length(n) + 1 \
            { print }' "$SCOPES")"
    [ -n "$SCOPE_LINE" ] || blocked 3 unknown-package \
        "no scopes.def package row names '$PKG_ARG'"
    case "$(printf '%s\n' "$SCOPE_LINE" | wc -l)" in
        1) ;;
        *) blocked 3 ambiguous-package \
            "short name '$PKG_ARG' matches more than one scopes.def row" ;;
    esac
fi
# Row shape: package <name> | root <64hex> | store <path> | kind ... — the
# "root"/"store" keywords are literal fields, so name=$2 root=$5 store=$8.
FULL_NAME="$(printf '%s\n' "$SCOPE_LINE" | awk '{ print $2 }')"
PUB_ROOT="$(printf '%s\n' "$SCOPE_LINE" | awk '{ print $5 }')"
STORE="$(printf '%s\n' "$SCOPE_LINE" | awk '{ print $8 }')"
SHORT="${FULL_NAME##*/}"
SRC_DIR="$ROOT/packages/$SHORT"
REPORT_JSON="$ROOT/corpus/factory/$SHORT.report.json"

[ -d "$SRC_DIR" ] || blocked 3 source-missing \
    "packages/$SHORT not in this checkout — cannot rebuild from source"
[ -d "$STORE/zcode" ] || blocked 3 store-missing \
    "published store $STORE has no zcode/ tree"
[ -d "$STORE/zcode/installed/$PUB_ROOT" ] || blocked 3 not-installed \
    "$FULL_NAME ($PUB_ROOT) is not installed in $STORE"
[ -f "$REPORT_JSON" ] || blocked 3 report-missing \
    "corpus/factory/$SHORT.report.json not found — no published recipe root"
log "package $FULL_NAME root=${PUB_ROOT:0:16}… store=$STORE"

# ── scratch workspace ──────────────────────────────────────────────────

WORK="$(mktemp -d "${TMPDIR:-/tmp}/zcl-xmr-XXXXXXXX")"
cleanup()
{
    if [ "$KEEP" != 1 ]; then
        rm -rf "$WORK"
    else
        log "scratch kept at $WORK"
    fi
}
trap cleanup EXIT

# ── read-only re-derivation of roots + recipe (factory's own path) ─────

# Throwaway publisher key: dev prepare wants a valid compressed curve
# point; nothing is signed and the key never leaves the scratch dir.
PREP_PUB="$("$SIGN_BIN" --generate "$WORK/prep-key" 2>/dev/null |
    tr -d '[:space:]')"
printf '{"dir":"%s","publisher_pubkey":"%s","publisher_sequence":1}' \
    "$SRC_DIR" "$PREP_PUB" |
    "$NODE_BIN" zcode package dev prepare --input=- > "$WORK/prepare.json" ||
    blocked 3 dev-prepare-failed \
        "zcode package dev prepare failed for packages/$SHORT"

RESOLVE_RC=0
python3 - "$WORK/prepare.json" "$WORK/recipe.wire" "$PUB_ROOT" \
    > "$WORK/resolved.txt" <<'PYEOF' || RESOLVE_RC=$?
import json, sys
doc = json.load(open(sys.argv[1]))
if not doc.get("ok"):
    print("dev prepare not ok", file=sys.stderr)
    sys.exit(1)
data = doc["data"]
if data["package_root"] != sys.argv[3]:
    print("derived package root %s != published root %s (source drift)" %
          (data["package_root"], sys.argv[3]), file=sys.stderr)
    sys.exit(3)
with open(sys.argv[2], "wb") as fh:
    fh.write(bytes.fromhex(data["recipe_hex"]))
print(data["package_root"])
print(data["dependency_lock_root"])
PYEOF
if [ "$RESOLVE_RC" -eq 3 ]; then
    blocked 3 source-drift \
        "packages/$SHORT no longer derives the published root $PUB_ROOT"
fi
[ "$RESOLVE_RC" -eq 0 ] ||
    blocked 3 dev-prepare-failed "dev prepare output unusable ($RESOLVE_RC)"
DERIVED_ROOT="$(sed -n 1p "$WORK/resolved.txt")"
LOCK_ROOT="$(sed -n 2p "$WORK/resolved.txt")"

# Cross-check the re-derived recipe wire against the published store's
# recipe object (root from the committed factory report).
RECIPE_ROOT="$(python3 -c \
    "import json; print(json.load(open('$REPORT_JSON'))['package']['recipe_root'])")"
STORE_RECIPE="$STORE/zcode/recipes/$RECIPE_ROOT"
[ -f "$STORE_RECIPE" ] || blocked 3 recipe-missing \
    "store has no recipes/$RECIPE_ROOT"
cmp -s "$WORK/recipe.wire" "$STORE_RECIPE" || blocked 3 recipe-drift \
    "re-derived recipe wire != $STORE_RECIPE"
log "lock=${LOCK_ROOT:0:16}… recipe=${RECIPE_ROOT:0:16}… (store byte-identical)"

# Dependency pins, transitive post-order, from the repo manifests (same
# file order the factory's recipe generation uses). Every pinned dep must
# be installed in the store with its build-report.
python3 - "$ROOT/packages" "$SHORT" > "$WORK/deps.txt" <<'PYEOF'
import json, os, sys
root_dir, start = sys.argv[1], sys.argv[2]
order = []
def visit(short):
    manifest = os.path.join(root_dir, short, "zcode-package.json")
    if not os.path.isfile(manifest):
        print("dep manifest missing: %s" % manifest, file=sys.stderr)
        sys.exit(3)
    m = json.load(open(manifest))
    for d in m.get("dependencies", []):
        visit(d["name"].split("/")[0])
        if d["root"] not in order:
            order.append(d["root"])
visit(start)
for r in order:
    print(r)
PYEOF
DEP_ARGS=()
DEPS_JSON="[]"
while IFS= read -r droot; do
    [ -n "$droot" ] || continue
    [ -f "$STORE/zcode/installed/$droot/build-report" ] || blocked 3 \
        dep-not-installed \
        "locked dependency $droot not installed under $STORE/zcode/installed"
    DEP_ARGS+=("--dep=$droot,$STORE/zcode/installed/$droot")
    DEPS_JSON="$(python3 -c "import json,sys; a=json.loads(sys.argv[1]); a.append(sys.argv[2]); print(json.dumps(a))" "$DEPS_JSON" "$droot")"
done < "$WORK/deps.txt"
log "deps: $(wc -l < "$WORK/deps.txt" | tr -d ' ') pinned (transitive closure)"

# ── one confined control build (always) ────────────────────────────────

# run_local_build <emit-dir> <plan-file> <stdout-file>
run_local_build()
{
    local emit="$1" plan="$2" out="$3"
    rm -rf "$emit"
    mkdir -p "$emit"
    local rc=0
    "$VERIFY_BIN" "$DERIVED_ROOT" \
        "--zbuild-package-source=$SRC_DIR" \
        "--zbuild-package-recipe=$WORK/recipe.wire" \
        "--zbuild-package-name=$FULL_NAME" \
        "--zbuild-package-profile=standard" \
        "--zbuild-package-max-cpu-seconds=120" \
        "--emit=$emit" "--lock-root=$LOCK_ROOT" \
        ${DEP_ARGS[@]+"${DEP_ARGS[@]}"} \
        "--plan=$plan" --require-full-isolation \
        > "$out" 2>"$out.stderr" || rc=$?
    printf '%s' "$rc"
}

# build_result_ok <stdout-file> <rc> — rc 0 AND a green result line.
build_result_ok()
{
    local out="$1" rc="$2" result
    result="$(sed -n 's/^.* result=\([a-z-]*\) .*/\1/p' "$out" | head -1)"
    [ "$rc" -eq 0 ] &&
        { [ "$result" = test-pass ] || [ "$result" = build-pass ]; }
}

mkdir -p "$WORK/control"
log "control build (local, standard profile, full isolation)"
RC_A="$(run_local_build "$WORK/control/emit" "$WORK/control/plan.json" \
    "$WORK/control/stdout")"
build_result_ok "$WORK/control/stdout" "$RC_A" || {
    printf '[cross-machine-reproduce] control build failed: rc=%s (%s.stderr)\n' \
        "$RC_A" "$WORK/control/stdout" >&2
    exit 5
}
UNAME_LOCAL="$(uname -srm)"
HOST_LOCAL="$(uname -n)"

# ── comparison run: ssh remote, or second local control ────────────────

MODE=""
SIDE_B_DIR=""
UNAME_B=""
HOST_B=""
BLOCKED_REASON=""
EVIDENCE_LABEL=""

if [ -n "$SSH_HOST" ]; then
    MODE="ssh"
    EVIDENCE_LABEL="$(printf '%s' "$SSH_HOST" | tr -c 'a-zA-Z0-9._-' '-')"
    SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=8)
    log "probing ssh host $SSH_HOST (BatchMode, fail closed)"
    if ! ssh "${SSH_OPTS[@]}" "$SSH_HOST" true 2>"$WORK/ssh-probe.err"; then
        BLOCKED_REASON="ssh-unreachable: $SSH_HOST did not answer a BatchMode probe ($(head -1 "$WORK/ssh-probe.err"))"
    else
        RWORK="$(ssh "${SSH_OPTS[@]}" "$SSH_HOST" \
            'mktemp -d /tmp/zcl-xmr-remote-XXXXXXXX')" ||
            { BLOCKED_REASON="ssh-remote-setup: mktemp failed on $SSH_HOST"; RWORK=""; }
    fi
    if [ -z "$BLOCKED_REASON" ]; then
        case "$RWORK" in
            /tmp/zcl-xmr-remote-*) ;;
            *) BLOCKED_REASON="ssh-remote-setup: unexpected remote workdir '$RWORK'" ;;
        esac
    fi
    if [ -z "$BLOCKED_REASON" ]; then
        log "transferring verifier + exact inputs to $SSH_HOST:$RWORK"
        TRANSFER_OK=1
        scp "${SSH_OPTS[@]}" -q "$VERIFY_BIN" \
            "$SSH_HOST:$RWORK/zclassic23-package-verify" || TRANSFER_OK=0
        scp "${SSH_OPTS[@]}" -q "$WORK/recipe.wire" \
            "$SSH_HOST:$RWORK/recipe.wire" || TRANSFER_OK=0
        tar czf - -C "$ROOT/packages" "$SHORT" |
            ssh "${SSH_OPTS[@]}" "$SSH_HOST" \
                "mkdir -p '$RWORK/src' && tar xzf - -C '$RWORK/src'" ||
            TRANSFER_OK=0
        while IFS= read -r droot; do
            [ -n "$droot" ] || continue
            tar czf - -C "$STORE/zcode/installed" "$droot" |
                ssh "${SSH_OPTS[@]}" "$SSH_HOST" \
                    "mkdir -p '$RWORK/deps' && tar xzf - -C '$RWORK/deps'" ||
                TRANSFER_OK=0
        done < "$WORK/deps.txt"
        [ "$TRANSFER_OK" -eq 1 ] ||
            BLOCKED_REASON="ssh-transfer: an input transfer to $SSH_HOST failed"
    fi
    if [ -z "$BLOCKED_REASON" ]; then
        UNAME_B="$(ssh "${SSH_OPTS[@]}" "$SSH_HOST" 'uname -srm')"
        HOST_B="$SSH_HOST"
        REMOTE_DEPS=""
        while IFS= read -r droot; do
            [ -n "$droot" ] || continue
            REMOTE_DEPS="$REMOTE_DEPS --dep=$droot,$RWORK/deps/$droot"
        done < "$WORK/deps.txt"
        log "remote confined build on $SSH_HOST"
        RC_B=0
        ssh "${SSH_OPTS[@]}" "$SSH_HOST" \
            "'$RWORK/zclassic23-package-verify' '$DERIVED_ROOT' \
'--zbuild-package-source=$RWORK/src/$SHORT' \
'--zbuild-package-recipe=$RWORK/recipe.wire' \
'--zbuild-package-name=$FULL_NAME' \
'--zbuild-package-profile=standard' \
'--zbuild-package-max-cpu-seconds=120' \
'--emit=$RWORK/emit' '--lock-root=$LOCK_ROOT'$REMOTE_DEPS \
'--plan=$RWORK/plan.json' '--require-full-isolation'" \
            > "$WORK/remote.stdout" 2>"$WORK/remote.stderr" || RC_B=$?
        ssh "${SSH_OPTS[@]}" "$SSH_HOST" \
            "tar czf - -C '$RWORK' emit plan.json" 2>/dev/null |
            tar xzf - -C "$WORK" 2>/dev/null || true
        ssh "${SSH_OPTS[@]}" "$SSH_HOST" "rm -rf -- '$RWORK'" || true
        build_result_ok "$WORK/remote.stdout" "$RC_B" || {
            printf '[cross-machine-reproduce] remote build failed: rc=%s (%s)\n' \
                "$RC_B" "$WORK/remote.stderr" >&2
            exit 5
        }
        SIDE_B_DIR="$WORK"
    fi
else
    MODE="local-double"
    EVIDENCE_LABEL="local-double"
    UNAME_B="$UNAME_LOCAL"
    HOST_B="$HOST_LOCAL"
    mkdir -p "$WORK/control2"
    log "comparison build (second local control — physical_machines=1)"
    RC_B="$(run_local_build "$WORK/control2/emit" "$WORK/control2/plan.json" \
        "$WORK/control2/stdout")"
    build_result_ok "$WORK/control2/stdout" "$RC_B" || {
        printf '[cross-machine-reproduce] comparison build failed: rc=%s (%s.stderr)\n' \
            "$RC_B" "$WORK/control2/stdout" >&2
        exit 5
    }
    SIDE_B_DIR="$WORK/control2"
fi

# ── compare + evidence (zcl.cross_machine_repro.v1) ────────────────────

mkdir -p "$OUTDIR"
EVIDENCE="$OUTDIR/$SHORT-$EVIDENCE_LABEL.json"

VERDICT_RC=0
python3 - "$EVIDENCE" "$FULL_NAME" "$PUB_ROOT" "$RECIPE_ROOT" "$LOCK_ROOT" \
    "$MODE" "$WORK" "$SIDE_B_DIR" "$HOST_LOCAL" "$UNAME_LOCAL" \
    "$HOST_B" "$UNAME_B" "$BLOCKED_REASON" "$DEPS_JSON" <<'PYEOF' || VERDICT_RC=$?
import hashlib, json, os, sys

(evidence_path, full_name, pub_root, recipe_root, lock_root, mode, work,
 side_b_dir, host_a, uname_a, host_b, uname_b, blocked_reason,
 deps_raw) = sys.argv[1:15]

def tree_map(emit):
    out = {}
    for base, _dirs, files in os.walk(emit):
        for f in sorted(files):
            full = os.path.join(base, f)
            rel = os.path.relpath(full, emit)
            if rel == "build-report":
                continue  # embeds toolchain identity; compared separately
            data = open(full, "rb").read()
            out[rel] = {"sha3_256": hashlib.sha3_256(data).hexdigest(),
                        "bytes": len(data)}
    return out

def build_report_bytes(emit):
    p = os.path.join(emit, "build-report")
    return open(p, "rb").read() if os.path.isfile(p) else None

def toolchain(plan_path):
    doc = json.load(open(plan_path))
    return doc.get("toolchain") or {}

def side(host_label, uname_smry, emit, plan_path):
    return {
        "host_label": host_label,
        "uname_smry": uname_smry,
        "toolchain": toolchain(plan_path),
        "outputs": tree_map(emit),
    }

side_a = side(host_a, uname_a,
              os.path.join(work, "control", "emit"),
              os.path.join(work, "control", "plan.json"))

independence = {
    "operator": "same",
    "physical_machines": 1 if mode == "local-double" else 2,
    "operator_groups": 1,
    "note": "same-operator reproduction is not independent-operator "
            "evidence; durably_hosted_loc stays 0 until external operator "
            "evidence exists",
}

doc = {
    "schema": "zcl.cross_machine_repro.v1",
    "package": {"name": full_name, "root": pub_root},
    "recipe_root": recipe_root,
    "lock_root": lock_root,
    "deps": json.loads(deps_raw),
    "mode": mode,
    "side_a": side_a,
    "independence": independence,
}

if blocked_reason:
    doc["verdict"] = "blocked"
    doc["blocked_reason"] = blocked_reason
    doc["side_b"] = None
    doc["outputs_equal"] = None
    doc["toolchain_equal"] = None
    rc = 4
else:
    side_b = side(host_b, uname_b,
                  os.path.join(side_b_dir, "emit"),
                  os.path.join(side_b_dir, "plan.json"))
    rep_a = build_report_bytes(os.path.join(work, "control", "emit"))
    rep_b = build_report_bytes(os.path.join(side_b_dir, "emit"))
    outputs_equal = side_a["outputs"] == side_b["outputs"]
    toolchain_equal = side_a["toolchain"] == side_b["toolchain"]
    doc["side_b"] = side_b
    doc["outputs_equal"] = outputs_equal
    doc["toolchain_equal"] = toolchain_equal
    doc["build_report_equal"] = (
        rep_a is not None and rep_b is not None and rep_a == rep_b)
    doc["output_count"] = len(side_a["outputs"])
    if not toolchain_equal:
        doc["verdict"] = "toolchain_mismatch"
        rc = 7
    elif not outputs_equal:
        doc["verdict"] = "mismatch"
        rc = 6
    else:
        doc["verdict"] = "reproduced"
        rc = 0

with open(evidence_path, "w", encoding="utf-8") as fh:
    json.dump(doc, fh, indent=1)
    fh.write("\n")
print("verdict=%s" % doc["verdict"])
sys.exit(rc)
PYEOF

log "evidence: $EVIDENCE"
case "$VERDICT_RC" in
    0) log "REPRODUCED ($MODE): outputs and toolchain identical" ;;
    4) printf '[cross-machine-reproduce] BLOCKED[ssh]: %s\n' \
        "$BLOCKED_REASON" >&2 ;;
    6) log "MISMATCH: outputs diverged on equal toolchains — see $EVIDENCE" ;;
    7) log "TOOLCHAIN MISMATCH (honest evidence, not a pass): see $EVIDENCE" ;;
esac
exit "$VERDICT_RC"
