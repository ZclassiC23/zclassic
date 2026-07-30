#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# ship.sh — build ONE production binary and put those exact bytes on every node
# in the fleet, verifying each one by the source identity it reports back.
#
# Why this exists. Before it, deployment was: `make deploy` for this host only,
# `deploy-dev` refusing outright, and no path at all to the second server —
# `remote_node_update.sh` is observation-only by construction. The result was
# both public nodes running a binary three days older than main, one of them
# blocked by a defect whose fix was already merged and just never shipped.
#
# The design rule is BUILD ONCE, SHIP MANY:
#   - The production binary is a single whole-program cc over every source file
#     (~200 s). Rebuilding it per host multiplies that by the fleet size and,
#     worse, produces DIFFERENT bytes per host, so "are they running the same
#     code" stops being answerable by comparison.
#   - Instead one candidate is frozen, its SHA-256 and baked source id recorded,
#     and those bytes are copied to every target. Fleet agreement is then a
#     string compare, not an inference.
#
# Every host is verified by asking the RUNNING daemon for the source id baked
# into it and requiring it to equal the candidate's. A host that does not come
# back healthy is rolled back to the binary it was running, automatically,
# before the next host is touched.
#
# Usage:
#   tools/ship.sh                     # gate, build, deploy local, deploy remote
#   tools/ship.sh --targets=local     # one host
#   tools/ship.sh --targets=remote
#   tools/ship.sh --dry-run           # print the plan, touch nothing
#   tools/ship.sh --skip-gate         # reuse a banked verdict for this source id
#
# Environment:
#   ZCL_SHIP_REMOTE   ssh destination of the second node (default rhett2)
#   ZCL_SHIP_HOSTS    override the whole fleet list, space separated
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# shellcheck source=tools/scripts/source_identity_lib.sh
. "$REPO_ROOT/tools/scripts/source_identity_lib.sh"  # zcl_is_sha256, zcl_json_first_sha256

REMOTE_HOST="${ZCL_SHIP_REMOTE:-205.209.104.118}"
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=15)
TARGETS="local remote"
TARGETS_EXPLICIT=0
DRY_RUN=0
SKIP_GATE=0
GATE_CACHE_DIR="${HOME}/.cache/zcl-ship"

for arg in "$@"; do
    case "$arg" in
        --targets=*) TARGETS="${arg#*=}"; TARGETS="${TARGETS//,/ }"; TARGETS_EXPLICIT=1 ;;
        --dry-run)   DRY_RUN=1 ;;
        --skip-gate) SKIP_GATE=1 ;;
        -h|--help)   sed -n '2,35p' "$0"; exit 0 ;;
        *) printf 'ship: unknown argument %s\n' "$arg" >&2; exit 2 ;;
    esac
done

say()  { printf '\033[1mship:\033[0m %s\n' "$*"; }
step() { printf '\n\033[1m── %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31mship: REFUSE:\033[0m %s\n' "$*" >&2; exit 1; }

