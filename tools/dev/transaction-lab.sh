#!/usr/bin/env bash
# Redacted, append-only transaction-lab notebook and statistics.
# This tool records evidence only. It cannot build, sign, authorize, or send.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CATALOG="${ZCL_TRANSACTION_LAB_CATALOG:-$REPO/tools/dev/transaction_lab_catalog.def}"
LEDGER="${ZCL_TRANSACTION_LAB_LEDGER:-$REPO/docs/work/transaction-lab-events.jsonl}"
TYPE_CATALOG="${ZCL_TRANSACTION_TYPE_CATALOG:-$REPO/app/controllers/include/controllers/transaction_types.def}"

die() {
    echo "transaction-lab: $*" >&2
    exit 2
}

proof_allowed() {
    case "$1" in
        builder_verified|interpreter_verified|projection_verified|\
        consensus_verified|simnet_confirmed|live_confirmed|\
        not_demonstrated) return 0 ;;
        *) return 1 ;;
    esac
}

bar() {
    local done="$1" total="$2" width=20 filled=0 empty=0
    [ "$total" -le 0 ] || filled=$((done * width / total))
    [ "$filled" -le "$width" ] || filled="$width"
    empty=$((width - filled))
    printf '%*s' "$filled" '' | tr ' ' '#'
    printf '%*s' "$empty" '' | tr ' ' '-'
}

catalog_count() {
    awk -F'|' '!/^#/ && NF { n++ } END { print n + 0 }' "$CATALOG"
}

stats() {
    awk -F'"' '
        /"schema":"zcl.transaction_lab_event.v1"/ {
            id=$12; network[id]=$16; proof[id]=$20; result[id]=$24
            line=$0
            sub(/^.*"recipient_zat":/, "", line); sub(/,.*/, "", line)
            recipient[id]=line + 0
            line=$0
            sub(/^.*"fee_zat":/, "", line); sub(/}.*/, "", line)
            fee[id]=line + 0
        }
        END {
            for (id in result) {
                seen++
                if (result[id] == "PASS") passed++
                else if (result[id] == "FAIL") failed++
                else if (result[id] == "BLOCKED") blocked++
                if (result[id] == "PASS" &&
                    (proof[id] == "simnet_confirmed" ||
                     proof[id] == "live_confirmed")) chain++
                if (result[id] == "PASS" && proof[id] == "live_confirmed" &&
                    network[id] == "mainnet") {
                    live++
                    live_recipient += recipient[id]
                    live_fee += fee[id]
                }
            }
            printf "%d %d %d %d %d %d %.0f %.0f\n", seen, passed, failed,
                   blocked, chain, live, live_recipient, live_fee
        }
    ' "$LEDGER"
}

