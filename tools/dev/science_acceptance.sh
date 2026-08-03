# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# science_acceptance.sh — the v1 acceptance proof for the ZCODE
# scientific-metaverse slice (S3 science store, S4 closed executors,
# S5 discovery projection), per /tmp/acceptance-proof-plan.md:
#
#   two clean nodes, no GitHub, preregister and run a C23 benchmark,
#   reproduce it, publish findings/review, rank it locally, restart both
#   nodes, and reconstruct every object and receipt from hashes.
#
# Topology (two disjoint isolated nodes sharing NO datadir, loopback only):
#
#   Node A (author/executor): listens on $A_PORT, dead -connect sink; its
#       package store is seeded (real vcs_package_store_put_* APIs) with a
#       small content.v2 package, and its workspace CAS with the execution
#       context (the context objects have NO command-leaf admission path —
#       the landed tests seed them with vcs_object_put_addressed; the
#       fixture tool does exactly that, field-for-field).
#   Node B (second clean node): -connect=127.0.0.1:$A_PORT only. Same
#       lifecycle with an independent study (different fixture salt).
#
# What this script PROVES (each step asserts before proceeding):
#   [1] two-node loopback topology, exactly A<->B, nothing off-host.
#   [2] A: study plan -> commit -> show/list; workspace CAS holds the wire.
#   [3] A: confined c23.benchmark.v1 execute (sandbox self-check gate) ->
#       COMMITTED receipt; work.receipt re-verifies the row against CAS.
#   [4] A: reproduction via the v1 mirror (the executor refuses non-v1
#       originals by name; the S4 test hand-builds its v1 original the
#       same way) -> reproduction.v1 COMMITTED; roots differ, same
#       study/task/candidate binding (action equality enforced inside the
#       executor: executor-action-mismatch).
#   [5] A: findings -> review.submit (a stale review predating findings is
#       REFUSED with science-review-predates-findings) -> curation vote.
#   [6] A: zcode.science.discover renders with explanation (mass,
#       direct_citations, seed_weight), corpus/graph/seed-set roots,
#       truncation flag; rank.snapshot agrees.
#   [7] B: the SAME lifecycle independently (proves a second clean node).
#   [8] GAP ASSERTIONS: A's science objects never reached B (CAS absent,
#       projection found=false, B's execute against A's study refuses
#       executor-study-not-in-cas) — see NAMED GAPS below.
#   [9] PACKAGE LEG: B fetches A's package over the zpkgswm swarm
#       (download record pre-seeded via the one-shot fetch leaf, the
#       node's own resume path). Bounded wait; either it completes
#       (verified byte-for-byte against the root) or it stalls in
#       WANT_MANIFEST — see NAMED GAPS.
#   [10] SIGTERM both nodes, cold boot same datadirs, topology re-forms.
#   [11] HEADLINE: on both nodes — snapshot the science projection, run
#        zcode.science.rebuild (drop + re-derive from CAS), snapshot again:
#        byte-identical. Then DELETE the six projection tables directly
#        (python3 sqlite3, never touching .zvcs/objects), prove study.show
#        goes found=false, rebuild again, byte-identical once more. CAS
#        object count unchanged throughout.
#
# NAMED GAPS (asserted, not worked around — a named gap is a deliverable):
#   G1  Science CAS objects (study_spec, results, reproductions, findings,
#       reviews, votes) have NO node-to-node distribution: the zpkgswm
#       swarm serves only content.v2 package manifests/chunks out of the
#       package store, and nothing admits science wires into that store or
#       carries a science root to a peer. Therefore the plan's "B resolves
#       the study closure FROM A" and "B reproduces A's benchmark" cannot
#       run node-to-node today; B runs an independent lifecycle instead.
#   G2  CLOSED. The fresh-node package fetch stalled for three stacked
#       reasons, each fixed and covered by this proof's package leg:
#       (a) the frozen policy table gave NEW_USER 0 announces/hour, so the
#           receiver flood-refused the very first ANNOUNCE — now a 4/hour
#           bootstrap quota (VCS_POLICY_FREE_ANNOUNCE_PER_HOUR);
#       (b) announces were only queued when a peer was first added, so
#           content published after the handshake never propagated — the
#           per-sync membership sweep now re-announces to every known
#           peer, deduped per peer in the engine;
#       (c) the swarm tick only fired from the per-peer message cycle,
#           so an idle-but-healthy connection got ZERO ticks — no sync, no
#           announce, no WANT, no drain. The swarm is now clock-driven by
#           a supervisor child (net.zcode_swarm, 1 s period) registered in
#           boot_zcode_swarm_wire; the message-cycle hook survives only as
#           a send-latency fast path.
#   G3  zcode_science_rebuild had no operator surface (test-only
#       callers); this proof lands the zcode.science.rebuild leaf as the
#       sanctioned additive glue.
#   G4  findings.v1 and the execution-context objects (env policy,
#       workload, task, candidate, method) have no command-leaf admission
#       path; seeded via tools/zcode_science_fixture.c through the same
#       codecs/CAS APIs the landed tests use. Also: the deterministic
#       now_unix pin (and the science int keys) had no input-validator
#       rule, so the leaves were uninvokable from the shell — fixed in
#       lib/kernel/src/command_registry.c as sanctioned glue.
#
# SAFETY: mirrors isolated_node_env.sh / two_node_peer_tip.sh —
#   test-tmp/-only datadirs (mktemp -d), 39xxx isolation ports probed with
#   ss(8) against the live refuse-set AND the LISTEN table, setsid process
#   groups killed on EXIT/INT/TERM, never near 8033/18232.
#
# Run:  make test-science-acceptance   (opt-in; NOT in `make ci` — spawns
# two real node processes and needs the host Landlock/seccomp confinement
# backend, same opt-in class as test-two-node-peer-tip.)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
NODE_BIN="${ZCL_NODE_BIN:-$REPO_ROOT/build/bin/zclassic23}"
RPC_BIN="${ZCL_RPC_BIN:-$REPO_ROOT/build/bin/zcl-rpc}"