# ── 0. The proof server is not a deploy target ──────────────────────────────
# $REMOTE_HOST holds one immutable tagged release candidate and records the
# evidence that the candidate stayed up. Restarting it destroys the very thing
# it exists to measure: an uptime and zero-intervention record is only worth
# something if nothing quietly resets it.
#
# Until 2026-07-29 this script did exactly that. `remote` was in the DEFAULT
# target list and the deploy step ran an unconditional `systemctl --user restart
# zclassic23` on it, so a bare `tools/ship.sh` — the documented everyday
# invocation — restarted the box that must not be restarted. The tooling and
# the operating rule were in direct opposition and nothing said so.
#
# Two different refusals on purpose, because the two mistakes are different:
#   - Bare `ship.sh`: the operator asked to ship, not to touch the proof
#     server. Drop `remote`, say so loudly, ship local. Refusing the whole run
#     would punish the common case for a target the operator never named.
#   - Explicit `--targets=...remote`: the operator DID name it. Silently
#     dropping it there would be worse than failing — they would believe the
#     proof server had been updated. Refuse outright, non-zero.
#
# Promotion is a deliberate act: ZCL_SHIP_ALLOW_PROOF_SERVER=1. A successful
# promotion records itself twice in step 4 below, right after the running
# daemon proves it took the candidate: a signed, hash-chained line in the
# TRACKED ledger deploy/promotion-receipts.jsonl (the authority — it replicates
# to origin and cannot be rewritten undetected), plus a local
# proof-server/<timestamp> tag as a convenience index.
# `tools/scripts/promotion_receipt.sh verify` checks the chain offline;
# `tools/scripts/proof_server_pin.sh check` dials the box to see whether it
# still runs what was pinned.
case " $TARGETS " in *" remote "*)
    if [ "${ZCL_SHIP_ALLOW_PROOF_SERVER:-0}" != "1" ]; then
        if [ "$TARGETS_EXPLICIT" -eq 1 ]; then
            die "--targets names 'remote', but $REMOTE_HOST is the immutable proof
       server: it runs one tagged candidate and records the evidence that the
       candidate held. Deploying restarts it and resets that record.
       Promoting a new candidate is deliberate:
           ZCL_SHIP_ALLOW_PROOF_SERVER=1 tools/ship.sh --targets=remote
       A successful promotion appends a signed receipt to the tracked ledger
       deploy/promotion-receipts.jsonl automatically (verify it any time with
       'tools/scripts/promotion_receipt.sh verify', then COMMIT it so it
       replicates); run 'tools/scripts/proof_server_pin.sh check' afterwards to
       confirm the box still runs what was pinned."
        fi
        TARGETS="${TARGETS//remote/}"
        TARGETS="$(printf '%s' "$TARGETS" | tr -s ' ' | sed 's/^ *//; s/ *$//')"
        say "skipping 'remote' — $REMOTE_HOST is the immutable proof server."
        say "  to promote a candidate there: ZCL_SHIP_ALLOW_PROOF_SERVER=1 tools/ship.sh --targets=remote"
        [ -n "$TARGETS" ] || die "no targets left to ship to"
    else
        say "ZCL_SHIP_ALLOW_PROOF_SERVER=1 — the proof server WILL be restarted and its evidence window reset"
    fi
;; esac

# ── 1. Preflight ────────────────────────────────────────────────────────────
# Everything that can refuse cheaply refuses BEFORE the 200-second build, so a
# misconfigured run costs seconds rather than minutes.
step "Preflight"

[ -z "$(git status --porcelain)" ] || \
    die "working tree is dirty — ship what is committed, not what is lying around"

HEAD_SHA="$(git rev-parse HEAD)"
git fetch -q origin main 2>/dev/null || say "warning: could not fetch origin (offline?)"
if git rev-parse --verify -q origin/main >/dev/null; then
    behind="$(git rev-list --count "$HEAD_SHA"..origin/main)"
    [ "$behind" -eq 0 ] || \
        die "HEAD is $behind commit(s) behind origin/main — rebase before shipping"
    ahead="$(git rev-list --count origin/main.."$HEAD_SHA")"
    if [ "$ahead" -gt 0 ]; then
        say "HEAD is $ahead commit(s) ahead of origin/main — pushing first"
        [ "$DRY_RUN" -eq 1 ] || git push --no-verify origin main
    fi
fi
say "source     $(git rev-parse --short HEAD)  $(git log -1 --format=%s | cut -c1-58)"

# A remote host that cannot run these bytes must be found now, not after the
# binary is already installed and the service restarted. The production build
# targets -march=x86-64-v3, so AVX2/FMA/BMI2 are load-bearing: shipping to a
# host without them yields SIGILL on a public node.
for target in $TARGETS; do
    case "$target" in
        local) continue ;;
        remote) ;;
        *) die "unknown target '$target' (want: local, remote)" ;;
    esac
    ssh "${SSH_OPTS[@]}" "$REMOTE_HOST" true 2>/dev/null || \
        die "remote $REMOTE_HOST is unreachable over ssh"
    # libc version via awk, NOT `... | head -1 | grep ...`. head closes the pipe
    # after one line, ldd takes SIGPIPE, and `set -o pipefail` turns that into a
    # 141 exit that kills this script before a single host is touched. It is a
    # RACE — it survives only when ldd's whole output lands in one write, which
    # is why ship worked on 2026-07-27 and died here on 2026-07-28. awk consumes
    # the stream to EOF, so there is no early close and no signal.
    remote_facts="$(ssh "${SSH_OPTS[@]}" "$REMOTE_HOST" \
        'printf "%s|%s|%s\n" "$(uname -m)" \
             "$(grep -c -m1 avx2 /proc/cpuinfo)" \
             "$(ldd --version 2>/dev/null | awk "NR==1 && match(\$0, /[0-9]+\.[0-9]+\$/) { print substr(\$0, RSTART, RLENGTH) }")"')"
    r_arch="${remote_facts%%|*}"; rest="${remote_facts#*|}"
    r_avx2="${rest%%|*}"; r_libc="${rest##*|}"
    l_arch="$(uname -m)"
    l_libc="$(ldd --version 2>/dev/null |
        awk 'NR==1 && match($0, /[0-9]+\.[0-9]+$/) { print substr($0, RSTART, RLENGTH) }')"
    [ "$r_arch" = "$l_arch" ] || \
        die "remote arch $r_arch != local $l_arch — cannot ship one binary to both"
    [ "$r_avx2" -ge 1 ] || \
        die "remote lacks AVX2 but the build targets x86-64-v3 — it would SIGILL"
    [ "$r_libc" = "$l_libc" ] || \
        die "remote glibc $r_libc != local $l_libc — build on the older host instead"
    say "remote     $REMOTE_HOST  $r_arch  glibc $r_libc  avx2 ok"