check_notebook() {
    local declared_ids lab_ids
    awk -F'|' '
        function bad(msg) { print "transaction-lab-check: " msg > "/dev/stderr"; errors++ }
        NR == FNR {
            if ($0 ~ /^#/ || NF == 0) next
            if (NF != 5) { bad("catalog line " FNR " must have five fields"); next }
            if ($1 !~ /^[a-z0-9_]+$/) bad("invalid case_id at catalog line " FNR)
            if (known[$1]++) bad("duplicate case_id " $1)
            if ($4 !~ /^(builder_verified|interpreter_verified|projection_verified|consensus_verified|simnet_confirmed|live_confirmed|not_demonstrated)$/)
                bad("invalid minimum proof for " $1)
            if ($5 !~ /^test_[a-z0-9_]+$/) bad("invalid test group for " $1)
            catalog[$1]=1
            next
        }
        {
            line=$0
            if (line ~ /"(address|endpoint|datadir|grant_token|private_key|recovery_words|memo|secret)"/)
                bad("sensitive field name at ledger line " FNR)
            n=split(line, a, "\"")
            if (n < 37 || a[4] != "zcl.transaction_lab_event.v1") {
                bad("invalid event schema or field order at ledger line " FNR)
                next
            }
            id=a[12]; network=a[16]; proof=a[20]; result=a[24]
            source=a[28]; commit=a[32]; txid=a[36]
            if (!(id in catalog)) bad("unknown case_id " id)
            seen[id]=1
            if (network !~ /^(isolated|simnet|mainnet)$/) bad("invalid network for " id)
            if (proof !~ /^(builder_verified|interpreter_verified|projection_verified|consensus_verified|simnet_confirmed|live_confirmed|not_demonstrated)$/)
                bad("invalid proof for " id)
            if (result !~ /^(PASS|FAIL|BLOCKED)$/) bad("invalid result for " id)
            if (proof == "not_demonstrated" && result != "BLOCKED")
                bad("not_demonstrated evidence must be BLOCKED for " id)
            if (source !~ /^[A-Za-z0-9_.:-]+$/) bad("invalid source for " id)
            if (commit !~ /^[0-9a-f]{8,64}$/) bad("invalid source commit for " id)
            if (txid != "UNAVAILABLE" && txid !~ /^[0-9a-f]{64}$/)
                bad("invalid txid for " id)
            if (network == "mainnet" && proof == "live_confirmed" &&
                txid == "UNAVAILABLE") bad("live confirmation lacks txid for " id)
            amount=line
            sub(/^.*"recipient_zat":/, "", amount); sub(/,.*/, "", amount)
            cost=line
            sub(/^.*"fee_zat":/, "", cost); sub(/}.*/, "", cost)
            if (amount !~ /^[0-9]+$/ || cost !~ /^[0-9]+$/)
                bad("non-integer money field for " id)
        }
        END {
            for (id in catalog) if (!(id in seen)) bad("missing evidence for " id)
            if (errors) exit 1
        }
    ' "$CATALOG" "$LEDGER"
    declared_ids="$(sed -n 's/^TX_TYPE("\([^"]*\)".*/\1/p' "$TYPE_CATALOG" | sort)"
    lab_ids="$(awk -F'|' '!/^#/ && NF { print $1 }' "$CATALOG" | sort)"
    if [ "$declared_ids" != "$lab_ids" ]; then
        echo "transaction-lab-check: catalog ids differ from transaction type ids" >&2
        diff -u <(printf '%s\n' "$declared_ids") <(printf '%s\n' "$lab_ids") >&2 || true
        return 1
    fi
    echo "transaction-lab-check: PASS"
}

print_status() {
    local total seen passed failed blocked chain live live_recipient live_fee
    total="$(catalog_count)"
    read -r seen passed failed blocked chain live live_recipient live_fee < <(stats)
    printf 'transaction-lab proof:   [%s] %d/%d cases latest=PASS\n' \
        "$(bar "$passed" "$total")" "$passed" "$total"
    printf 'transaction-lab chain:   [%s] %d/%d simulated/live confirmations\n' \
        "$(bar "$chain" "$total")" "$chain" "$total"
    printf 'transaction-lab mainnet: [%s] %d/%d live confirmations\n' \
        "$(bar "$live" "$total")" "$live" "$total"
    printf '  latest_events=%d failures=%d blocked=%d\n' "$seen" "$failed" "$blocked"
    printf '  live_recipient_zat=%s live_fee_zat=%s live_total_zat=%s\n' \
        "$live_recipient" "$live_fee" "$((live_recipient + live_fee))"
    printf '  mainnet_gate=BLOCKED next=clear HANDOFF and identity-bound custody gates\n'
}

print_json() {
    local total seen passed failed blocked chain live live_recipient live_fee
    total="$(catalog_count)"
    read -r seen passed failed blocked chain live live_recipient live_fee < <(stats)
    printf '{"schema":"zcl.transaction_lab_stats.v1","cases_total":%d,' "$total"
    printf '"latest_events":%d,"passed":%d,"failed":%d,"blocked":%d,' \
        "$seen" "$passed" "$failed" "$blocked"
    printf '"chain_confirmed":%d,"mainnet_confirmed":%d,' "$chain" "$live"
    printf '"live_recipient_zat":%s,"live_fee_zat":%s,"live_total_zat":%s,' \
        "$live_recipient" "$live_fee" "$((live_recipient + live_fee))"
    printf '"mainnet_gate":"BLOCKED"}\n'
}

record_event() {
    local case_id='' network='' proof='' result='' source=''
    local txid='UNAVAILABLE' recipient_zat=0 fee_zat=0 arg observed commit
    shift
    for arg in "$@"; do
        case "$arg" in
            --case=*) case_id="${arg#*=}" ;;
            --network=*) network="${arg#*=}" ;;
            --proof=*) proof="${arg#*=}" ;;
            --result=*) result="${arg#*=}" ;;
            --source=*) source="${arg#*=}" ;;
            --txid=*) txid="${arg#*=}" ;;
            --recipient-zat=*) recipient_zat="${arg#*=}" ;;
            --fee-zat=*) fee_zat="${arg#*=}" ;;
            *) die "unknown record argument: $arg" ;;
        esac
    done
    [ -n "$case_id" ] &&
        awk -F'|' -v id="$case_id" '$1 == id { found=1 } END { exit !found }' "$CATALOG" ||
        die "record requires a known --case"
    case "$network" in isolated|simnet|mainnet) ;; *) die "invalid --network" ;; esac
    proof_allowed "$proof" || die "invalid --proof"
    case "$result" in PASS|FAIL|BLOCKED) ;; *) die "invalid --result" ;; esac
    if [ "$proof" = not_demonstrated ] && [ "$result" != BLOCKED ]; then
        die "not_demonstrated evidence must be BLOCKED"
    fi
    [[ "$source" =~ ^[A-Za-z0-9_.:-]+$ ]] || die "invalid --source"
    [[ "$txid" == UNAVAILABLE || "$txid" =~ ^[0-9a-f]{64}$ ]] || die "invalid --txid"
    [[ "$recipient_zat" =~ ^[0-9]+$ ]] || die "invalid --recipient-zat"
    [[ "$fee_zat" =~ ^[0-9]+$ ]] || die "invalid --fee-zat"
    if [ "$network" = mainnet ] && [ "$proof" = live_confirmed ] &&
       [ "$txid" = UNAVAILABLE ]; then
        die "live_confirmed mainnet evidence requires a public txid"
    fi
    observed="$(date -u +%FT%TZ)"
    commit="$(git -C "$REPO" rev-parse --short=8 HEAD)"
    exec 9>>"$LEDGER"
    flock 9
    printf '{"schema":"zcl.transaction_lab_event.v1","observed_at":"%s","case_id":"%s","network":"%s","proof":"%s","result":"%s","source":"%s","source_commit":"%s","txid":"%s","recipient_zat":%s,"fee_zat":%s}\n' \
        "$observed" "$case_id" "$network" "$proof" "$result" "$source" \
        "$commit" "$txid" "$recipient_zat" "$fee_zat" >&9
    flock -u 9
    check_notebook
}