SA_LIVE_PORTS="8023 8033 8034 8035 8043 8044 8045 8046 8232 8443 \
18034 18232 18234 18243 18244 18245 18246"

A_PORT=39110; A_RPC=39111; A_FS=39112; A_HTTPS=39113
B_PORT=39120; B_RPC=39121; B_FS=39122; B_HTTPS=39123
DEAD_SINK=39999
RPC_WARMUP="${RPC_WARMUP:-90}"     # per-node RPC warmup budget (s)
PKG_WAIT="${PKG_WAIT:-75}"         # swarm fetch budget (s)

# Deterministic submission pins (fixture windows: study 1000..5000,
# findings 1800, stale review 1700, fresh review 1900, vote expires 5000).
NOW_STUDY=1500
NOW_REPRO=1600
NOW_REVIEW=1900

SA_DD_A=""; SA_DD_B=""; SA_WORK=""
SA_PGID_A=""; SA_PGID_B=""
SA_CLEANED=0
SA_STEP_START=$(date +%s)

sa_die() { echo "science-acceptance: FATAL: $*" >&2; exit 2; }
sa_step() {
    local now; now=$(date +%s)
    echo "science-acceptance: [$1] (t+$((now - SA_STEP_START))s) $2"
    SA_STEP_START=$now
}

sa_assert_not_live_port() {
    local p="$1" lp
    for lp in $SA_LIVE_PORTS; do
        [ "$p" = "$lp" ] && sa_die "port $p is in the live refuse-set — refusing"
    done
    return 0
}
sa_assert_port_free() {
    local p="$1"
    if ss -tlnH "sport = :$p" 2>/dev/null | grep -q .; then
        sa_die "port $p is already LISTENING — refusing (operator port math is wrong)"
    fi
    return 0
}

sa_kill_group() {
    local pgid="$1" sig="${2:-TERM}"
    [ -n "$pgid" ] || return 0
    kill -"$sig" "-$pgid" 2>/dev/null || true
    local i
    for i in $(seq 1 50); do
        kill -0 "-$pgid" 2>/dev/null || break
        sleep 0.2
    done
    kill -KILL "-$pgid" 2>/dev/null || true
}
sa_rm_dir() {
    local dd="$1"
    [ -n "$dd" ] && [ -d "$dd" ] || return 0
    case "$dd" in
        "$REPO_ROOT"/test-tmp/zcl23-sciacc-*) rm -rf "$dd" 2>/dev/null || true ;;
        *) echo "science-acceptance: WARN: refusing to rm non-scratch dir '$dd'" >&2 ;;
    esac
}
sa_cleanup() {
    [ "$SA_CLEANED" = "1" ] && return 0
    SA_CLEANED=1
    sa_kill_group "$SA_PGID_A"
    sa_kill_group "$SA_PGID_B"
    [ -n "$SA_DD_A" ] && pkill -KILL -f -- "-datadir=$SA_DD_A" 2>/dev/null || true
    [ -n "$SA_DD_B" ] && pkill -KILL -f -- "-datadir=$SA_DD_B" 2>/dev/null || true
    sa_rm_dir "$SA_DD_A"
    sa_rm_dir "$SA_DD_B"
    sa_rm_dir "$SA_WORK"
}

sa_rpc() { # $1=datadir $2=rpcport $3.. = method/args
    local dd="$1" rp="$2"; shift 2
    ZCL_DATADIR="$dd" ZCL_RPCPORT="$rp" "$RPC_BIN" "$@" 2>/dev/null || true
}
a_rpc() { sa_rpc "$SA_DD_A" "$A_RPC" "$@"; }
b_rpc() { sa_rpc "$SA_DD_B" "$B_RPC" "$@"; }

sa_peer_count() { # $1=datadir $2=rpcport → integer peer count
    sa_rpc "$1" "$2" getpeerinfo | python3 -c \
        'import json,sys
try:
    d = json.load(sys.stdin)
    print(len(d.get("result") or []))
except Exception:
    print(-1)'
}

sa_spawn() { # $1=datadir $2=p2p $3=rpc $4=fs $5=https $6=connect-target
    local dd="$1" p2p="$2" rpc="$3" fs="$4" https="$5" conn="$6"
    setsid "$NODE_BIN" \
        -datadir="$dd" -regtest \
        -port="$p2p" -rpcport="$rpc" -fsport="$fs" -httpsport="$https" \
        -connect="$conn" -packagehost=1 \
        -nobgvalidation -nolegacyimport -nofilesync -showmetrics=0 \
        >"$dd/node.log" 2>&1 &
    echo "$!"   # PID == PGID (setsid leader)
}