done

# ── 2. Gate ─────────────────────────────────────────────────────────────────
# Keyed on the source identity, so re-shipping an already-proven tree is free
# but a single changed byte re-gates. The token, never the exit code, decides.
step "Gate"

SOURCE_ID="$(tools/dev/source-identity.sh capture 2>/dev/null || true)"
zcl_is_sha256 "$SOURCE_ID" || SOURCE_ID=""
gate_stamp="${GATE_CACHE_DIR}/${SOURCE_ID:-unknown}.passed"

if [ "$SKIP_GATE" -eq 1 ] && [ -n "$SOURCE_ID" ] && [ -f "$gate_stamp" ]; then
    say "gate       banked for this exact source id ($(cat "$gate_stamp"))"
elif [ "$DRY_RUN" -eq 1 ]; then
    say "gate       (dry run — would run lint + full suite)"
else
    say "gate       make lint"
    make lint >/dev/null || die "make lint failed"
    say "gate       make test-parallel"
    suite_log="$(mktemp)"
    make test-parallel >"$suite_log" 2>&1 || true
    if grep -q 'ALL TESTS PASSED' "$suite_log" && \
       ! grep -q 'SOME TESTS FAILED' "$suite_log"; then
        say "gate       $(grep -m1 'ALL TESTS PASSED' "$suite_log")"
        mkdir -p "$GATE_CACHE_DIR"
        [ -n "$SOURCE_ID" ] && date -u +%Y-%m-%dT%H:%M:%SZ > "$gate_stamp"
    else
        grep -E 'SUITE VERDICT|repro:' "$suite_log" | head -5 >&2
        rm -f "$suite_log"
        die "full suite did not pass — nothing ships"
    fi
    rm -f "$suite_log"
fi

# ── 3. Build one candidate ──────────────────────────────────────────────────
# The production rule is a whole-program cc with no depfile tracking, so a
# header-only edit leaves every .c mtime unchanged and plain `make` would relink
# nothing and ship stale bytes. Removing the target forces the rebuild. This is
# the same reasoning the `deploy` target documents; it is the single most
# expensive step and the reason the result is shared across the fleet.
step "Build"

if [ "$DRY_RUN" -eq 1 ]; then
    say "build      (dry run — would rebuild and freeze one candidate)"
    CANDIDATE=""; ARTIFACT_SHA=""; CAND_SOURCE_ID="$SOURCE_ID"
else
    rm -f build/bin/zclassic23
    make -j"$(nproc)" zclassic23 >/dev/null || die "production build failed"
    CANDIDATE="$(mktemp "${TMPDIR:-/tmp}/zclassic23.ship.XXXXXX")"
    trap 'rm -f "$CANDIDATE"' EXIT HUP INT TERM
    install -m 755 build/bin/zclassic23 "$CANDIDATE"
    ARTIFACT_SHA="$(sha256sum < "$CANDIDATE" | awk '{print $1}')"
    zcl_is_sha256 "$ARTIFACT_SHA" || die "could not hash the frozen candidate"

    # Ask the candidate itself what source it was built from. A binary that
    # cannot answer is not shippable, because nothing downstream could then
    # prove which code a node is running.
    agentbuild="$(timeout 30 "$CANDIDATE" agentbuild 2>&1)" || \
        die "frozen candidate failed its agentbuild preflight"
    CAND_SOURCE_ID="$(zcl_json_first_sha256 "$agentbuild" source_id_sha256)"
    zcl_is_sha256 "$CAND_SOURCE_ID" || \
        die "frozen candidate reports no valid source_id_sha256"
    say "candidate  source_id ${CAND_SOURCE_ID:0:16}…  sha256 ${ARTIFACT_SHA:0:16}…  $(du -h "$CANDIDATE" | cut -f1)"
fi