selftest() {
    local fixture fixture_ledger body
    fixture="$(mktemp -d)"
    fixture_ledger="$fixture/events.jsonl"
    cleanup_transaction_lab_selftest() {
        [ ! -d "$fixture" ] || rm -r -- "$fixture"
    }
    trap cleanup_transaction_lab_selftest RETURN
    cp "$LEDGER" "$fixture_ledger"
    ZCL_TRANSACTION_LAB_CATALOG="$CATALOG" \
    ZCL_TRANSACTION_LAB_LEDGER="$fixture_ledger" \
        "$0" record --case=transparent_t_to_t --network=mainnet \
        --proof=live_confirmed --result=PASS --source=selftest_receipt \
        --txid=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa \
        --recipient-zat=1000 --fee-zat=100 >/dev/null
    body="$(ZCL_TRANSACTION_LAB_CATALOG="$CATALOG" \
        ZCL_TRANSACTION_LAB_LEDGER="$fixture_ledger" "$0" json)"
    if [[ "$body" != *'"mainnet_confirmed":1'* ]] ||
       [[ "$body" != *'"live_recipient_zat":1000'* ]] ||
       [[ "$body" != *'"live_fee_zat":100'* ]] ||
       [[ "$body" != *'"live_total_zat":1100'* ]]; then
        die "selftest stats did not include the fixture mainnet receipt"
    fi
    if ZCL_TRANSACTION_LAB_CATALOG="$CATALOG" \
       ZCL_TRANSACTION_LAB_LEDGER="$fixture_ledger" \
        "$0" record --case=market_purchase --network=isolated \
        --proof=not_demonstrated --result=PASS --source=selftest_invalid \
        >/dev/null 2>&1; then
        die "selftest accepted not_demonstrated evidence as PASS"
    fi
    echo "transaction-lab selftest: PASS"
}

case "${1:-status}" in
    status) check_notebook >/dev/null; print_status ;;
    json) check_notebook >/dev/null; print_json ;;
    check) check_notebook ;;
    record) record_event "$@" ;;
    selftest) selftest ;;
    -h|--help)
        echo "usage: tools/dev/transaction-lab.sh {status|json|check|selftest|record ...}"
        ;;
    *) die "unknown action: $1" ;;
esac