sa_wait_rpc() { # $1=dd $2=rpc $3=pid $4=secs
    local dd="$1" rp="$2" pid="$3" secs="$4" deadline t
    deadline=$(( $(date +%s) + secs ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "science-acceptance: node (pid $pid) exited during warmup (see $dd/node.log)" >&2
            return 1
        fi
        if [ -f "$dd/.cookie" ]; then
            t="$(sa_rpc "$dd" "$rp" getblockcount | tr -dc '0-9-')"
            [ -n "$t" ] && return 0
        fi
        sleep 0.5
    done
    return 1
}

# ── science leaf driver + assertions ──────────────────────────────────
# sa_sci <datadir> <leaf> <json> → prints the reply's one JSON line.
sa_sci() {
    local dd="$1" leaf="$2" input="$3"
    # The CLI exits non-zero whenever the reply is ok:false — expected for
    # the refusal assertions (stale review, B-against-A's-study). Always
    # return 0; every reply is asserted on its JSON content.
    "$NODE_BIN" "$leaf" -datadir="$dd" --input="$input" 2>/dev/null | tail -1 || true
}
# jget '<python expr over d>' — reads one JSON doc on stdin.
jget() { python3 -c "import json,sys
d = json.load(sys.stdin)
print($1)"; }
sa_jget() { echo "$1" | jget "$2"; }

# ── fixture tool compile ───────────────────────────────────────────────
sa_build_fixture() {
    cc -std=c23 -O1 -w -D_GNU_SOURCE \
        -I"$REPO_ROOT/lib/vcs/include" -I"$REPO_ROOT/lib/base/include" \
        -I"$REPO_ROOT/lib/crypto/include" -I"$REPO_ROOT/lib/json/include" \
        -I"$REPO_ROOT/lib/util/include" -I"$REPO_ROOT/lib/platform/include" \
        -I"$REPO_ROOT/lib/support/include" \
        -o "$SA_WORK/zcode_science_fixture" \
        "$REPO_ROOT/tools/zcode_science_fixture.c" \
        "$REPO_ROOT/lib/vcs/src/zcode_science.c" \
        "$REPO_ROOT/lib/vcs/src/zcode_dev.c" \
        "$REPO_ROOT/lib/vcs/src/zcode_benchmark_receipt.c" \
        "$REPO_ROOT/lib/vcs/src/vcs_object.c" \
        "$REPO_ROOT/lib/vcs/src/package_store.c" \
        "$REPO_ROOT/lib/vcs/src/package_store_io.c" \
        "$REPO_ROOT/lib/vcs/src/package_manifest.c" \
        "$REPO_ROOT/lib/vcs/src/build_action.c" \
        "$REPO_ROOT/lib/crypto/src/sha3.c" \
        "$REPO_ROOT/lib/crypto/src/ed25519.c" \
        "$REPO_ROOT/lib/crypto/src/sha512.c" \
        "$REPO_ROOT/lib/crypto/src/sha256.c" \
        "$REPO_ROOT/lib/base/src/safe_alloc.c" \
        "$REPO_ROOT/lib/base/src/log_level.c" \
        "$REPO_ROOT/lib/base/src/result.c" \
        "$REPO_ROOT/lib/support/src/cleanse.c" \
        "$REPO_ROOT/lib/platform/src/clock.c" \
        "$REPO_ROOT/lib/json/src/json.c" \
        "$REPO_ROOT/lib/util/src/hw_profile.c" \
        "$REPO_ROOT/lib/util/src/spawn.c" \
        "$REPO_ROOT/lib/util/src/cpu_topology.c" 2>/dev/null \
        || sa_die "fixture tool compile failed"
}

FIX=""   # fixture tool path
CH_HASH="abababababababababababababababababababababababababababababababab"
REPRO_PUB="cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"

# Full single-node science lifecycle. $1=datadir $2=salt $3=label.
# Sets globals: L_STUDY L_TASK L_CAND L_METHOD L_RA L_V1 L_PA L_RB L_FR L_RR L_VID
run_lifecycle() {
    local dd="$1" salt="$2" label="$3" out
    eval "$("$FIX" seed-context "$dd/zcode" "$salt" | grep -E 'ROOT=|WIRE=')"
    L_STUDY="$STUDY_ROOT"; L_TASK="$TASK_ROOT"; L_CAND="$CANDIDATE_ROOT"
    L_METHOD="$METHOD_ROOT"

    # study plan -> commit -> show/list
    out="$(sa_sci "$dd" zcode.science.study.plan \
        "{\"wire_hex\":\"$STUDY_WIRE\",\"now_unix\":$NOW_STUDY}")"
    [ "$(sa_jget "$out" 'd["ok"]')" = "True" ] || sa_die "$label study.plan refused: $out"
    out="$(sa_sci "$dd" zcode.science.study.commit \
        "{\"wire_hex\":\"$STUDY_WIRE\",\"confirm\":true,\"now_unix\":$NOW_STUDY}")"
    [ "$(sa_jget "$out" 'd["ok"]')" = "True" ] || sa_die "$label study.commit refused: $out"
    [ "$(sa_jget "$out" 'd["data"]["study_root"]')" = "$L_STUDY" ] \
        || sa_die "$label study root drifted"
    out="$(sa_sci "$dd" zcode.science.study.show \
        "{\"study_root\":\"$L_STUDY\"}")"
    [ "$(sa_jget "$out" 'd["data"]["found"]')" = "True" ] \
        || sa_die "$label study.show found=false after commit"
    [ "$("$FIX" cas-has "$dd/zcode" "$L_STUDY" | grep -c 'PRESENT=1')" = "1" ] \
        || sa_die "$label study wire absent from CAS after commit"
    echo "science-acceptance:     $label study committed: ${L_STUDY:0:16}…"

    # confined benchmark execute (the sandbox self-check gate is inside)
    out="$(sa_sci "$dd" zcode.science.work.execute \
        "{\"study_root\":\"$L_STUDY\",\"task_root\":\"$L_TASK\",\"candidate_root\":\"$L_CAND\",\"method_root\":\"$L_METHOD\",\"challenge_block_height\":3200000,\"challenge_block_hash\":\"$CH_HASH\",\"confirm\":true,\"now_unix\":$NOW_STUDY}")"
    [ "$(sa_jget "$out" 'd["ok"]')" = "True" ] \
        || sa_die "$label work.execute refused (confinement gate?): $out"
    [ "$(sa_jget "$out" 'd["data"]["state"]')" = "COMMITTED" ] \
        || sa_die "$label execute did not commit"
    L_RA="$(sa_jget "$out" 'd["data"]["result_root"]')"
    local raw_root payload_root ev_root
    raw_root="$(sa_jget "$out" 'd["data"]["raw_sample_root"]')"
    payload_root="$(sa_jget "$out" 'd["data"]["sample_payload_root"]')"
    ev_root="$(sa_jget "$out" 'd["data"]["evidence_root"]')"
    out="$(sa_sci "$dd" zcode.science.work.receipt "{\"root\":\"$L_RA\"}")"
    [ "$(sa_jget "$out" 'd["data"]["cas_verified"]')" = "True" ] \
        || sa_die "$label receipt failed CAS verification"
    [ "$(sa_jget "$out" 'd["data"]["study_root"]')" = "$L_STUDY" ] \
        || sa_die "$label receipt bound the wrong study"
    echo "science-acceptance:     $label benchmark committed: ${L_RA:0:16}… (samples ${raw_root:0:12}… payload ${payload_root:0:12}… evidence ${ev_root:0:12}…)"

    # reproduction against the v1 mirror of the real observation
    eval "$("$FIX" v1mirror "$dd/zcode" "$L_RA")"
    L_V1="$V1_ROOT"
    out="$(sa_sci "$dd" zcode.science.work.execute \
        "{\"method_root\":\"$L_METHOD\",\"original_result_root\":\"$L_V1\",\"reproducer_pubkey\":\"$REPRO_PUB\",\"action_kind\":\"c23.benchmark.reproduce.v1\",\"challenge_block_height\":3200000,\"challenge_block_hash\":\"$CH_HASH\",\"confirm\":true,\"now_unix\":$NOW_REPRO}")"
    [ "$(sa_jget "$out" 'd["ok"]')" = "True" ] \
        || sa_die "$label reproduction execute refused: $out"
    L_PA="$(sa_jget "$out" 'd["data"]["reproduction_root"]')"
    L_RB="$(sa_jget "$out" 'd["data"]["reproduced_result_root"]')"
    local verdict
    verdict="$(sa_jget "$out" 'd["data"]["verdict"]')"
    [ "$L_RB" != "$L_RA" ] || sa_die "$label reproduced root equals original root"
    out="$(sa_sci "$dd" zcode.science.work.status "{\"root\":\"$L_PA\"}")"
    [ "$(sa_jget "$out" 'd["data"]["study_root"]')" = "$L_STUDY" ] \
        || sa_die "$label reproduction bound the wrong study"
    [ "$(sa_jget "$out" 'd["data"]["link_root"]')" = "$L_V1" ] \
        || sa_die "$label reproduction bound the wrong original"
    [ "$(sa_jget "$out" 'd["data"]["aux_root"]')" = "$L_RB" ] \
        || sa_die "$label reproduction bound the wrong reproduced root"
    echo "science-acceptance:     $label reproduction committed: ${L_PA:0:16}… verdict=$verdict (1=replicated 2=contradicted 3=inconclusive; any is a valid observation)"

    # findings -> stale review REFUSED -> fresh review -> vote
    eval "$("$FIX" mkfindings "$dd/zcode" "$L_STUDY" "$L_TASK" "$L_CAND" "$L_RA" 1800 "$salt")"
    L_FR="$FINDINGS_ROOT"
    eval "$("$FIX" mkreview "$L_TASK" "$L_CAND" "$L_FR" 1700 1 "$salt")"
    sa_sci "$dd" zcode.science.review.submit \
        "{\"wire_hex\":\"$REVIEW_WIRE\",\"now_unix\":$NOW_REVIEW}" >/dev/null
    out="$(sa_sci "$dd" zcode.science.review.submit \
        "{\"wire_hex\":\"$REVIEW_WIRE\",\"confirm\":true,\"now_unix\":$NOW_REVIEW}")"
    [ "$(sa_jget "$out" 'd["ok"]')" = "False" ] \
        || sa_die "$label stale review was ACCEPTED — the freshness rule is broken"
    [ "$(sa_jget "$out" 'd["error"]["message"]')" = "science-review-predates-findings" ] \
        || sa_die "$label stale review refused with the wrong rule: $out"
    echo "science-acceptance:     $label stale review refused by name (science-review-predates-findings)"
    eval "$("$FIX" mkreview "$L_TASK" "$L_CAND" "$L_FR" 1900 1 "$salt")"
    sa_sci "$dd" zcode.science.review.submit \
        "{\"wire_hex\":\"$REVIEW_WIRE\",\"now_unix\":$NOW_REVIEW}" >/dev/null
    out="$(sa_sci "$dd" zcode.science.review.submit \
        "{\"wire_hex\":\"$REVIEW_WIRE\",\"confirm\":true,\"now_unix\":$NOW_REVIEW}")"
    [ "$(sa_jget "$out" 'd["ok"]')" = "True" ] || sa_die "$label fresh review refused: $out"
    L_RR="$(sa_jget "$out" 'd["data"]["review_root"]')"
    eval "$("$FIX" mkvote "$L_STUDY" 5000 "$salt")"
    sa_sci "$dd" zcode.science.vote.submit \
        "{\"wire_hex\":\"$VOTE_WIRE\",\"now_unix\":$NOW_STUDY,\"network_genesis_root\":\"$GENESIS_ROOT\",\"voter_zid_root\":\"$VOTER_ZID_ROOT\",\"signer_pubkey\":\"$SIGNER_PUBKEY\"}" >/dev/null
    out="$(sa_sci "$dd" zcode.science.vote.submit \
        "{\"wire_hex\":\"$VOTE_WIRE\",\"confirm\":true,\"now_unix\":$NOW_STUDY,\"network_genesis_root\":\"$GENESIS_ROOT\",\"voter_zid_root\":\"$VOTER_ZID_ROOT\",\"signer_pubkey\":\"$SIGNER_PUBKEY\"}")"
    [ "$(sa_jget "$out" 'd["ok"]')" = "True" ] || sa_die "$label vote refused: $out"
    L_VID="$(sa_jget "$out" 'd["data"]["vote_id"]')"
    echo "science-acceptance:     $label findings ${L_FR:0:12}… review ${L_RR:0:12}… vote ${L_VID:0:12}… committed"

    # local discovery over the committed corpus
    out="$(sa_sci "$dd" zcode.science.discover \
        "{\"category\":\"active\",\"max\":16,\"now_unix\":$NOW_STUDY}")"
    [ "$(sa_jget "$out" 'd["ok"]')" = "True" ] || sa_die "$label discover failed: $out"
    [ "$(sa_jget "$out" 'd["data"]["count"]')" -ge 1 ] \
        || sa_die "$label discover rendered an empty corpus over a committed study"
    echo "$out" | python3 -c '
import json, sys
d = json.load(sys.stdin)["data"]
assert d["corpus_root"] and d["graph_root"] and d["seed_set_root"]
assert isinstance(d["truncated"], bool)
e = d["entries"][0]
for k in ("property_root", "mass", "mass_share_millionths", "direct_citations", "seed_weight"):
    assert k in e, k
print("science-acceptance:     '"$label"' discover: %d entr%s, corpus %s…, truncated=%s" % (
    d["count"], "y" if d["count"] == 1 else "ies",
    d["corpus_root"][:16], d["truncated"]))' \
        || sa_die "$label discover output lacks explanation/roots/truncation"
}

# Projection snapshot for the rebuild-equivalence proof. $1=datadir
# $2=study $3=ra $4=pa $5=outfile
snap_projection() {
    local dd="$1" study="$2" ra="$3" pa="$4" outfile="$5"
    {
        sa_sci "$dd" zcode.science.study.list '{"max":32}'
        sa_sci "$dd" zcode.science.study.show "{\"study_root\":\"$study\"}"
        sa_sci "$dd" zcode.science.work.status "{\"root\":\"$ra\"}"
        sa_sci "$dd" zcode.science.work.status "{\"root\":\"$pa\"}"
        sa_sci "$dd" zcode.science.work.receipt "{\"root\":\"$ra\"}"
        sa_sci "$dd" zcode.science.work.receipt "{\"root\":\"$pa\"}"
        sa_sci "$dd" zcode.science.discover "{\"category\":\"active\",\"max\":16,\"now_unix\":$NOW_STUDY}"
        sa_sci "$dd" zcode.science.rank.snapshot "{\"workspace\":\"$dd/zcode\",\"now_unix\":$NOW_STUDY}"
    } | python3 -c '
import json, sys
docs = []
for line in sys.stdin:
    line = line.strip()
    if line.startswith("{"):
        d = json.loads(line)
        docs.append((d.get("command"), json.dumps(d.get("data"), sort_keys=True)))
for cmd, blob in docs:
    print(cmd, blob)' > "$outfile"
}

cas_object_count() { # $1=datadir
    find "$1/zcode/.zvcs/objects" -type f -not -path "*/tmp/*" | wc -l
}

sql_wipe_projection() { # $1=datadir — the six rebuildable tables ONLY.
    python3 - "$1/node.db" <<'EOF'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1], timeout=30)
for t in ("zcode_science_studies", "zcode_science_results",
          "zcode_science_reproductions", "zcode_science_findings",
          "zcode_science_votes", "zcode_science_reviews"):
    db.execute(f"DELETE FROM {t}")
db.commit()
db.close()
EOF
}

# Rebuild + wipe + rebuild proof on one node. $1=datadir $2=label
# $3=study $4=ra $5=pa
rebuild_proof() {
    local dd="$1" label="$2" study="$3" ra="$4" pa="$5" out
    local before after
    before="$(mktemp)"; after="$(mktemp)"
    snap_projection "$dd" "$study" "$ra" "$pa" "$before"
    out="$(sa_sci "$dd" zcode.science.rebuild "{\"now_unix\":$NOW_STUDY}")"
    [ "$(sa_jget "$out" 'd["ok"]')" = "True" ] || sa_die "$label rebuild failed: $out"
    echo "science-acceptance:     $label rebuild counts: $(sa_jget "$out" 'd["data"]')"
    snap_projection "$dd" "$study" "$ra" "$pa" "$after"
    diff -q "$before" "$after" >/dev/null \
        || { diff "$before" "$after" | head -10; sa_die "$label projection changed across rebuild"; }
    echo "science-acceptance:     $label rebuild-equivalence: projection byte-identical after drop+rebuild"

    # The projection is disposable: wipe the six tables directly, prove the
    # reads go empty, rebuild from the CAS again, byte-identical again.
    local objs_before objs_after
    objs_before="$(cas_object_count "$dd")"
    sql_wipe_projection "$dd"
    out="$(sa_sci "$dd" zcode.science.study.show "{\"study_root\":\"$study\"}")"
    [ "$(sa_jget "$out" 'd["data"]["found"]')" = "False" ] \
        || sa_die "$label study.show still served after the SQL wipe"
    echo "science-acceptance:     $label SQL projection wiped (6 tables; .zvcs/objects untouched): study.show found=false"
    out="$(sa_sci "$dd" zcode.science.rebuild "{\"now_unix\":$NOW_STUDY}")"
    [ "$(sa_jget "$out" 'd["ok"]')" = "True" ] || sa_die "$label rebuild after wipe failed: $out"
    snap_projection "$dd" "$study" "$ra" "$pa" "$after"
    diff -q "$before" "$after" >/dev/null \
        || { diff "$before" "$after" | head -10; sa_die "$label projection did not reconstruct identically from CAS"; }
    objs_after="$(cas_object_count "$dd")"
    [ "$objs_before" = "$objs_after" ] \
        || sa_die "$label CAS object count changed ($objs_before -> $objs_after) across wipe+rebuild"
    echo "science-acceptance:     $label reconstructed from hashes: byte-identical after SQL wipe; CAS object count stable at $objs_after"
    rm -f "$before" "$after"
}

# ── preflight ──────────────────────────────────────────────────────────
command -v ss      >/dev/null 2>&1 || sa_die "ss(8) not found (need iproute2)"
command -v mktemp  >/dev/null 2>&1 || sa_die "mktemp not found"
command -v python3 >/dev/null 2>&1 || sa_die "python3 not found (JSON + sqlite3 glue)"
command -v cc      >/dev/null 2>&1 || sa_die "cc not found (fixture tool compile)"
[ -x "$NODE_BIN" ] || sa_die "$NODE_BIN not built — run make first"
[ -x "$RPC_BIN" ]  || sa_die "$RPC_BIN not built — run make zcl-rpc"

for p in "$A_PORT" "$A_RPC" "$A_FS" "$A_HTTPS" \
         "$B_PORT" "$B_RPC" "$B_FS" "$B_HTTPS" "$DEAD_SINK"; do
    sa_assert_not_live_port "$p"
done

SA_DD_A="$(mktemp -d "$REPO_ROOT/test-tmp/zcl23-sciacc-A-XXXXXX")" || sa_die "mktemp A failed"
SA_DD_B="$(mktemp -d "$REPO_ROOT/test-tmp/zcl23-sciacc-B-XXXXXX")" || sa_die "mktemp B failed"
SA_WORK="$(mktemp -d "$REPO_ROOT/test-tmp/zcl23-sciacc-W-XXXXXX")" || sa_die "mktemp W failed"
case "$SA_DD_A" in "$REPO_ROOT"/test-tmp/zcl23-sciacc-A-*) : ;; *) sa_die "bad A datadir $SA_DD_A" ;; esac
case "$SA_DD_B" in "$REPO_ROOT"/test-tmp/zcl23-sciacc-B-*) : ;; *) sa_die "bad B datadir $SA_DD_B" ;; esac
if [ -n "${HOME:-}" ]; then
    case "$SA_DD_A" in "$HOME"/.zclassic-c23*) sa_die "A datadir under live tree — refusing" ;; esac
    case "$SA_DD_B" in "$HOME"/.zclassic-c23*) sa_die "B datadir under live tree — refusing" ;; esac