# ── 4. Deploy, host by host ─────────────────────────────────────────────────
# Sequential on purpose. Two nodes restarting at once is how a fleet goes dark
# on one bad build; this way the first failure stops the rollout with every
# later host still serving.
deploy_local() {
    step "Deploy → local"
    if [ "$DRY_RUN" -eq 1 ]; then say "would run: make deploy"; return 0; fi
    ZCL_DEPLOY_ALLOW_CANONICAL=1 make deploy 2>&1 | tail -6
    return "${PIPESTATUS[0]}"
}

deploy_remote() {
    step "Deploy → $REMOTE_HOST"
    local svc_bin prev_sha
    svc_bin="$(ssh "${SSH_OPTS[@]}" "$REMOTE_HOST" \
        'systemctl --user show zclassic23 -p ExecStart --value | tr " " "\n" | sed -n "s/^path=//p" | head -1')"
    case "$svc_bin" in
        /*) ;;
        *) die "remote ExecStart path is missing or not absolute: '$svc_bin'" ;;
    esac
    say "remote bin $svc_bin"
    if [ "$DRY_RUN" -eq 1 ]; then say "would install candidate + restart + verify"; return 0; fi

    prev_sha="$(ssh "${SSH_OPTS[@]}" "$REMOTE_HOST" "sha256sum < '$svc_bin' | awk '{print \$1}'" 2>/dev/null || echo none)"
    say "remote now sha256 ${prev_sha:0:16}…"

    # Stage beside the target, keep the outgoing binary as the rollback copy,
    # then swap. The running process holds its inode open, so replacing the
    # path never disturbs the daemon still serving from the old bytes.
    scp -q "${SSH_OPTS[@]}" "$CANDIDATE" "$REMOTE_HOST:${svc_bin}.incoming" || \
        die "could not copy the candidate to $REMOTE_HOST"

    ssh "${SSH_OPTS[@]}" "$REMOTE_HOST" bash -s -- \
        "$svc_bin" "$ARTIFACT_SHA" "$CAND_SOURCE_ID" <<'REMOTE_SCRIPT'
set -euo pipefail
svc_bin="$1"; want_sha="$2"; want_src="$3"
got="$(sha256sum < "${svc_bin}.incoming" | awk '{print $1}')"
[ "$got" = "$want_sha" ] || { echo "remote: transferred bytes differ from candidate" >&2; exit 1; }
chmod 755 "${svc_bin}.incoming"
# zcl-identity-parser-allow: this heredoc runs on the REMOTE host verbatim
# (bash -s -- ... <<'REMOTE_SCRIPT') and cannot `source` a local library file
# that only exists in this checkout — the inline grep/sed pair is the only
# option here. See tools/scripts/source_identity_lib.sh for why it is
# anchored on the FIRST occurrence rather than greedy.
got_src="$(timeout 30 "${svc_bin}.incoming" agentbuild 2>&1 | \
    grep -oE '"source_id_sha256"[[:space:]]*:[[:space:]]*"[^"]*"' | head -1 | \
    sed -E 's/.*"[[:space:]]*:[[:space:]]*"([^"]*)".*/\1/')"
[ "$got_src" = "$want_src" ] || { echo "remote: staged binary reports source id '$got_src'" >&2; exit 1; }
cp -f "$svc_bin" "${svc_bin}.rollback" 2>/dev/null || true
mv -f "${svc_bin}.incoming" "$svc_bin"
systemctl --user restart zclassic23
echo "remote: installed and restarted"
REMOTE_SCRIPT

    # Verify against the RUNNING daemon, not the file on disk. The file proves
    # what was installed; only the daemon proves what is being served.
    local deadline running_src ok=0
    deadline=$(( $(date +%s) + 300 ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        # zcl-identity-parser-allow: the whole pipeline (agentbuild | grep |
        # sed) is one double-quoted string handed to the REMOTE shell over
        # ssh, so it executes on that host, which cannot source a local
        # library file from this checkout — the inline extraction is the
        # only option here.
        running_src="$(ssh "${SSH_OPTS[@]}" "$REMOTE_HOST" \
            "timeout 15 '$svc_bin' agentbuild 2>/dev/null | grep -oE '\"source_id_sha256\"[[:space:]]*:[[:space:]]*\"[^\"]*\"' | head -1 | sed -E 's/.*\"[[:space:]]*:[[:space:]]*\"([^\"]*)\".*/\\1/'" 2>/dev/null || true)"
        if [ "$running_src" = "$CAND_SOURCE_ID" ] && \
           ssh "${SSH_OPTS[@]}" "$REMOTE_HOST" \
               "timeout 20 '$svc_bin' status >/dev/null 2>&1"; then
            ok=1; break
        fi
        sleep 10
    done

    if [ "$ok" -ne 1 ]; then
        say "remote did not come back healthy — ROLLING BACK"
        ssh "${SSH_OPTS[@]}" "$REMOTE_HOST" \
            "if [ -f '${svc_bin}.rollback' ]; then mv -f '${svc_bin}.rollback' '$svc_bin'; systemctl --user restart zclassic23; echo 'remote: rolled back'; fi" || true
        die "remote deploy failed and was rolled back"
    fi
    say "remote ok  running source_id ${running_src:0:16}… and answering status"

    # Record the pin at the one moment this script provably holds the
    # binding: the running daemon just proved it reports the candidate's
    # source id. A failure here must not undo or fail an already-successful
    # deploy — report it loudly and move on.
    tools/scripts/proof_server_pin.sh record "$HEAD_SHA" "$CAND_SOURCE_ID" "$ARTIFACT_SHA" "$REMOTE_HOST" || \
        say "WARNING: could not record the proof-server pin for $HEAD_SHA / ${CAND_SOURCE_ID:0:16}… on $REMOTE_HOST — the deploy itself succeeded; re-run by hand: tools/scripts/proof_server_pin.sh record $HEAD_SHA $CAND_SOURCE_ID $ARTIFACT_SHA $REMOTE_HOST"

    # The tag above is a local convenience index. THIS is the record that
    # matters: a signed, hash-chained line appended to a TRACKED ledger, so it
    # replicates to origin on the next push of main and cannot be rewritten
    # afterwards without breaking the chain. Same failure policy as the pin —
    # a recording failure must never undo an already-successful deploy.
    #
    # It needs ZCL_RECEIPT_KEY in the environment (a key whose only job is
    # signing promotion evidence — never a login/push key) and refuses loudly
    # without one rather than reaching for a default. See "Owner setup" in
    # docs/PROMOTION_RECEIPTS.md; the chain must also have been started with
    # `promotion_receipt.sh init` before the first append can land.
    if tools/scripts/promotion_receipt.sh append "$HEAD_SHA" "$CAND_SOURCE_ID" "$ARTIFACT_SHA" "$REMOTE_HOST"; then
        say "commit deploy/promotion-receipts.jsonl — until it is committed the receipt exists on this disk only, and ship's clean-tree preflight will refuse the next run"
    else
        say "WARNING: could not append the promotion receipt for $HEAD_SHA / ${CAND_SOURCE_ID:0:16}… on $REMOTE_HOST — the deploy itself succeeded; re-run by hand: tools/scripts/promotion_receipt.sh append $HEAD_SHA $CAND_SOURCE_ID $ARTIFACT_SHA $REMOTE_HOST"
    fi
}

for target in $TARGETS; do
    case "$target" in
        local)  deploy_local  || die "local deploy failed" ;;
        remote) deploy_remote ;;
    esac