fi

trap sa_cleanup EXIT INT TERM

for p in "$A_PORT" "$A_RPC" "$A_FS" "$A_HTTPS" \
         "$B_PORT" "$B_RPC" "$B_FS" "$B_HTTPS"; do
    sa_assert_port_free "$p"
done

mkdir -p "$SA_DD_A/zcode" "$SA_DD_B/zcode"
sa_build_fixture
FIX="$SA_WORK/zcode_science_fixture"
echo "science-acceptance: A{dd=$SA_DD_A p2p=$A_PORT rpc=$A_RPC} B{dd=$SA_DD_B p2p=$B_PORT rpc=$B_RPC}"

# ── [0] seed stores before any boot ───────────────────────────────────
sa_step 0 "seed A's package store (real store APIs) + B's download record (one-shot fetch, node down)"
eval "$("$FIX" seed-package "$SA_DD_A" 7)"
PKG_ROOT="$PACKAGE_ROOT"
[ "$COMPLETE" = "1" ] || sa_die "seeded package not tracked-complete on A"
echo "science-acceptance:     A serves package ${PKG_ROOT:0:16}… (tracked+complete)"
# B's swarm download record, persisted by the node's own one-shot fetch
# path — the live engine replays it at B's first hosting boot.
out="$(sa_sci "$SA_DD_B" zcode.package.fetch "{\"root\":\"$PKG_ROOT\"}")"
[ "$(sa_jget "$out" 'd["ok"]')" = "True" ] || sa_die "B download-record seed failed: $out"
[ "$(sa_jget "$out" 'd["data"]["live"]')" = "False" ] \
    || sa_die "B one-shot fetch claimed a live engine"