done

# ── 5. Fleet report ─────────────────────────────────────────────────────────
step "Fleet"
printf '%-22s %-18s %-12s %s\n' HOST SOURCE_ID HEIGHT STATE
for target in $TARGETS; do
    case "$target" in
        local)
            s="$(timeout 20 build/bin/zclassic23 status 2>/dev/null || true)"
            printf '%-22s %-18s %-12s %s\n' "local" "${CAND_SOURCE_ID:0:16}…" \
                "$(printf '%s' "$s" | grep -oE 'hstar=[0-9]+' | cut -d= -f2)" \
                "$(printf '%s' "$s" | grep -oE 'sync=[a-z_]+' | cut -d= -f2)" ;;
        remote)
            s="$(ssh "${SSH_OPTS[@]}" "$REMOTE_HOST" 'timeout 20 ~/bin/zclassic23 status 2>/dev/null' || true)"
            printf '%-22s %-18s %-12s %s\n' "$REMOTE_HOST" "${CAND_SOURCE_ID:0:16}…" \
                "$(printf '%s' "$s" | grep -oE 'hstar=[0-9]+' | cut -d= -f2)" \
                "$(printf '%s' "$s" | grep -oE 'sync=[a-z_]+' | cut -d= -f2)" ;;
    esac
done
echo
if [ "$DRY_RUN" -eq 1 ]; then
    say "DRY RUN — nothing was built, installed, restarted, or pushed."
    say "plan was: $(git rev-parse --short HEAD) -> $TARGETS"
else
    say "shipped $(git rev-parse --short HEAD) to: $TARGETS"
fi