# ── [1] boot A and B; assert the loopback-only topology ───────────────
sa_step 1 "boot A (dead sink) and B (connect-only to A); assert exactly A<->B"
SA_PGID_A="$(sa_spawn "$SA_DD_A" "$A_PORT" "$A_RPC" "$A_FS" "$A_HTTPS" "127.0.0.1:$DEAD_SINK")"
sa_wait_rpc "$SA_DD_A" "$A_RPC" "$SA_PGID_A" "$RPC_WARMUP" \
    || { tail -20 "$SA_DD_A/node.log" >&2; sa_die "A RPC never came up"; }
SA_PGID_B="$(sa_spawn "$SA_DD_B" "$B_PORT" "$B_RPC" "$B_FS" "$B_HTTPS" "127.0.0.1:$A_PORT")"
sa_wait_rpc "$SA_DD_B" "$B_RPC" "$SA_PGID_B" "$RPC_WARMUP" \
    || { tail -20 "$SA_DD_B/node.log" >&2; sa_die "B RPC never came up"; }
sleep 3   # version handshake + connect settle
pc_a="$(sa_peer_count "$SA_DD_A" "$A_RPC")"
pc_b="$(sa_peer_count "$SA_DD_B" "$B_RPC")"
[ "$pc_a" = "1" ] || sa_die "A peer count is $pc_a, expected exactly 1 (B)"
[ "$pc_b" = "1" ] || sa_die "B peer count is $pc_b, expected exactly 1 (A)"
echo "science-acceptance:     topology exactly A<->B (peers: A=$pc_a B=$pc_b; regtest, no DNS seeds, no GitHub, -nofilesync)"

# ── [2..6] A's science lifecycle ──────────────────────────────────────
sa_step "2-6" "A: preregister -> confined execute -> reproduce -> findings/review/vote -> discover"
run_lifecycle "$SA_DD_A" 11 "A"
A_STUDY="$L_STUDY"; A_RA="$L_RA"; A_PA="$L_PA"; A_V1="$L_V1"

# ── [7] B's independent lifecycle ─────────────────────────────────────
sa_step 7 "B: the same lifecycle independently (second clean node)"
run_lifecycle "$SA_DD_B" 29 "B"
B_STUDY="$L_STUDY"; B_RA="$L_RA"; B_PA="$L_PA"
[ "$B_STUDY" != "$A_STUDY" ] || sa_die "A and B minted the same study root"

# ── [8] GAP G1 assertions: A's science objects never reached B ────────
sa_step 8 "G1: assert A's science CAS objects have no node-to-node path"
[ "$("$FIX" cas-has "$SA_DD_B/zcode" "$A_STUDY" | grep -c 'PRESENT=0')" = "1" ] \
    || sa_die "A's study wire unexpectedly present in B's CAS"
[ "$("$FIX" cas-has "$SA_DD_B/zcode" "$A_RA" | grep -c 'PRESENT=0')" = "1" ] \
    || sa_die "A's result wire unexpectedly present in B's CAS"
out="$(sa_sci "$SA_DD_B" zcode.science.study.show "{\"study_root\":\"$A_STUDY\"}")"
[ "$(sa_jget "$out" 'd["data"]["found"]')" = "False" ] \
    || sa_die "B's projection unexpectedly knows A's study"
out="$(sa_sci "$SA_DD_B" zcode.science.work.execute \
    "{\"study_root\":\"$A_STUDY\",\"task_root\":\"$L_TASK\",\"candidate_root\":\"$L_CAND\",\"method_root\":\"$L_METHOD\",\"challenge_block_height\":3200000,\"challenge_block_hash\":\"$CH_HASH\",\"confirm\":true,\"now_unix\":$NOW_STUDY}")"
[ "$(sa_jget "$out" 'd["ok"]')" = "False" ] \
    || sa_die "B executed against A's study — distribution happened out of band?!"
case "$(sa_jget "$out" 'd["error"]["message"]')" in
    executor-study-not-in-cas*) : ;;
    *) sa_die "B's refusal named the wrong rule: $out" ;;
esac
echo "science-acceptance:     G1 confirmed: B refuses A's study with executor-study-not-in-cas; no science object crossed the wire"

# ── [9] PACKAGE LEG: B's swarm fetch of A's package ───────────────────
sa_step 9 "package leg: poll B's store for the swarm fetch (budget ${PKG_WAIT}s)"
PKG_COMPLETE=0
deadline=$(( $(date +%s) + PKG_WAIT ))
while [ "$(date +%s)" -lt "$deadline" ]; do
    eval "$("$FIX" verify-package "$SA_DD_B" "$PKG_ROOT")"
    [ "$COMPLETE" = "1" ] && { PKG_COMPLETE=1; break; }
    sleep 3
done
if [ "$PKG_COMPLETE" = "1" ]; then
    [ "$ROOT_MATCH" = "1" ] && [ "$CHUNKS_OK" = "1" ] \
        || sa_die "B fetched bytes that do not re-derive the package root"
    echo "science-acceptance:     package leg PROVEN: B fetched $CHUNKS_CHECKED chunks node-to-node; rederived root == address"
else
    out="$(sa_sci "$SA_DD_B" zcode.package.fetch "{\"root\":\"$PKG_ROOT\"}")"
    dl_state="$(sa_jget "$out" 'd["data"].get("download",{}).get("state")')"
    dl_ads="$(sa_jget "$out" 'd["data"].get("download",{}).get("advertisers")')"
    dl_bytes="$(sa_jget "$out" 'd["data"].get("download",{}).get("present_bytes")')"
    echo "science-acceptance:     G2 REGRESSION: fetch stalled (state=$dl_state advertisers=$dl_ads present_bytes=$dl_bytes)" >&2
    echo "science-acceptance:     the package leg is gated CLOSED (announce bootstrap quota + deduped" >&2
    echo "science-acceptance:     per-sync re-announce + supervisor clock-driven swarm) — a stall is a bug." >&2
    grep -m5 -i "zcode swarm\|announce" "$SA_DD_B/node.log" 2>/dev/null | sed 's/^/science-acceptance:       B log: /' >&2 || true
    sa_die "package leg stalled: G2 regressed"
fi

# ── [10] restart both nodes (SIGTERM, cold boot, same datadirs) ───────
sa_step 10 "SIGTERM both nodes; cold boot same datadirs; topology re-forms"
sa_kill_group "$SA_PGID_A" TERM
sa_kill_group "$SA_PGID_B" TERM
SA_PGID_A=""; SA_PGID_B=""
sleep 1
SA_PGID_A="$(sa_spawn "$SA_DD_A" "$A_PORT" "$A_RPC" "$A_FS" "$A_HTTPS" "127.0.0.1:$DEAD_SINK")"
sa_wait_rpc "$SA_DD_A" "$A_RPC" "$SA_PGID_A" "$RPC_WARMUP" \
    || { tail -20 "$SA_DD_A/node.log" >&2; sa_die "A never came back after SIGTERM restart"; }
SA_PGID_B="$(sa_spawn "$SA_DD_B" "$B_PORT" "$B_RPC" "$B_FS" "$B_HTTPS" "127.0.0.1:$A_PORT")"
sa_wait_rpc "$SA_DD_B" "$B_RPC" "$SA_PGID_B" "$RPC_WARMUP" \
    || { tail -20 "$SA_DD_B/node.log" >&2; sa_die "B never came back after SIGTERM restart"; }
sleep 3
pc_a="$(sa_peer_count "$SA_DD_A" "$A_RPC")"
pc_b="$(sa_peer_count "$SA_DD_B" "$B_RPC")"
[ "$pc_a" = "1" ] && [ "$pc_b" = "1" ] \
    || sa_die "topology did not re-form after restart (A=$pc_a B=$pc_b)"
echo "science-acceptance:     both nodes cold-booted; topology A<->B restored"

# ── [11] HEADLINE: reconstruct every object and receipt from hashes ────
sa_step 11 "rebuild-equivalence + SQL-wipe reconstruction on BOTH nodes"
rebuild_proof "$SA_DD_A" "A" "$A_STUDY" "$A_RA" "$A_PA"
rebuild_proof "$SA_DD_B" "B" "$B_STUDY" "$B_RA" "$B_PA"

# ── verdict ────────────────────────────────────────────────────────────
echo "science-acceptance: ─────────────────────────────────────────────"
echo "science-acceptance: NAMED GAPS (asserted, not worked around):"
echo "science-acceptance:   G1 science CAS objects have no node-to-node distribution (no publisher, no root carrier)"
echo "science-acceptance:   G2 CLOSED (gated): fresh-node swarm fetch proven node-to-node —"
echo "science-acceptance:       NEW_USER bootstrap announce quota (4/h) + deduped per-sync re-announce"
echo "science-acceptance:       + supervisor clock-driven swarm (net.zcode_swarm, 1 s)"
echo "science-acceptance:   G3 rebuild had no operator surface — glued: zcode.science.rebuild leaf"
echo "science-acceptance:   G4 context/findings objects + now_unix pin had no CLI path — glued: fixture tool + validator rules"
echo "science-acceptance: PASS"
exit 0
