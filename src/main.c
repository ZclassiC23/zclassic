/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * ZClassic full node — pure C23 implementation.
 *
 * One binary, three operator modes:
 *   zclassic23 [node options]         — run as full node / linger service
 *   zclassic23 api                    — API discovery from running node
 *   zclassic23 appprotocols           — application protocol catalog
 *   zclassic23 servicecatalog         — sovereign service UX catalog
 *   zclassic23 serviceoperations      — sovereign operation UX catalog
 *   zclassic23 <command> [--input=…]  — typed native command call
 *   zclassic23 status                 — compact native status + next action
 *   zclassic23 proofbundle            — single read-only proof artifact
 *   zclassic23 statecatalog           — zcl_state subsystem catalog
 *   zclassic23 agentlanes             — canonical/soak/dev lane topology
 *   zclassic23 agentliveness          — unified liveness rollup
 *   zclassic23 agentinterface         — preferred AI/operator interface
 *   zclassic23 milestone              — ASCII milestone status from node
 *   zclassic23 refold                 — UTXO anchor rebuild readiness
 *   zclassic23 <method> [params...]   — RPC client to running node */

#include "config/boot.h"
#include "hotswap/hotswap_module.h"
#include "rpc/client.h"
#include "rpc/protocol.h"
#include "net/file_service.h"
#include "util/thread_registry.h"
#include "util/util.h"
#include "util/log_level.h"
#include "util/log_macros.h"
#include "util/file_tree_ops.h"
#include "util/spawn.h"
#include <sqlite3.h>
#include "json/json.h"
#include "views/wallet_gui.h"
#include "models/database.h"
#include "models/block.h"                 /* db_block_find_by_hash: tip-bind sapling_root */
#include "controllers/agent_controller.h"
#include "controllers/diagnostics_controller.h"
#include "controllers/network_controller.h"
#include "controllers/sync_controller.h"
#include "controllers/snapshot_controller.h"
#include "storage/coins_db.h"
#include "storage/chainstate_legacy_reader.h"
#include "storage/ldb_snapshot.h"
#include "storage/progress_store.h"
#include "services/shielded_history_import_service.h"
#include "jobs/proof_validate_null_hash_rearm.h"
#include "chain/chain.h"                  /* BLOCK_VALID_TRANSACTIONS: connected floor */
#include "chain/chainparams.h"
#include "chain/checkpoints.h"
#include "chain/utxo_snapshot_loader.h"  /* uss_open: post-write checkpoint verify */
#include "coins/utxo_commitment.h"
#include "crypto/sha3.h"
#include "crypto/sha256.h"                /* -bench-crypto-vs-rust: SHA256 */
#include "crypto/blake2b.h"               /* -bench-crypto-vs-rust: BLAKE2b */
#include "crypto/ed25519.h"               /* -bench-crypto-vs-rust: JoinSplit sig verify */
#include "keys/key.h"                     /* -bench-crypto-vs-rust: secp256k1 signing ctx */
#include "keys/pubkey.h"                  /* -bench-crypto-vs-rust: secp256k1 ECDSA verify */
#include "core/uint256.h"
#include "chain/equihash.h"               /* -bench-crypto-verify: PoW verify */
#include "primitives/block.h"
#include "sapling/sapling.h"              /* -bench-crypto-verify: Groth16 verify */
#include "sapling/bls12_381.h"            /* -bench-crypto-vs-rust: BLS12-381 fp_mul + pairing */
#include "sapling/sapling_prover.h"
#include "sapling/params_init.h"
#include "platform/clock.h"               /* clock_now_monotonic_ns (banned: gettimeofday) */
#include "platform/os_proc.h"             /* os_proc_exe_path (banned: raw /proc/self/exe) */
#include "test/verify_bench_fixture.h"    /* baked real (200,9) witness */
#include "controllers/explorer_internal.h"
#include "services/wallet_backup_service.h"
#include "services/chain_tip_watchdog.h"  /* #8: self-respawn when off-systemd */
#include "util/supervisor_backstop.h"     /* Pillar 7: self-respawn on a frozen
                                            * supervisor sweep, off-systemd */
#include "util/clientversion.h"
#include "util/sd_notify.h"               /* #8: detect NOTIFY_SOCKET */
#include "util/ar_step_readonly.h"
#include "controllers/rpc_client.h"
#include "command/native_command.h"
#include "config/command_catalog.h"
#include "kernel/command_registry.h"
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ════════════════════════════════════════════════════════════════
 *  BENCH MODE — canonical user benchmark entry points
 * ════════════════════════════════════════════════════════════════ */

struct bench_row {
    char bench[96];
    double value;
    bool numeric;
    char unit[16];
    char notes[256];
};

static const char *bench_history_path(void)
{
    const char *p = getenv("ZCL_BENCH_HISTORY");
    return (p && *p) ? p : "docs/bench-history.csv";
}

static const char *bench_commit(void)
{
    const char *c = getenv("ZCL_BENCH_COMMIT");
    if (c && *c) return c;
    return zcl_build_commit();
}

static bool bench_env_true(const char *name)
{
    const char *v = getenv(name);
    if (!v || !*v) return false;
    return strcmp(v, "0") != 0 &&
           strcmp(v, "false") != 0 &&
           strcmp(v, "FALSE") != 0 &&
           strcmp(v, "no") != 0 &&
           strcmp(v, "NO") != 0;
}

static void bench_iso8601(char *out, size_t out_len)
{
    time_t now = time(NULL);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    strftime(out, out_len, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

static double bench_system_uptime_seconds(void)
{
    FILE *f = fopen("/proc/uptime", "r");
    if (!f) return 0.0;
    double up = 0.0;
    int n = fscanf(f, "%lf", &up);
    fclose(f);
    return n == 1 ? up : 0.0;
}

static void bench_csv_field(FILE *f, const char *s)
{
    bool quote = false;
    for (const char *p = s; p && *p; ++p) {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') {
            quote = true;
            break;
        }
    }
    if (!quote) {
        fputs(s ? s : "", f);
        return;
    }
    fputc('"', f);
    for (const char *p = s; p && *p; ++p) {
        if (*p == '"') fputc('"', f);
        fputc(*p, f);
    }
    fputc('"', f);
}

static bool bench_history_ensure(const char *path)
{
    FILE *check = fopen(path, "r");
    if (check) {
        fclose(check);
        return true;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "bench: cannot create %s\n", path);
        return false;
    }
    fputs("# zclassic23 benchmark history.\n", f);
    fputs("# Columns: date, commit, bench, value, unit, notes.\n", f);
    fputs("# Numeric value rows are regression-gated by build/bin/zclassic23 -bench-regress.\n", f);
    fputs("# Empty value rows are pending/skipped measurements and are ignored by the gate.\n", f);
    fputs("# Bench names follow docs/USER_BENCHMARKS.md primaries #1..#5.\n", f);
    fputs("# Append-only: do not rewrite old rows; add a correction row instead.\n", f);
    fputs("date,commit,bench,value,unit,notes\n", f);
    fclose(f);
    return true;
}

static bool bench_history_append(const char *path, const struct bench_row *rows,
                                 size_t rows_len)
{
    if (!bench_history_ensure(path)) return false;
    FILE *f = fopen(path, "a");
    if (!f) {
        fprintf(stderr, "bench: cannot append %s\n", path);
        return false;
    }
    char date[32];
    bench_iso8601(date, sizeof(date));
    for (size_t i = 0; i < rows_len; ++i) {
        char val[64] = "";
        if (rows[i].numeric)
            snprintf(val, sizeof(val), "%.3f", rows[i].value);
        bench_csv_field(f, date); fputc(',', f);
        bench_csv_field(f, bench_commit()); fputc(',', f);
        bench_csv_field(f, rows[i].bench); fputc(',', f);
        bench_csv_field(f, val); fputc(',', f);
        bench_csv_field(f, rows[i].unit); fputc(',', f);
        bench_csv_field(f, rows[i].notes); fputc('\n', f);
    }
    fclose(f);
    return true;
}

static void bench_rows_default(struct bench_row rows[5])
{
    snprintf(rows[0].bench, sizeof(rows[0].bench), "#1 cold-start to operational");
    snprintf(rows[0].unit, sizeof(rows[0].unit), "s");
    snprintf(rows[0].notes, sizeof(rows[0].notes),
             "pending P0/full-chain run; use -bench-coldstart after deploy");

    snprintf(rows[1].bench, sizeof(rows[1].bench), "#2 warm-start to operational");
    snprintf(rows[1].unit, sizeof(rows[1].unit), "s");
    snprintf(rows[1].notes, sizeof(rows[1].notes),
             "pending seeded datadir; use -bench-warmstart with ZCL_BENCH_SOURCE_DATADIR");

    snprintf(rows[2].bench, sizeof(rows[2].bench), "#3 stay-in-sync MTBF");
    snprintf(rows[2].unit, sizeof(rows[2].unit), "s");
    snprintf(rows[2].notes, sizeof(rows[2].notes),
             "pending soak after P0 deploy");

    snprintf(rows[3].bench, sizeof(rows[3].bench), "#4 RAM steady-state");
    snprintf(rows[3].unit, sizeof(rows[3].unit), "MB");
    snprintf(rows[3].notes, sizeof(rows[3].notes),
             "pending seeded datadir; sample RSS after isolated node boot");

    snprintf(rows[4].bench, sizeof(rows[4].bench), "#5 kill-9 recovery");
    snprintf(rows[4].unit, sizeof(rows[4].unit), "s");
    snprintf(rows[4].notes, sizeof(rows[4].notes),
             "pending seeded datadir; measure RPC-ready recovery histogram");

    for (size_t i = 0; i < 5; ++i) {
        rows[i].value = 0.0;
        rows[i].numeric = false;
    }
}

static bool bench_read_pid_from_file(const char *path, long *pid_out)
{
    if (!path || !pid_out) return false;
    FILE *f = fopen(path, "r");
    if (!f) return false;
    long pid = -1;
    int n = fscanf(f, "%ld", &pid);
    fclose(f);
    if (n != 1 || pid <= 0) return false;
    *pid_out = pid;
    return true;
}

static bool bench_find_live_pid(long *pid_out, char *source, size_t source_len)
{
    const char *pid_env = getenv("ZCL_BENCH_PID");
    if (pid_env && *pid_env) {
        char *end = NULL;
        errno = 0;
        long pid = strtol(pid_env, &end, 10);
        if (errno == 0 && end && *end == '\0' && pid > 0) {
            if (pid_out) *pid_out = pid;
            if (source && source_len)
                snprintf(source, source_len, "ZCL_BENCH_PID=%ld", pid);
            return true;
        }
    }

    char default_datadir[512];
    const char *datadir = getenv("ZCL_BENCH_SOURCE_DATADIR");
    if (!datadir || !*datadir)
        datadir = getenv("ZCL_BENCH_LIVE_DATADIR");
    if (!datadir || !*datadir) {
        const char *home = getenv("HOME");
        if (!home || !*home)
            home = ".";
        snprintf(default_datadir, sizeof(default_datadir),
                 "%s/.zclassic-c23", home);
        datadir = default_datadir;
    }

    char pid_path[512];
    snprintf(pid_path, sizeof(pid_path), "%s/zclassic23.pid", datadir);
    long pid = -1;
    if (!bench_read_pid_from_file(pid_path, &pid))
        return false;
    if (pid_out) *pid_out = pid;
    if (source && source_len)
        snprintf(source, source_len, "%s", pid_path);
    return true;
}

static bool bench_read_proc_status(long pid, double *rss_mb, double *uptime_s)
{
    if (pid <= 0) return false;

    char path[128];
    snprintf(path, sizeof(path), "/proc/%ld/status", pid);
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[256];
    bool got_rss = false;
    while (fgets(line, sizeof(line), f)) {
        long kb = 0;
        if (sscanf(line, "VmRSS: %ld kB", &kb) == 1) {
            if (rss_mb) *rss_mb = (double)kb / 1024.0;
            got_rss = true;
            break;
        }
    }
    fclose(f);

    if (uptime_s) {
        snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
        f = fopen(path, "r");
        if (!f) return got_rss;
        if (fgets(line, sizeof(line), f)) {
            char *rp = strrchr(line, ')');
            if (rp) {
                char fields[256];
                snprintf(fields, sizeof(fields), "%s", rp + 2);
                char *save = NULL;
                char *tok = strtok_r(fields, " \t\r\n", &save);
                unsigned long long start_ticks = 0;
                for (int field = 3; tok; field++) {
                    if (field == 22) {
                        start_ticks = strtoull(tok, NULL, 10);
                        break;
                    }
                    tok = strtok_r(NULL, " \t\r\n", &save);
                }
                long hz = sysconf(_SC_CLK_TCK);
                double up = bench_system_uptime_seconds();
                if (start_ticks > 0 && hz > 0 && up > 0.0) {
                    double start_s = (double)start_ticks / (double)hz;
                    if (up >= start_s)
                        *uptime_s = up - start_s;
                }
            }
        }
        fclose(f);
    }

    return got_rss;
}

static void bench_maybe_live_readonly(struct bench_row rows[5])
{
    if (!bench_env_true("ZCL_BENCH_LIVE_READONLY"))
        return;

    long pid = -1;
    char source[512] = "";
    double rss_mb = 0.0;
    double uptime_s = 0.0;
    if (!bench_find_live_pid(&pid, source, sizeof(source)) ||
        !bench_read_proc_status(pid, &rss_mb, &uptime_s)) {
        snprintf(rows[3].notes, sizeof(rows[3].notes),
                 "live read-only sample requested but no readable zclassic23 pid was found");
        return;
    }

    rows[3].value = rss_mb;
    rows[3].numeric = true;
    snprintf(rows[3].notes, sizeof(rows[3].notes),
             "live read-only RSS sample from pid %ld (%s); process uptime %.0fs",
             pid, source, uptime_s);

    if (uptime_s > 0.0) {
        rows[2].value = uptime_s;
        rows[2].numeric = true;
        snprintf(rows[2].notes, sizeof(rows[2].notes),
                 "live read-only uptime sample from pid %ld (%s); not a 30-day MTBF soak",
                 pid, source);
    }
}

static int bench_run_all(void)
{
    struct bench_row rows[5];
    bench_rows_default(rows);
    bench_maybe_live_readonly(rows);
    const char *path = bench_history_path();
    const char *dir = getenv("ZCL_BENCH_DIR");
    if (!dir || !*dir) dir = "/tmp/zcl23-bench";

    printf("zclassic23 benchmark harness (C)\n");
    printf("  commit:        %s\n", bench_commit());
    printf("  history:       %s\n", path);
    printf("  bench datadir: %s\n", dir);
    printf("  live service:  %s\n\n",
           bench_env_true("ZCL_BENCH_LIVE_READONLY")
             ? "read-only /proc sample only"
             : "not touched by this mode");
    for (size_t i = 0; i < 5; ++i) {
        char value[32];
        if (rows[i].numeric)
            snprintf(value, sizeof(value), "%.3f", rows[i].value);
        else
            snprintf(value, sizeof(value), "--");
        printf("  %-31s %10s %-8s %s\n",
               rows[i].bench, value, rows[i].unit, rows[i].notes);
    }
    return bench_history_append(path, rows, 5) ? 0 : 1;
}

static int bench_run_one(const char *name)
{
    struct bench_row rows[5];
    bench_rows_default(rows);
    bench_maybe_live_readonly(rows);
    int idx = -1;
    if (strcmp(name, "-bench-coldstart") == 0) idx = 0;
    else if (strcmp(name, "-bench-warmstart") == 0) idx = 1;
    else if (strcmp(name, "-bench-mtbf") == 0) idx = 2;
    else if (strcmp(name, "-bench-rss") == 0) idx = 3;
    else if (strcmp(name, "-bench-kill9") == 0) idx = 4;
    if (idx < 0) return 2;

    if (rows[idx].numeric)
        printf("%s: %.3f %s\n%s\n",
               rows[idx].bench, rows[idx].value, rows[idx].unit,
               rows[idx].notes);
    else
        printf("%s: pending\n%s\n", rows[idx].bench, rows[idx].notes);
    return bench_history_append(bench_history_path(), &rows[idx], 1) ? 0 : 1;
}

static bool bench_higher_is_better(const char *bench)
{
    return bench && strstr(bench, "#3 stay-in-sync MTBF") != NULL;
}

static int bench_regress(void)
{
    const char *path = bench_history_path();
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("[bench-regress] no history at %s; skipping\n", path);
        return 0;
    }

    struct seen {
        char bench[96];
        double prev;
        double last;
        int count;
    } seen[32];
    size_t seen_len = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || strncmp(line, "date,", 5) == 0)
            continue;
        char tmp[1024];
        snprintf(tmp, sizeof(tmp), "%s", line);
        char *fields[6] = {0};
        char *save = NULL;
        char *tok = strtok_r(tmp, ",", &save);
        for (int i = 0; i < 6 && tok; ++i) {
            fields[i] = tok;
            tok = strtok_r(NULL, ",", &save);
        }
        if (!fields[2] || !fields[3] || fields[3][0] == '\0' ||
            fields[3][0] == '"')
            continue;
        char *end = NULL;
        double v = strtod(fields[3], &end);
        if (!end || end == fields[3])
            continue;
        size_t i;
        for (i = 0; i < seen_len; ++i) {
            if (strcmp(seen[i].bench, fields[2]) == 0)
                break;
        }
        if (i == seen_len && seen_len < 32) {
            snprintf(seen[i].bench, sizeof(seen[i].bench), "%s", fields[2]);
            seen[i].prev = 0.0;
            seen[i].last = 0.0;
            seen[i].count = 0;
            seen_len++;
        }
        if (i < seen_len) {
            seen[i].prev = seen[i].last;
            seen[i].last = v;
            seen[i].count++;
        }
    }
    fclose(f);

    bool failed = false;
    for (size_t i = 0; i < seen_len; ++i) {
        if (seen[i].count < 2 || seen[i].prev <= 0.0)
            continue;
        double pct = bench_higher_is_better(seen[i].bench)
            ? ((seen[i].prev - seen[i].last) / seen[i].prev) * 100.0
            : ((seen[i].last - seen[i].prev) / seen[i].prev) * 100.0;
        if (pct > 20.0) {
            fprintf(stderr,
                    "[bench-regress] FAIL %s: %.3f -> %.3f (%.1f%% > 20%%)\n",
                    seen[i].bench, seen[i].prev, seen[i].last, pct);
            failed = true;
        }
    }
    if (failed)
        return 1;
    printf("[bench-regress] OK: no numeric primary regressed > 20%%\n");
    return 0;
}

/* ── -bench-crypto-verify ────────────────────────────────────────────
 *
 * Microbenchmark for the two dominant per-block consensus-VERIFY costs:
 *   (a) the pure-C23 BLS12-381 Groth16 pairing verify (Sapling output
 *       proof, via sapling_check_output), and
 *   (b) the Equihash (200,9) solution verify (check_equihash_solution).
 * Emits ns/op rows to bench-history.csv in the same shape the rest of
 * the harness uses, so `-bench-regress` gates them at ±20% (ns/op is
 * lower-is-better, which is bench_regress's default direction).
 *
 * TEETH: before timing, each primitive is asserted to return the CORRECT
 * result on its fixture — valid -> true AND one-bit-flipped -> false. If
 * either fails, the row is NOT emitted. This makes it impossible for the
 * benchmark to "get fast" by benching a broken/always-true/no-op verify:
 * a hollow-fast verifier fails the flipped-input check here (and in the
 * hermetic test group test_verify_bench_selftest) before any number is
 * recorded. The timed loop also folds every result into a volatile sink
 * so the verify call cannot be optimised away.
 *
 * All timing uses the mandated monotonic clock (clock_now_monotonic_ns);
 * gettimeofday is banned (Gate #19). Iteration count is auto-calibrated
 * to a wall-time budget so ns/op is stable across fast and slow hosts. */

static volatile uint64_t g_bench_verify_sink;

/* Run `fn(arg)` in a timed loop until `budget_ns` elapses; return ns/op.
 * Every result is folded into the global sink so the call is not elided;
 * if any call returns false, *ok_out is set false (an invalid bench). */
static double bench_time_verify(bool (*fn)(void *), void *arg,
                                double budget_ns, bool *ok_out)
{
    /* Warm caches / branch predictors. */
    for (int i = 0; i < 3; ++i)
        g_bench_verify_sink += fn(arg) ? 1u : 0u;

    uint64_t iters = 0;
    bool all_true = true;
    int64_t t0 = clock_now_monotonic_ns();
    int64_t elapsed = 0;
    do {
        /* Inner batch keeps the clock read amortised. */
        for (int b = 0; b < 8; ++b) {
            bool r = fn(arg);
            all_true = all_true && r;
            g_bench_verify_sink += r ? 1u : 0u;
        }
        iters += 8;
        elapsed = clock_now_monotonic_ns() - t0;
    } while ((double)elapsed < budget_ns);

    if (ok_out) *ok_out = all_true;
    return (double)elapsed / (double)iters;
}

/* Adapters matching bench_time_verify's fn(arg) shape. */
static bool bench_verify_equihash(void *arg)
{
    return check_equihash_solution((const struct block_header *)arg, NULL);
}

struct bench_groth16_arg {
    struct sapling_verification_ctx vctx;
    uint8_t cv[32], cm[32], epk[32], proof[192];
};
static bool bench_verify_groth16(void *arg)
{
    struct bench_groth16_arg *a = arg;
    sapling_verification_ctx_init(&a->vctx);
    return sapling_check_output(&a->vctx, a->cv, a->cm, a->epk, a->proof);
}

static bool bench_find_diversifier(uint8_t diversifier[11])
{
    memset(diversifier, 0, 11);
    for (int i = 0; i < 256; i++) {
        diversifier[0] = (uint8_t)i;
        if (sapling_check_diversifier(diversifier))
            return true;
    }
    return false;
}

static int bench_crypto_verify(void)
{
    const char *path = bench_history_path();
    /* ~500 ms per primitive: enough iterations for a stable ns/op without
     * making the bench slow. Override with ZCL_BENCH_BUDGET_MS. */
    double budget_ms = 500.0;
    const char *bm = getenv("ZCL_BENCH_BUDGET_MS");
    if (bm && *bm) {
        double v = strtod(bm, NULL);
        if (v >= 20.0 && v <= 60000.0) budget_ms = v;
    }
    double budget_ns = budget_ms * 1e6;

    printf("zclassic23 consensus-verify microbenchmark (C)\n");
    printf("  commit:   %s\n", bench_commit());
    printf("  history:  %s\n", path);
    printf("  budget:   %.0f ms/primitive\n", budget_ms);
    printf("  clock:    clock_now_monotonic_ns (monotonic)\n\n");

    struct bench_row rows[2];
    size_t nrows = 0;

    /* ── (b) Equihash (200,9) — hermetic ───────────────────────────── */
    {
        struct block_header h;
        verify_bench_fill_eh_header(&h);

        /* TEETH: valid -> true, one-bit-flipped -> false. */
        bool pos = check_equihash_solution(&h, NULL);
        struct block_header bad = h;
        bad.nSolution[600] ^= 0x01;
        bool neg = check_equihash_solution(&bad, NULL);
        if (!pos || neg) {
            fprintf(stderr,
                "[bench-crypto-verify] REFUSING equihash row: verify is "
                "hollow (valid=%d flipped=%d); expected valid=1 flipped=0\n",
                pos, neg);
        } else {
            bool ok = true;
            double ns = bench_time_verify(bench_verify_equihash, &h,
                                          budget_ns, &ok);
            if (ok) {
                snprintf(rows[nrows].bench, sizeof(rows[nrows].bench),
                         "consensus-verify equihash-200-9");
                snprintf(rows[nrows].unit, sizeof(rows[nrows].unit), "ns/op");
                rows[nrows].value = ns;
                rows[nrows].numeric = true;
                snprintf(rows[nrows].notes, sizeof(rows[nrows].notes),
                    "check_equihash_solution on baked real 200,9 witness; "
                    "host-relative; %.0f ops/sec", 1e9 / ns);
                printf("  %-34s %12.1f ns/op  (%.0f ops/sec)\n",
                       "equihash-200-9 verify", ns, 1e9 / ns);
                nrows++;
            }
        }
    }

    /* ── (a) Groth16 / BLS12-381 output-proof verify — needs params ── */
    {
        const char *home = getenv("HOME");
        char params_dir[512];
        snprintf(params_dir, sizeof(params_dir), "%s/.zcash-params",
                 (home && *home) ? home : ".");
        if (!sapling_init_params(params_dir)) {
            printf("  groth16 verify: ~/.zcash-params absent -> SKIPPED "
                   "(VK/proving keys not vendored)\n");
        } else {
            uint8_t diversifier[11];
            bool div_ok = bench_find_diversifier(diversifier);
            uint8_t ask[32], nsk[32], ovk[32];
            sapling_generate_r(ask);
            sapling_generate_r(nsk);
            sapling_generate_r(ovk);
            uint8_t ak[32], nk[32], ivk[32], pk_d[32];
            sapling_ask_to_ak(ask, ak);
            sapling_nsk_to_nk(nsk, nk);
            sapling_crh_ivk(ak, nk, ivk);
            bool pk_ok = div_ok && sapling_ivk_to_pkd(ivk, diversifier, pk_d);

            void *pctx = zclassic_sapling_proving_ctx_init();
            struct bench_groth16_arg ga;
            uint8_t enc[580], out_ct[80];
            bool built = pctx && pk_ok &&
                sapling_build_output_with_ctx(pctx, ovk, diversifier, pk_d,
                                              54321, NULL,
                                              ga.cv, ga.cm, ga.epk, enc,
                                              out_ct, ga.proof);
            if (pctx) zclassic_sapling_proving_ctx_free(pctx);

            if (!built) {
                fprintf(stderr, "[bench-crypto-verify] prover failed; "
                                "skipping groth16 row\n");
            } else {
                /* TEETH: valid -> true, one-bit-flipped -> false. */
                sapling_verification_ctx_init(&ga.vctx);
                bool pos = sapling_check_output(&ga.vctx, ga.cv, ga.cm,
                                                ga.epk, ga.proof);
                struct bench_groth16_arg bad = ga;
                bad.proof[64] ^= 0x01;
                sapling_verification_ctx_init(&bad.vctx);
                bool neg = sapling_check_output(&bad.vctx, bad.cv, bad.cm,
                                                bad.epk, bad.proof);
                if (!pos || neg) {
                    fprintf(stderr,
                        "[bench-crypto-verify] REFUSING groth16 row: verify "
                        "is hollow (valid=%d flipped=%d)\n", pos, neg);
                } else {
                    bool ok = true;
                    double ns = bench_time_verify(bench_verify_groth16, &ga,
                                                  budget_ns, &ok);
                    if (ok) {
                        snprintf(rows[nrows].bench, sizeof(rows[nrows].bench),
                                 "consensus-verify groth16-bls12-381-output");
                        snprintf(rows[nrows].unit, sizeof(rows[nrows].unit),
                                 "ns/op");
                        rows[nrows].value = ns;
                        rows[nrows].numeric = true;
                        snprintf(rows[nrows].notes, sizeof(rows[nrows].notes),
                            "sapling_check_output (full BLS12-381 pairing) on "
                            "a real prover output proof; host-relative; "
                            "%.0f ops/sec", 1e9 / ns);
                        printf("  %-34s %12.1f ns/op  (%.0f ops/sec)\n",
                               "groth16-bls12-381 output verify", ns,
                               1e9 / ns);
                        nrows++;
                    }
                }
            }
        }
    }

    if (nrows == 0) {
        fprintf(stderr,
            "[bench-crypto-verify] no rows produced (nothing appended)\n");
        return 1;
    }
    printf("\n  appended %zu row(s) to %s\n", nrows, path);
    /* Fold the sink into the exit path so the optimiser cannot elide the
     * timed calls (value is otherwise unused). */
    if (g_bench_verify_sink == 0xdeadbeefULL) fputc(' ', stderr);
    return bench_history_append(path, rows, nrows) ? 0 : 1;
}

/* ── -bench-crypto-vs-rust ───────────────────────────────────────────
 *
 * The STANDING crypto-performance surface: times EVERY C crypto primitive
 * on the consensus path (Equihash verify, Groth16/BLS12-381 output verify,
 * BLS12-381 pairing, BLS12-381 Fp mul, secp256k1 ECDSA verify, ed25519
 * verify, SHA256, SHA3-256, BLAKE2b), reports a flake-resistant MEDIAN
 * ns/op per primitive, appends the medians to docs/bench-history.csv, and
 * prints a machine-readable `CRYPTOPERF <key> <median_ns> <ops_per_sec>
 * <samples>` line the gate (tools/scripts/check_crypto_perf.sh) parses to
 * enforce the ratchet + ratio-vs-Rust invariant.
 *
 * This is MEASUREMENT ONLY — it calls the exact production verify/hash
 * predicates. Consensus verify LOGIC is frozen; no validity predicate is
 * touched. Every primitive is TEETH-checked (correct on a real fixture,
 * rejecting on a perturbed one / KAT-sensitive) BEFORE any number is
 * recorded, so a hollow-fast (always-true / no-op) primitive can never
 * "get fast" and slip past the gate. Timing uses the mandated monotonic
 * clock (Gate #19); every result is folded into a volatile sink so the
 * optimiser cannot elide the timed call. */

static int bench_dcmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Median of `samples` runs of bench_time_verify, each with its own budget.
 * Median (not mean) resists a single slow sample from a scheduler hiccup. */
static double bench_median_verify(bool (*fn)(void *), void *arg,
                                  double budget_ns_each, int samples,
                                  bool *ok_out)
{
    double s[16];
    if (samples > 16) samples = 16;
    if (samples < 1) samples = 1;
    bool all_ok = true;
    for (int i = 0; i < samples; ++i) {
        bool ok = true;
        s[i] = bench_time_verify(fn, arg, budget_ns_each, &ok);
        all_ok = all_ok && ok;
    }
    qsort(s, (size_t)samples, sizeof(double), bench_dcmp);
    if (ok_out) *ok_out = all_ok;
    return s[samples / 2];
}

/* ── per-primitive timed adapters (fn(arg)->bool shape) ────────────── */

struct bench_hash_arg { const uint8_t *buf; size_t len; uint8_t out[64]; };

static bool bench_hash_sha256(void *arg)
{
    struct bench_hash_arg *a = arg;
    struct sha256_ctx c;
    sha256_init(&c);
    sha256_write(&c, a->buf, a->len);
    sha256_finalize(&c, a->out);
    g_bench_verify_sink += a->out[0]; /* observe the digest */
    return true;
}
static bool bench_hash_sha3_256(void *arg)
{
    struct bench_hash_arg *a = arg;
    zcl_sha3_256(a->buf, a->len, a->out);
    g_bench_verify_sink += a->out[0];
    return true;
}
static bool bench_hash_blake2b(void *arg)
{
    struct bench_hash_arg *a = arg;
    struct blake2b_ctx c;
    blake2b_init(&c, 64);
    blake2b_update(&c, a->buf, a->len);
    blake2b_final(&c, a->out, 64);
    g_bench_verify_sink += a->out[0];
    return true;
}

struct bench_pairing_arg { struct g1_point g1; struct g2_point g2; struct fp12 r; };
static bool bench_pairing(void *arg)
{
    struct bench_pairing_arg *a = arg;
    bls12_381_pairing(&a->r, &a->g1, &a->g2);
    g_bench_verify_sink += a->r.c0.c0.c0.d[0]; /* observe the GT element */
    return true;
}

struct bench_fpmul_arg { struct fp a, b, r; };
static bool bench_fpmul(void *arg)
{
    struct bench_fpmul_arg *x = arg;
    fp_mul(&x->r, &x->a, &x->b);
    g_bench_verify_sink += x->r.d[0];
    return true;
}

struct bench_ecdsa_arg {
    struct pubkey pk;
    struct uint256 hash;
    unsigned char sig[80];
    size_t siglen;
};
static bool bench_verify_ecdsa(void *arg)
{
    struct bench_ecdsa_arg *a = arg;
    return pubkey_verify(&a->pk, &a->hash, a->sig, a->siglen);
}

struct bench_ed_arg { uint8_t sig[64], pk[32]; const uint8_t *msg; size_t msglen; };
static bool bench_verify_ed25519(void *arg)
{
    struct bench_ed_arg *a = arg;
    return ed25519_verify(a->sig, a->msg, a->msglen, a->pk);
}

/* Record one primitive: median ns/op, machine line, and a bench-history row. */
static void bench_vsr_record(struct bench_row *rows, size_t *nrows,
                             const char *key, const char *human, double ns)
{
    printf("  %-30s %14.1f ns/op  (%.0f ops/sec)\n", human, ns, 1e9 / ns);
    printf("CRYPTOPERF %s %.1f %.0f\n", key, ns, 1e9 / ns);
    struct bench_row *r = &rows[*nrows];
    snprintf(r->bench, sizeof(r->bench), "crypto-vs-rust %s", key);
    snprintf(r->unit, sizeof(r->unit), "ns/op");
    r->value = ns;
    r->numeric = true;
    snprintf(r->notes, sizeof(r->notes),
             "median C ns/op; host-relative; vs pinned Rust baseline "
             "(tools/crypto_perf_baseline.csv); %.0f ops/sec", 1e9 / ns);
    (*nrows)++;
}

static int bench_crypto_vs_rust(void)
{
    const char *path = bench_history_path();
    /* Per-SAMPLE budget; the primitive is measured `samples` times and the
     * MEDIAN reported. Defaults chosen so the whole run is a few seconds.
     * Override budget with ZCL_BENCH_BUDGET_MS, sample count with
     * ZCL_BENCH_SAMPLES (median-of-N; 5 by default). */
    double budget_ms = 120.0;
    const char *bm = getenv("ZCL_BENCH_BUDGET_MS");
    if (bm && *bm) { double v = strtod(bm, NULL); if (v >= 10.0 && v <= 60000.0) budget_ms = v; }
    int samples = 5;
    const char *sp = getenv("ZCL_BENCH_SAMPLES");
    if (sp && *sp) { long v = strtol(sp, NULL, 10); if (v >= 1 && v <= 15) samples = (int)v; }
    double budget_ns = budget_ms * 1e6;

    printf("zclassic23 crypto-vs-rust microbenchmark (pure C23)\n");
    printf("  commit:   %s\n", bench_commit());
    printf("  history:  %s\n", path);
    printf("  budget:   %.0f ms/sample, median of %d samples\n", budget_ms, samples);
    printf("  clock:    clock_now_monotonic_ns (monotonic)\n\n");

    struct bench_row rows[16];
    size_t nrows = 0;

    /* ── Equihash (200,9) verify — hermetic ────────────────────────── */
    {
        struct block_header h;
        verify_bench_fill_eh_header(&h);
        bool pos = check_equihash_solution(&h, NULL);
        struct block_header bad = h;
        bad.nSolution[600] ^= 0x01;
        bool neg = check_equihash_solution(&bad, NULL);
        if (!pos || neg) {
            fprintf(stderr, "[bench-crypto-vs-rust] REFUSING equihash: "
                    "hollow (valid=%d flipped=%d)\n", pos, neg);
        } else {
            bool ok = true;
            double ns = bench_median_verify(bench_verify_equihash, &h,
                                            budget_ns, samples, &ok);
            if (ok) bench_vsr_record(rows, &nrows, "equihash-200-9",
                                     "equihash-200-9 verify", ns);
        }
    }

    /* ── BLS12-381 Fp multiply — hermetic ──────────────────────────── */
    {
        struct bench_fpmul_arg fa;
        /* Both fixtures must be < the BLS12-381 field modulus q (top byte
         * 0x1a); 0x11 and 0x07 repeated are safely canonical. */
        uint8_t ba[48], bb[48];
        memset(ba, 0x11, sizeof(ba));
        memset(bb, 0x07, sizeof(bb));
        bool va = fp_from_bytes(&fa.a, ba);
        bool vb = fp_from_bytes(&fa.b, bb);
        /* TEETH (Montgomery-form agnostic): the multiply must agree with the
         * independent squaring routine (fp_mul(a,a) == fp_sq(a)) and produce a
         * non-zero, non-trivial result. A no-op multiply (returns an operand)
         * or a constant-output multiply fails square-consistency. */
        struct fp sq, mm, ab;
        fp_sq(&sq, &fa.a);
        fp_mul(&mm, &fa.a, &fa.a);
        fp_mul(&ab, &fa.a, &fa.b);
        bool sq_consistent = va && vb && fp_eq(&sq, &mm);
        bool nondegen = sq_consistent && !fp_is_zero(&ab) &&
                        !fp_eq(&ab, &fa.a) && !fp_eq(&ab, &fa.b);
        if (!sq_consistent || !nondegen) {
            fprintf(stderr, "[bench-crypto-vs-rust] REFUSING bls-fp-mul: "
                    "teeth failed (sq_consistent=%d nondegen=%d)\n",
                    sq_consistent, nondegen);
        } else {
            bool ok = true;
            double ns = bench_median_verify(bench_fpmul, &fa,
                                            budget_ns, samples, &ok);
            if (ok) bench_vsr_record(rows, &nrows, "bls12-381-fp-mul",
                                     "bls12-381 Fp multiply", ns);
        }
    }

    /* ── BLS12-381 optimal-Ate pairing (miller loop + final exp) ───── */
    {
        static const uint8_t G1_GEN[48] = {
            0x97,0xf1,0xd3,0xa7,0x31,0x97,0xd7,0x94,0x26,0x95,0x63,0x8c,
            0x4f,0xa9,0xac,0x0f,0xc3,0x68,0x8c,0x4f,0x97,0x74,0xb9,0x05,
            0xa1,0x4e,0x3a,0x3f,0x17,0x1b,0xac,0x58,0x6c,0x55,0xe8,0x3f,
            0xf9,0x7a,0x1a,0xef,0xfb,0x3a,0xf0,0x0a,0xdb,0x22,0xc6,0xbb };
        static const uint8_t G2_GEN[96] = {
            0x93,0xe0,0x2b,0x60,0x52,0x71,0x9f,0x60,0x7d,0xac,0xd3,0xa0,
            0x88,0x27,0x4f,0x65,0x59,0x6b,0xd0,0xd0,0x99,0x20,0xb6,0x1a,
            0xb5,0xda,0x61,0xbb,0xdc,0x7f,0x50,0x49,0x33,0x4c,0xf1,0x12,
            0x13,0x94,0x5d,0x57,0xe5,0xac,0x7d,0x05,0x5d,0x04,0x2b,0x7e,
            0x02,0x4a,0xa2,0xb2,0xf0,0x8f,0x0a,0x91,0x26,0x08,0x05,0x27,
            0x2d,0xc5,0x10,0x51,0xc6,0xe4,0x7a,0xd4,0xfa,0x40,0x3b,0x02,
            0xb4,0x51,0x0b,0x64,0x7a,0xe3,0xd1,0x77,0x0b,0xac,0x03,0x26,
            0xa8,0x05,0xbb,0xef,0xd4,0x80,0x56,0xc8,0xc1,0x21,0xbd,0xb8 };
        struct bench_pairing_arg pa;
        bool g1_ok = g1_from_compressed(&pa.g1, G1_GEN);
        bool g2_ok = g2_from_compressed(&pa.g2, G2_GEN);
        /* TEETH: e(G1,G2) must be a non-degenerate GT element (!=1, !=0). */
        bool teeth = false;
        if (g1_ok && g2_ok) {
            struct fp12 r, one12;
            bls12_381_pairing(&r, &pa.g1, &pa.g2);
            fp12_one(&one12);
            teeth = !fp12_is_zero(&r) && !(fp12_sub(&one12, &one12, &r),
                                           fp12_is_zero(&one12));
        }
        if (!teeth) {
            fprintf(stderr, "[bench-crypto-vs-rust] REFUSING bls-pairing: "
                    "teeth failed (g1=%d g2=%d)\n", g1_ok, g2_ok);
        } else {
            bool ok = true;
            double ns = bench_median_verify(bench_pairing, &pa,
                                            budget_ns, samples, &ok);
            if (ok) bench_vsr_record(rows, &nrows, "bls12-381-pairing",
                                     "bls12-381 Ate pairing", ns);
        }
    }

    /* ── secp256k1 ECDSA verify — locally signed round-trip ────────── */
    {
        ecc_start();
        ecc_verify_init();
        struct bench_ecdsa_arg ea;
        struct privkey k;
        privkey_make_new(&k, true);
        for (int i = 0; i < 32; ++i) ea.hash.data[i] = (uint8_t)(i * 7 + 1);
        ea.siglen = sizeof(ea.sig);
        bool signed_ok = privkey_get_pubkey(&k, &ea.pk) &&
                         privkey_sign(&k, &ea.hash, ea.sig, &ea.siglen);
        /* TEETH: valid sig -> true, corrupted hash -> false. */
        bool pos = signed_ok &&
                   pubkey_verify(&ea.pk, &ea.hash, ea.sig, ea.siglen);
        struct uint256 badh = ea.hash;
        badh.data[0] ^= 0x01;
        bool neg = signed_ok &&
                   pubkey_verify(&ea.pk, &badh, ea.sig, ea.siglen);
        if (!pos || neg) {
            fprintf(stderr, "[bench-crypto-vs-rust] REFUSING secp256k1: "
                    "teeth failed (valid=%d corrupted=%d)\n", pos, neg);
        } else {
            bool ok = true;
            double ns = bench_median_verify(bench_verify_ecdsa, &ea,
                                            budget_ns, samples, &ok);
            if (ok) bench_vsr_record(rows, &nrows, "secp256k1-ecdsa-verify",
                                     "secp256k1 ECDSA verify", ns);
        }
        ecc_verify_destroy();
        ecc_stop();
    }

    /* ── ed25519 verify (JoinSplit) — RFC 8032 Test 2 vector ───────── */
    {
        struct bench_ed_arg ed;
        static const uint8_t ED_PK[32] = {
            0x3d,0x40,0x17,0xc3,0xe8,0x43,0x89,0x5a,0x92,0xb7,0x0a,0xa7,
            0x4d,0x1b,0x7e,0xbc,0x9c,0x98,0x2c,0xcf,0x2e,0xc4,0x96,0x8c,
            0xc0,0xcd,0x55,0xf1,0x2a,0xf4,0x66,0x0c };
        static const uint8_t ED_SIG[64] = {
            0x92,0xa0,0x09,0xa9,0xf0,0xd4,0xca,0xb8,0x72,0x0e,0x82,0x0b,
            0x5f,0x64,0x25,0x40,0xa2,0xb2,0x7b,0x54,0x16,0x50,0x3f,0x8f,
            0xb3,0x76,0x22,0x23,0xeb,0xdb,0x69,0xda,0x08,0x5a,0xc1,0xe4,
            0x3e,0x15,0x99,0x6e,0x45,0x8f,0x36,0x13,0xd0,0xf1,0x1d,0x8c,
            0x38,0x7b,0x2e,0xae,0xb4,0x30,0x2a,0xee,0xb0,0x0d,0x29,0x16,
            0x12,0xbb,0x0c,0x00 };
        static const uint8_t ED_MSG[1] = { 0x72 };
        memcpy(ed.pk, ED_PK, 32);
        memcpy(ed.sig, ED_SIG, 64);
        ed.msg = ED_MSG;
        ed.msglen = 1;
        bool pos = ed25519_verify(ed.sig, ed.msg, ed.msglen, ed.pk);
        uint8_t bad_sig[64];
        memcpy(bad_sig, ED_SIG, 64);
        bad_sig[10] ^= 0x01;
        bool neg = ed25519_verify(bad_sig, ed.msg, ed.msglen, ed.pk);
        if (!pos || neg) {
            fprintf(stderr, "[bench-crypto-vs-rust] REFUSING ed25519: "
                    "teeth failed (valid=%d corrupted=%d)\n", pos, neg);
        } else {
            bool ok = true;
            double ns = bench_median_verify(bench_verify_ed25519, &ed,
                                            budget_ns, samples, &ok);
            if (ok) bench_vsr_record(rows, &nrows, "ed25519-verify",
                                     "ed25519 verify", ns);
        }
    }

    /* ── Hash primitives — 1 KiB message, avalanche teeth ──────────── */
    {
        static uint8_t msg1k[1024];
        for (size_t i = 0; i < sizeof(msg1k); ++i) msg1k[i] = (uint8_t)(i * 131 + 7);

        struct { const char *key; const char *human; bool (*fn)(void *); } H[] = {
            { "sha256",   "SHA256 (1 KiB)",    bench_hash_sha256   },
            { "sha3-256", "SHA3-256 (1 KiB)",  bench_hash_sha3_256 },
            { "blake2b",  "BLAKE2b-512 (1 KiB)", bench_hash_blake2b },
        };
        for (size_t hi = 0; hi < sizeof(H) / sizeof(H[0]); ++hi) {
            struct bench_hash_arg ha = { .buf = msg1k, .len = sizeof(msg1k) };
            /* TEETH: avalanche — digest depends on all input and is not a
             * copy of the input (defeats no-op / constant-output hashes). */
            H[hi].fn(&ha);
            uint8_t d0[64];
            memcpy(d0, ha.out, 64);
            uint8_t flipped[1024];
            memcpy(flipped, msg1k, sizeof(flipped));
            flipped[500] ^= 0x01;
            struct bench_hash_arg hb = { .buf = flipped, .len = sizeof(flipped) };
            H[hi].fn(&hb);
            bool avalanche = memcmp(d0, hb.out, 32) != 0;
            bool not_copy = memcmp(d0, msg1k, 32) != 0;
            if (!avalanche || !not_copy) {
                fprintf(stderr, "[bench-crypto-vs-rust] REFUSING %s: teeth "
                        "failed (avalanche=%d not_copy=%d)\n",
                        H[hi].key, avalanche, not_copy);
                continue;
            }
            struct bench_hash_arg ht = { .buf = msg1k, .len = sizeof(msg1k) };
            bool ok = true;
            double ns = bench_median_verify(H[hi].fn, &ht,
                                            budget_ns, samples, &ok);
            if (ok) bench_vsr_record(rows, &nrows, H[hi].key, H[hi].human, ns);
        }
    }

    /* ── Groth16 / BLS12-381 output-proof verify — needs params ────── */
    {
        const char *home = getenv("HOME");
        char params_dir[512];
        snprintf(params_dir, sizeof(params_dir), "%s/.zcash-params",
                 (home && *home) ? home : ".");
        if (!sapling_init_params(params_dir)) {
            printf("  groth16-bls12-381-output verify: ~/.zcash-params absent "
                   "-> SKIPPED (VK/proving keys not vendored)\n");
        } else {
            uint8_t diversifier[11];
            bool div_ok = bench_find_diversifier(diversifier);
            uint8_t ask[32], nsk[32], ovk[32];
            sapling_generate_r(ask);
            sapling_generate_r(nsk);
            sapling_generate_r(ovk);
            uint8_t ak[32], nk[32], ivk[32], pk_d[32];
            sapling_ask_to_ak(ask, ak);
            sapling_nsk_to_nk(nsk, nk);
            sapling_crh_ivk(ak, nk, ivk);
            bool pk_ok = div_ok && sapling_ivk_to_pkd(ivk, diversifier, pk_d);
            void *pctx = zclassic_sapling_proving_ctx_init();
            struct bench_groth16_arg ga;
            uint8_t enc[580], out_ct[80];
            bool built = pctx && pk_ok &&
                sapling_build_output_with_ctx(pctx, ovk, diversifier, pk_d,
                                              54321, NULL,
                                              ga.cv, ga.cm, ga.epk, enc,
                                              out_ct, ga.proof);
            if (pctx) zclassic_sapling_proving_ctx_free(pctx);
            if (!built) {
                fprintf(stderr, "[bench-crypto-vs-rust] prover failed; "
                                "skipping groth16 row\n");
            } else {
                sapling_verification_ctx_init(&ga.vctx);
                bool pos = sapling_check_output(&ga.vctx, ga.cv, ga.cm,
                                                ga.epk, ga.proof);
                struct bench_groth16_arg bad = ga;
                bad.proof[64] ^= 0x01;
                sapling_verification_ctx_init(&bad.vctx);
                bool neg = sapling_check_output(&bad.vctx, bad.cv, bad.cm,
                                                bad.epk, bad.proof);
                if (!pos || neg) {
                    fprintf(stderr, "[bench-crypto-vs-rust] REFUSING groth16: "
                            "hollow (valid=%d flipped=%d)\n", pos, neg);
                } else {
                    bool ok = true;
                    double ns = bench_median_verify(bench_verify_groth16, &ga,
                                                    budget_ns, samples, &ok);
                    if (ok) bench_vsr_record(rows, &nrows,
                                             "groth16-bls12-381-output",
                                             "groth16 output verify", ns);
                }
            }
        }
    }

    if (nrows == 0) {
        fprintf(stderr,
            "[bench-crypto-vs-rust] no rows produced (nothing appended)\n");
        return 1;
    }
    printf("\n  appended %zu row(s) to %s\n", nrows, path);
    if (g_bench_verify_sink == 0xdeadbeefULL) fputc(' ', stderr);
    return bench_history_append(path, rows, nrows) ? 0 : 1;
}

static int bench_mode_main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-bench") == 0 ||
            strcmp(argv[i], "-bench-all") == 0)
            return bench_run_all();
        if (strcmp(argv[i], "-bench-regress") == 0)
            return bench_regress();
        if (strcmp(argv[i], "-bench-crypto-verify") == 0)
            return bench_crypto_verify();
        if (strcmp(argv[i], "-bench-crypto-vs-rust") == 0)
            return bench_crypto_vs_rust();
        if (strncmp(argv[i], "-bench-", 7) == 0)
            return bench_run_one(argv[i]);
    }
    return 2;
}

/* ════════════════════════════════════════════════════════════════
 *  CLI MODE — connect to running node, execute RPC, print result
 * ════════════════════════════════════════════════════════════════ */

static char cli_cookie[256];
static int cli_port = 18232;
static int cli_p2p_port = 0;
static int cli_https_port = 0;
static int cli_fs_port = 0;

static bool cli_cookie_exists(const char *datadir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/.cookie", datadir);
    return access(path, R_OK) == 0;
}

static bool cli_service_exec_arg(const char *key, char *out, size_t out_size)
{
    if (!key || !*key || !out || out_size == 0)
        return false;
    out[0] = '\0';

    /* `systemctl --user show zclassic23 -p ExecStart --value` — capture its
     * stdout via the no-shell spawn primitive. stderr is discarded by
     * zcl_spawn_capture (matches the old `2>/dev/null`); the exit code is not
     * needed (the old code ignored pclose's return too). */
    const char *const argv[] = {
        "systemctl", "--user", "show", "zclassic23",
        "-p", "ExecStart", "--value", NULL
    };
    char buf[8192];
    zcl_spawn_capture(argv, buf, sizeof(buf), 5000);
    size_t n = strlen(buf);
    if (n == 0)
        return false;

    char needle[64];
    int written = snprintf(needle, sizeof(needle), "-%s=", key);
    if (written <= 0 || (size_t)written >= sizeof(needle))
        return false;

    const char *p = strstr(buf, needle);
    if (!p)
        return false;
    p += strlen(needle);

    char quote = '\0';
    if (*p == '"' || *p == '\'')
        quote = *p++;

    size_t len = 0;
    while (p[len] &&
           ((quote && p[len] != quote) ||
            (!quote && p[len] != ' ' && p[len] != '\t' &&
             p[len] != '\n' && p[len] != ';'))) {
        len++;
    }
    if (len == 0 || len >= out_size)
        return false;

    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

static bool cli_read_cookie(const char *datadir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/.cookie", datadir);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t n = fread(cli_cookie, 1, sizeof(cli_cookie) - 1, f);
    fclose(f);
    cli_cookie[n] = 0;
    char *nl = strchr(cli_cookie, '\n');
    if (nl) *nl = 0;
    return n > 0;
}

static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(const char *in, size_t len, char *out)
{
    size_t i, j = 0;
    for (i = 0; i + 2 < len; i += 3) {
        uint8_t a = (uint8_t)in[i], b = (uint8_t)in[i+1], c = (uint8_t)in[i+2];
        out[j++] = b64[a >> 2];
        out[j++] = b64[((a & 3) << 4) | (b >> 4)];
        out[j++] = b64[((b & 0xf) << 2) | (c >> 6)];
        out[j++] = b64[c & 0x3f];
    }
    if (i < len) {
        uint8_t a = (uint8_t)in[i];
        out[j++] = b64[a >> 2];
        if (i + 1 < len) {
            uint8_t b2 = (uint8_t)in[i+1];
            out[j++] = b64[((a & 3) << 4) | (b2 >> 4)];
            out[j++] = b64[(b2 & 0xf) << 2];
        } else {
            out[j++] = b64[(a & 3) << 4];
            out[j++] = '=';
        }
        out[j++] = '=';
    }
    out[j] = 0;
}

static char *cli_rpc_call_internal(const char *body, size_t body_len,
                                   bool quiet)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return NULL;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)cli_port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (!quiet)
            fprintf(stderr, "Cannot connect to node at 127.0.0.1:%d\n",
                    cli_port);
        close(sock);
        return NULL;
    }

    char auth[512];
    b64_encode(cli_cookie, strlen(cli_cookie), auth);

    char hdr[1024];
    int hlen = snprintf(hdr, sizeof(hdr),
        "POST / HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Authorization: Basic %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n",
        auth, body_len);

    send(sock, hdr, (size_t)hlen, 0);
    send(sock, body, body_len, 0);

    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    if (!buf) { close(sock); return NULL; }
    for (;;) {
        if (len + 4096 > cap) {
            size_t next_cap = cap * 2;
            char *next = realloc(buf, next_cap);
            if (!next) {
                free(buf);
                close(sock);
                return NULL;
            }
            buf = next;
            cap = next_cap;
        }
        ssize_t n = recv(sock, buf + len, cap - len - 1, 0);
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(sock);
    buf[len] = 0;

    char *start = strstr(buf, "\r\n\r\n");
    if (start) { start += 4; memmove(buf, start, strlen(start) + 1); }
    return buf;
}

static char *cli_rpc_call(const char *body, size_t body_len)
{
    return cli_rpc_call_internal(body, body_len, false);
}

static void cli_probe_capture_target_build(const struct json_value *result)
{
    if (!result || result->type != JSON_OBJ)
        return;

    const struct json_value *runtime = json_get(result, "runtime_identity");
    const struct json_value *runtime_build = json_get(result, "runtime_build");
    const char *source_id =
        json_get_str(json_get(result, "source_id_sha256"));
    if (!source_id[0])
        source_id = json_get_str(json_get(runtime, "source_id_sha256"));
    if (!source_id[0])
        source_id = json_get_str(json_get(
            runtime_build, "running_source_id_sha256"));
    if (source_id[0])
        agent_runtime_availability_set_target_source_id_sha256(source_id);

    const char *build = json_get_str(json_get(result, "build_commit"));
    if (!build[0])
        build = json_get_str(json_get(runtime, "build_commit"));
    if (!build[0])
        build = json_get_str(json_get(runtime_build,
                                      "running_build_commit"));
    if (build[0])
        agent_runtime_availability_set_target_build_commit(build);
}

static void cli_probe_record_response(const char *method, const char *resp,
                                      bool *partial_error)
{
    struct json_value root;
    json_init(&root);
    if (!resp || !json_read(&root, resp, strlen(resp)) ||
        root.type != JSON_OBJ) {
        agent_runtime_availability_record_method(method, "unknown",
                                                 RPC_PARSE_ERROR,
                                                 "invalid_json_response");
        if (partial_error)
            *partial_error = true;
        json_free(&root);
        return;
    }

    const struct json_value *err = json_get(&root, "error");
    if (err && err->type == JSON_OBJ) {
        int64_t code = json_get_int(json_get(err, "code"));
        const char *msg = json_get_str(json_get(err, "message"));
        agent_runtime_availability_record_method(
            method,
            code == RPC_METHOD_NOT_FOUND ? "unsupported_method_not_found"
                                         : "present_error",
            code, msg);
        if (code != RPC_METHOD_NOT_FOUND && partial_error)
            *partial_error = true;
        json_free(&root);
        return;
    }

    const struct json_value *result = json_get(&root, "result");
    if (result && !json_is_null(result)) {
        agent_runtime_availability_record_method(method, "supported", 0, "");
        cli_probe_capture_target_build(result);
    } else {
        agent_runtime_availability_record_method(method, "unknown", 0,
                                                 "missing_result");
        if (partial_error)
            *partial_error = true;
    }
    json_free(&root);
}

static void cli_probe_static_agent_target(const char *datadir)
{
    agent_runtime_availability_begin_probe("cli_target_rpc", datadir,
                                           cli_port, "no_cookie");
    if (!cli_read_cookie(datadir))
        return;

    bool partial_error = false;
    agent_runtime_availability_set_probe_status("probing");
    for (size_t i = 0; i < agent_runtime_probe_method_count(); i++) {
        const char *method = agent_runtime_probe_method_name(i);
        const char *probe_params = agent_contract_probe_params_json(method);
        if (!probe_params || !probe_params[0])
            probe_params = "[]";
        char body[512];
        int blen = snprintf(body, sizeof(body),
            "{\"jsonrpc\":\"1.0\",\"id\":\"agent-availability\","
            "\"method\":\"%s\",\"params\":%s}",
            method, probe_params);
        if (blen <= 0 || (size_t)blen >= sizeof(body)) {
            agent_runtime_availability_record_method(method, "unknown",
                                                     RPC_INTERNAL_ERROR,
                                                     "request_too_large");
            partial_error = true;
            continue;
        }
        char *resp = cli_rpc_call_internal(body, (size_t)blen, true);
        if (!resp) {
            agent_runtime_availability_set_probe_status(
                i == 0 ? "connect_failed" : "partial_error");
            return;
        }
        cli_probe_record_response(method, resp, &partial_error);
        free(resp);
    }
    agent_runtime_availability_set_probe_status(partial_error ?
        "partial_error" : "ok");
}

static bool cli_agent_contract_method(const char *method)
{
    if (!method || !method[0])
        return false;
    for (size_t i = 0; i < agent_runtime_probe_method_count(); i++) {
        const char *probe = agent_runtime_probe_method_name(i);
        if (probe && strcmp(probe, method) == 0)
            return true;
    }
    return false;
}

static bool cli_rpc_error_is_method_not_found(const char *resp,
                                              char *message,
                                              size_t message_len)
{
    if (message && message_len)
        message[0] = '\0';
    if (!resp || !resp[0])
        return false;

    struct json_value root;
    json_init(&root);
    if (!json_read(&root, resp, strlen(resp)) || root.type != JSON_OBJ) {
        json_free(&root);
        return false;
    }

    const struct json_value *err = json_get(&root, "error");
    if (!err || err->type != JSON_OBJ) {
        json_free(&root);
        return false;
    }

    int64_t code = json_get_int(json_get(err, "code"));
    const char *msg = json_get_str(json_get(err, "message"));
    if (message && message_len)
        snprintf(message, message_len, "%s", msg);
    json_free(&root);
    return code == RPC_METHOD_NOT_FOUND;
}

static bool cli_print_peer_incidents_dumpstate_fallback(const char *method,
                                                        const char *reason)
{
    if (!method || strcmp(method, "peerincidents") != 0)
        return false;

    const char *params = "[\"peer_lifecycle\",\"incidents\"]";
    char body[512];
    int blen = snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"1.0\",\"id\":\"z-peerincidents-fallback\","
        "\"method\":\"dumpstate\",\"params\":%s}", params);
    if (blen <= 0 || (size_t)blen >= sizeof(body))
        return false;

    char *resp = cli_rpc_call(body, (size_t)blen);
    if (!resp)
        return false;

    struct json_value root;
    json_init(&root);
    bool ok = false;
    if (json_read(&root, resp, strlen(resp)) && root.type == JSON_OBJ) {
        const struct json_value *result = json_get(&root, "result");
        struct json_value normalized;
        json_init(&normalized);
        if (result && peer_incidents_from_dumpstate_result_json(
                          result, &normalized, reason)) {
            char out[262144];
            size_t need = json_write(&normalized, out, sizeof(out));
            if (need < sizeof(out)) {
                printf("%s\n", out);
                ok = true;
            } else {
                fprintf(stderr, "peerincidents fallback JSON exceeded "
                                "CLI buffer\n");
            }
        }
        json_free(&normalized);
    }
    json_free(&root);
    free(resp);
    return ok;
}

static bool cli_print_contract_method_skew_diagnostic(
    const char *method,
    const char *datadir,
    enum zcl_operator_lane operator_lane,
    enum zcl_runtime_profile runtime_profile,
    const char *rpc_message)
{
    if (!cli_agent_contract_method(method))
        return false;

    agent_runtime_availability_reset();
    rpc_agent_set_boot_context(app_operator_lane_name(operator_lane),
                               app_runtime_profile_name(runtime_profile),
                               datadir, cli_port, cli_p2p_port,
                               cli_https_port, cli_fs_port);
    cli_probe_static_agent_target(datadir);

    struct json_value diag;
    json_init(&diag);
    json_set_object(&diag);
    json_push_kv_str(&diag, "schema", "zcl.cli_rpc_diagnostic.v1");
    json_push_kv_int(&diag, "schema_version", 1);
    json_push_kv_str(&diag, "status", "error");
    json_push_kv_str(&diag, "error", "target_runtime_method_not_found");
    json_push_kv_str(&diag, "method", method);
    json_push_kv_str(&diag, "rpc_error_message",
                     rpc_message && rpc_message[0] ? rpc_message
                                                   : "Method not found");
    json_push_kv_str(&diag, "producer_build_commit", zcl_build_commit());
    json_push_kv_str(&diag, "target_datadir", datadir ? datadir : "");
    json_push_kv_int(&diag, "target_rpcport", cli_port);
    json_push_kv_str(&diag, "probable_cause",
                     "target_runtime_version_skew_or_contract_not_deployed");
    json_push_kv_str(&diag, "summary",
                     "The local binary advertises this agent/operator method, "
                     "but the running target RPC does not support it.");
    json_push_kv_str(&diag, "safe_next_action",
                     "inspect runtime_availability, then target a lane whose "
                     "runtime supports the method or deploy the fresh binary "
                     "to a safe lane");
    agent_push_runtime_availability_json(&diag, "runtime_availability");

    char out[262144];
    size_t need = json_write(&diag, out, sizeof(out));
    if (need >= sizeof(out)) {
        json_free(&diag);
        fprintf(stderr, "Error: CLI diagnostic exceeded output buffer\n");
        return true;
    }
    printf("%s\n", out);
    json_free(&diag);
    return true;
}

/* CLI UX contract: unrecognized top-level command. Fires only after the RPC
 * layer itself has confirmed `method` is not a real RPC method (method not
 * found) and neither specialized fallback above claimed it — i.e. this is a
 * genuine typo, not a version-skew symptom. The message itself (typed error
 * line + up to 3 "did you mean" suggestions from the existing command-search
 * index) is built by zcl_native_render_unknown_command so it can be unit
 * tested without a live node; this just prints it. Never touches the node
 * again. */
static void cli_print_unknown_command_diagnostic(const char *method)
{
    char buf[1024];
    size_t n = zcl_native_render_unknown_command(zcl_command_catalog(),
                                                 method, buf, sizeof(buf));
    if (n > 0)
        fputs(buf, stderr);
    else
        fprintf(stderr, "error=UNKNOWN_COMMAND detail=no such command '%s' "
                        "try=zclassic23 discover search %s\n",
               method ? method : "", method ? method : "");
}

static void print_usage(const char *prog);

typedef bool (*cli_static_agent_handler)(const struct json_value *params,
                                         bool help,
                                         struct json_value *result);

struct cli_static_agent_route {
    const char *method;
    cli_static_agent_handler handler;
};

static const struct cli_static_agent_route g_cli_static_agent_routes[] = {
    { "agentmap", rpc_agent_map },
    { "agentlanes", rpc_agent_lanes },
    { "agentliveness", rpc_agent_liveness },
    { "agentimpact", rpc_agent_impact },
    { "agentcontracts", rpc_agent_contracts },
    { "agentbuild", rpc_agent_build },
    { "agentdevstatus", rpc_agent_dev_status },
    { "anchorstatus", rpc_agent_anchor_status },
    { "proofbundle", rpc_agent_proof_bundle },
    { "appprotocols", rpc_app_protocols },
    { "servicecatalog", rpc_service_catalog },
    { "serviceoperations", rpc_service_operations },
    { "agentinterface", rpc_agent_interface },
    { "agentops", rpc_agent_ops },
    { "statecatalog", diag_rpc_statecatalog },
    { "agentdeployguard", rpc_agent_deploy_guard },
};

static const struct cli_static_agent_route *
cli_static_agent_lookup(const char *method)
{
    if (!method || !method[0])
        return NULL;

    for (size_t i = 0; i < sizeof(g_cli_static_agent_routes) /
                            sizeof(g_cli_static_agent_routes[0]); i++) {
        const struct cli_static_agent_route *route =
            &g_cli_static_agent_routes[i];
        if (strcmp(route->method, method) == 0 &&
            agent_contract_lookup(route->method))
            return route;
    }
    return NULL;
}

static bool cli_static_agent_method(const char *method)
{
    return cli_static_agent_lookup(method) != NULL;
}

static int cli_static_agent_result_exit_code(const char *method,
                                             const struct json_value *result)
{
    if (strcmp(method, "agentdeployguard") != 0)
        return 0;
    const struct json_value *code_json = json_get(result, "exit_code");
    if (!code_json || code_json->type != JSON_INT)
        return 1;
    int64_t code = json_get_int(code_json);
    if (code < 0 || code > 125)
        return 1;
    return (int)code;
}

static int cli_run_static_agent_method(const char *method,
                                       const char **params_storage,
                                       int nparams)
{
    const struct cli_static_agent_route *route =
        cli_static_agent_lookup(method);
    if (!route) {
        fprintf(stderr, "unknown static agent method\n");
        return 1;
    }

    struct json_value params, result;
    if (!rpc_convert_values(method, params_storage, (size_t)nparams, &params)) {
        fprintf(stderr, "Bad parameters\n");
        json_free(&params);
        return 1;
    }

    json_init(&result);
    bool ok = route->handler(&params, false, &result);

    char out[131072];
    size_t need = ok ? json_write(&result, out, sizeof(out)) : 0;
    int exit_code = 1;
    if (ok && need >= sizeof(out)) {
        fprintf(stderr, "agent contract JSON exceeded CLI buffer\n");
        ok = false;
    } else if (ok) {
        printf("%s\n", out);
        exit_code = cli_static_agent_result_exit_code(method, &result);
    } else {
        fprintf(stderr, "agent contract generation failed\n");
    }

    json_free(&result);
    json_free(&params);
    return exit_code;
}

enum { CLI_MAX_PARAMS = 128 };

static int cli_main(int argc, char **argv)
{
    const char *home = getenv("HOME");
    char datadir[512];
    if (home) snprintf(datadir, sizeof(datadir), "%s/.zclassic-c23", home);
    else      snprintf(datadir, sizeof(datadir), ".zclassic-c23");

    bool datadir_set = false;
    bool rpcport_set = false;
    bool p2pport_set = false;
    bool httpsport_set = false;
    bool fsport_set = false;
    enum zcl_operator_lane operator_lane = ZCL_OPERATOR_LANE_UNKNOWN;
    enum zcl_runtime_profile runtime_profile = ZCL_RUNTIME_FULL;
    const char *method = NULL;
    const char *params_storage[CLI_MAX_PARAMS];
    int nparams = 0;

    const char *env_lane = getenv("ZCL_OPERATOR_LANE");
    if (env_lane && env_lane[0] &&
        !app_operator_lane_parse(env_lane, &operator_lane)) {
        fprintf(stderr, "Ignoring unknown ZCL_OPERATOR_LANE=%s\n", env_lane);
    }

    /* Operator target flags are accepted before or after the method so
     * `zclassic23 agent -datadir=... -rpcport=...` cannot accidentally query
     * the default service while an agent is trying to inspect a lane. */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-datadir=", 9) == 0) {
            snprintf(datadir, sizeof(datadir), "%s", argv[i] + 9);
            datadir_set = true;
        } else if (strncmp(argv[i], "-rpcport=", 9) == 0) {
            cli_port = atoi(argv[i] + 9);
            rpcport_set = true;
        } else if (strncmp(argv[i], "-port=", 6) == 0) {
            cli_p2p_port = atoi(argv[i] + 6);
            p2pport_set = true;
        } else if (strncmp(argv[i], "-httpsport=", 11) == 0) {
            cli_https_port = atoi(argv[i] + 11);
            httpsport_set = true;
        } else if (strncmp(argv[i], "-fsport=", 8) == 0) {
            cli_fs_port = atoi(argv[i] + 8);
            fsport_set = true;
        } else if (strncmp(argv[i], "-operator-lane=", 15) == 0) {
            if (!app_operator_lane_parse(argv[i] + 15, &operator_lane)) {
                fprintf(stderr, "Unknown operator lane: %s\n", argv[i] + 15);
                return 1;
            }
        } else if (strncmp(argv[i], "-profile=", 9) == 0) {
            if (!app_runtime_profile_parse(argv[i] + 9, &runtime_profile)) {
                fprintf(stderr, "Unknown runtime profile: %s\n", argv[i] + 9);
                return 1;
            }
        } else if (!method) {
            method = argv[i];
        } else {
            if (nparams >= CLI_MAX_PARAMS) {
                fprintf(stderr, "Too many RPC parameters (max %d)\n",
                        CLI_MAX_PARAMS);
                return 1;
            }
            params_storage[nparams++] = argv[i];
        }
    }

    if (!method) {
        print_usage(argv[0]);
        return 1;
    }

    if (!datadir_set && !cli_cookie_exists(datadir)) {
        char service_datadir[512];
        if (cli_service_exec_arg("datadir", service_datadir,
                                 sizeof(service_datadir)) &&
            cli_cookie_exists(service_datadir)) {
            snprintf(datadir, sizeof(datadir), "%s", service_datadir);
        }
    }
    if (!rpcport_set) {
        char service_rpcport[32];
        if (cli_service_exec_arg("rpcport", service_rpcport,
                                 sizeof(service_rpcport))) {
            int port = atoi(service_rpcport);
            if (port > 0 && port < 65536)
                cli_port = port;
        }
    }
    if (!p2pport_set) {
        char service_p2pport[32];
        if (cli_service_exec_arg("port", service_p2pport,
                                 sizeof(service_p2pport))) {
            int port = atoi(service_p2pport);
            if (port > 0 && port < 65536)
                cli_p2p_port = port;
        }
    }
    if (!httpsport_set) {
        char service_httpsport[32];
        if (cli_service_exec_arg("httpsport", service_httpsport,
                                 sizeof(service_httpsport))) {
            int port = atoi(service_httpsport);
            if (port > 0 && port < 65536)
                cli_https_port = port;
        }
    }
    if (!fsport_set) {
        char service_fsport[32];
        if (cli_service_exec_arg("fsport", service_fsport,
                                 sizeof(service_fsport))) {
            int port = atoi(service_fsport);
            if (port > 0 && port < 65536)
                cli_fs_port = port;
        }
    }

    if (strcmp(method, "--agent") == 0 || strcmp(method, "-agent") == 0)
        method = "agent";
    else if (strcmp(method, "--status") == 0 ||
             strcmp(method, "-status") == 0)
        method = "status";
    else if (strcmp(method, "--summary") == 0 ||
             strcmp(method, "-summary") == 0)
        method = "summary";

    /* Canonical registry roots (status/core/app/dev/ops/discover plus aliases)
     * resolve through the native command registry BEFORE the
     * arbitrary RPC-method fallback further down. A typo under a canonical
     * branch returns the structured unknown-command error (exit 2) and never
     * becomes an RPC method. */
    if (zcl_native_command_is_root(method))
        return zcl_native_command_main(method,
                                       (const char *const *)params_storage,
                                       nparams, datadir, cli_port);

    if (cli_static_agent_method(method)) {
        agent_runtime_availability_reset();
        rpc_agent_set_boot_context(app_operator_lane_name(operator_lane),
                                   app_runtime_profile_name(runtime_profile),
                                   datadir, cli_port, cli_p2p_port,
                                   cli_https_port, cli_fs_port);
        cli_probe_static_agent_target(datadir);
        return cli_run_static_agent_method(method, params_storage, nparams);
    }

    if (!cli_read_cookie(datadir)) {
        fprintf(stderr, "Node not running (no cookie at %s/.cookie)\n", datadir);
        return 1;
    }

    /* CLI UX contract: `zclassic23 dumpstate <subsystem> field=a,b` (and, in
     * principle, any other raw RPC method) — pull a bare `field=` token out
     * of the forwarded RPC params before the call, then select just those
     * names out of the response afterward. `dumpstate`'s result nests the
     * subsystem's own fields under "state" (see diag_rpc_dumpstate_builtin);
     * every other method is selected at the top level of "result". */
    const char *field_csv = NULL;
    bool saw_format_json = false;
    const char *rpc_params_storage[CLI_MAX_PARAMS];
    int rpc_nparams = 0;
    for (int i = 0; i < nparams; i++) {
        const char *p = params_storage[i];
        if (p && strncmp(p, "field=", 6) == 0 && p[6] && !field_csv) {
            field_csv = p + 6;
            continue;
        }
        if (p && strcmp(p, "--format=json") == 0)
            saw_format_json = true;
        if (rpc_nparams < CLI_MAX_PARAMS)
            rpc_params_storage[rpc_nparams++] = p;
    }
    /* CLI UX contract: ZCL_BRIEF=1 makes `dumpstate` default to the flat
     * field-selected rendering (every top-level key of the subsystem's own
     * `.state`) without typing `field=`; --format=json still escapes it. The
     * `status` is always compact native output; this is the raw-RPC-path twin for
     * dumpstate, which has no separate brief body to fetch). */
    const char *env_brief = getenv("ZCL_BRIEF");
    bool zcl_brief_on = env_brief && env_brief[0] && strcmp(env_brief, "0") != 0;
    bool auto_all_fields = !field_csv && zcl_brief_on && !saw_format_json &&
                           strcmp(method, "dumpstate") == 0;

    struct json_value jp;
    if (!rpc_convert_values(method, rpc_params_storage,
                            (size_t)rpc_nparams, &jp)) {
        fprintf(stderr, "Bad parameters\n");
        return 1;
    }

    char pbuf[32768];
    json_write(&jp, pbuf, sizeof(pbuf));
    json_free(&jp);

    char body[65536];
    int blen = snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"1.0\",\"id\":\"z\",\"method\":\"%s\",\"params\":%s}",
        method, pbuf);

    char *resp = cli_rpc_call(body, (size_t)blen);
    if (!resp) { fprintf(stderr, "RPC failed\n"); return 1; }
    char rpc_error_message[192];
    if (cli_rpc_error_is_method_not_found(resp, rpc_error_message,
                                          sizeof(rpc_error_message))) {
        if (cli_print_peer_incidents_dumpstate_fallback(method,
                                                        rpc_error_message)) {
            free(resp);
            return 0;
        }
        if (cli_print_contract_method_skew_diagnostic(method, datadir,
                                                      operator_lane,
                                                      runtime_profile,
                                                      rpc_error_message)) {
            free(resp);
            return 1;
        }
        cli_print_unknown_command_diagnostic(method);
        free(resp);
        return ZCL_COMMAND_EXIT_INVALID;
    }

    if (field_csv || auto_all_fields) {
        struct json_value root;
        bool handled = false;
        if (json_read(&root, resp, strlen(resp)) && root.type == JSON_OBJ) {
            const struct json_value *result = json_get(&root, "result");
            if (result && strcmp(method, "dumpstate") == 0) {
                const struct json_value *state = json_get(result, "state");
                if (state && state->type == JSON_OBJ)
                    result = state;
            }
            /* ZCL_BRIEF auto-select: no field= was given, so select every
             * top-level key of `result` — same selection function, no
             * second data path. */
            char all_fields[2048];
            const char *use_csv = field_csv;
            if (!use_csv && result && result->type == JSON_OBJ) {
                size_t alen = 0;
                for (size_t k = 0; k < result->num_children; k++) {
                    int an = snprintf(all_fields + alen,
                                     sizeof(all_fields) - alen, "%s%s",
                                     alen ? "," : "", result->keys[k]);
                    if (an > 0 && (size_t)an < sizeof(all_fields) - alen)
                        alen += (size_t)an;
                }
                use_csv = all_fields;
            }
            char sel[ZCL_COMMAND_LIST_BUDGET + 1];
            char selerr[320];
            if (result && use_csv && use_csv[0] &&
                zcl_native_render_field_selection(result, use_csv, sel,
                                                  sizeof(sel), selerr,
                                                  sizeof(selerr))) {
                fputs(sel, stdout);
                handled = true;
            } else if (field_csv) {
                /* Only an explicit field= failure is a hard error; an
                 * auto-select with nothing to select from just falls
                 * through to the normal full-result print below. */
                fprintf(stderr, "error=UNKNOWN_FIELD detail=%s try=%s\n",
                       result ? selerr : "this method returned no selectable "
                                          "result",
                       method);
                json_free(&root);
                free(resp);
                return ZCL_COMMAND_EXIT_INVALID;
            }
        }
        json_free(&root);
        if (handled) {
            free(resp);
            return 0;
        }
        if (field_csv) {
            free(resp);
            fprintf(stderr,
                   "error=UNKNOWN_FIELD detail=response did not parse "
                   "try=%s\n", method);
            return ZCL_COMMAND_EXIT_INVALID;
        }
        /* auto_all_fields found nothing to flatten (e.g. malformed
         * response) — fall through to the normal full print so ZCL_BRIEF
         * never hides a real error behind a silent no-op. */
    }

    int rc = rpc_cli_print_json_result(resp, stdout, stderr);
    free(resp);
    return rc;
}

/* ════════════════════════════════════════════════════════════════
 *  NODE MODE — full node daemon
 * ════════════════════════════════════════════════════════════════ */

volatile sig_atomic_t g_shutdown_requested = 0;

/* #8 — saved argv for in-process self-respawn (off-systemd liveness recovery).
 * Captured at the top of node mode (after the one-shot sub-modes return), used
 * only by the self-respawn execv after a clean app_shutdown. */
static char **g_saved_argv = NULL;

/* Alarm-based shutdown watchdog. Async-signal-safe.
 * Previous implementation used pthread_create from the signal handler
 * (not AS-safe) — under some kernel/glibc combinations the watchdog
 * thread never got CPU time and systemd's TimeoutStopSec=90 s fired
 * instead. Replaced with setitimer + SIGALRM handler: alarm() and
 * signal() ARE async-signal-safe, and the kernel guarantees SIGALRM
 * delivery at the scheduled time. */
static void shutdown_alarm_handler(int sig)
{
    (void)sig;
    static const char msg[] =
        "Shutdown watchdog: 90s timeout — forcing exit\n";
    /* write() is async-signal-safe; fprintf is not. */
    (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

static void signal_handler(int sig)
{
    if (g_shutdown_requested) {
        /* Repeated SIGTERM is normal under service managers and must be
         * idempotent. Keep Ctrl-C as the operator's immediate escape hatch. */
        if (sig == SIGINT)
            _exit(1);
        return;
    }
    g_shutdown_requested = 1;
    /* P7.9 — mirror to the thread_registry flag so every loop that
     * polls thread_registry_shutdown_requested() drains alongside the
     * legacy g_shutdown_requested readers. The setter is an atomic
     * store, safe to call from the signal handler. */
    thread_registry_request_shutdown();
    /* Schedule a forced exit if graceful shutdown cannot get control.
     * Startup may still be finishing when SIGTERM arrives, so this must
     * allow enough time for app_init to unwind into app_shutdown. */
    signal(SIGALRM, shutdown_alarm_handler);
    alarm(90);
}

static void print_usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s [node options]          Run full node\n", prog);
    printf("\nAgent/operator API commands (from agent_contracts.def):\n");
    agent_print_native_usage(stdout, prog);
    printf("  %s --agent                 Same compact status\n", prog);
    printf("  %s dev [branch...]         Native shallow LLM development tree\n", prog);
    printf("  %s <method> [params...]    RPC client\n\n", prog);
    printf("Node options:\n");
    printf("  -datadir=<dir>      Data directory\n");
    printf("  -paramsdir=<dir>    Params directory\n");
    printf("  -port=<port>        P2P port (default: 8033)\n");
    printf("  -rpcport=<port>     RPC port (default: 18232; 8232 is legacy zclassicd)\n");
    printf("  -addnode=<ip>       Add peer\n");
    printf("  -externalip=<ip[:port]>  Advertise this public P2P endpoint\n");
    printf("  -gen                Enable mining\n");
    printf("  -txindex            Transaction index\n");
    printf("  -tor                Start Tor hidden service (dynhost blog)\n");
    printf("  -gui                Launch the WebKit wallet GUI instead of the\n");
    printf("                      headless node (needs a display; default is\n");
    printf("                      headless node + REST/onion)\n");
    printf("  -httpsdomain=<dom>  TLS servername / HTTPS-redirect host for the\n");
    printf("                      clearnet explorer (optional; defaults to the\n");
    printf("                      request Host header with a single cert)\n");
    printf("  -profile=<name>     Service profile: full, zclassic-only, explorer, onion-node, legacy-compat\n");
    printf("  -operator-lane=<name>  Operator lane: canonical, soak, dev, test, copy\n");
    printf("  -nolegacyimport     Do not auto-read/link ~/.zclassic during boot\n");
    printf("  -hotswap-activate   Arm Tier-1 live hot-swap ACTIVATION (dev only;\n");
    printf("                      also needs ZCL_HOTSWAP_ACTIVATE=1 and the exact\n");
    printf("                      ~/.zclassic-c23-dev datadir; canonical refused).\n");
    printf("  -allow-plaintext-wallet  Create a new wallet UNENCRYPTED at rest\n");
    printf("                      (loud opt-in; otherwise set ZCL_WALLET_PASSPHRASE\n");
    printf("                      or first-run wallet creation refuses).\n");
    printf("  -backfill-nullifiers  One-shot owner-gated C-3 nullifier history backfill\n");
    printf("  -enforce-sapling-root  Reject ANY hashFinalSaplingRoot mismatch\n");
    printf("                      (default OFF: only all-zeros is rejected).\n");
    printf("                      DO NOT use on the live node until a full-history\n");
    printf("                      replay confirms zero false-rejects (h=478544).\n");
    printf("  -enforce-coinbase-maturity  Reject a live-path spend of a coinbase\n");
    printf("                      output younger than 100 blocks (default OFF).\n");
    printf("                      DO NOT use on the live node until a full-history\n");
    printf("                      replay confirms zero false-rejects (h=478544).\n");
    printf("  -enforce-checkdatasig-sigops  Count OP_CHECKDATASIG toward the\n");
    printf("                      per-block sigop ceiling in connect_block (default OFF).\n");
    printf("                      DO NOT use on the live node until a full-history\n");
    printf("                      replay confirms zero false-rejects (h=478544).\n");
    printf("  -rebuildfromlog     Rebuild block index + tip from the event-log\n");
    printf("                      projection (cold-start opt-in)\n");
    printf("  -bench              Run all five user benchmark probes\n");
    printf("  -bench-crypto-verify Bench Groth16 + Equihash-200,9 consensus verify (ns/op)\n");
    printf("  -bench-crypto-vs-rust Bench every consensus crypto primitive vs pinned Rust (ns/op)\n");
    printf("  -bench-regress      Fail if bench-history numeric rows regress >20%%\n");
    printf("  --decrypt-wallet-backup <src.enc> <dst.sqlite>\n");
    printf("                      Restore an encrypted wallet backup (password\n");
    printf("                      from WALLET_BACKUP_PASSWORD)\n");
    printf("  -help               This help\n\n");
    printf("RPC examples:\n");
    printf("  %s getblockcount\n", prog);
    printf("  %s getbalance\n", prog);
    printf("  %s z_gettotalbalance\n", prog);
    printf("  %s chainview 100 5\n", prog);
    printf("  %s z_sendmany \"zs1...\" '[{\"address\":\"zs1...\",\"amount\":0.001}]'\n", prog);
}

/* Detect CLI mode: first non-option arg doesn't start with '-' */
static bool is_cli_mode(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') return true;  /* RPC method name */
        if (strcmp(argv[i], "--agent") == 0 ||
            strcmp(argv[i], "-agent") == 0 ||
            strcmp(argv[i], "--status") == 0 ||
            strcmp(argv[i], "-status") == 0 ||
            strcmp(argv[i], "--summary") == 0 ||
            strcmp(argv[i], "-summary") == 0)
            return true;
        /* Skip known node options with values */
        if (strncmp(argv[i], "-datadir=", 9) == 0 ||
            strncmp(argv[i], "-rpcport=", 9) == 0 ||
            strncmp(argv[i], "-operator-lane=", 15) == 0 ||
            strncmp(argv[i], "-profile=", 9) == 0)
            continue;
        /* Any other -flag is a node option */
        return false;
    }
    return false;
}

/* ── --gen-utxo-snapshot mode ──────────────────────────────────
 *
 * Build-time tool: walks a legacy zclassicd chainstate LevelDB
 * (read via ldb_snapshot to avoid LOCK contention) and emits a
 * canonical UTXO sidecar ready for runtime mmap+SHA3-verify+
 * bulk-INSERT in phase 2.
 *
 * Usage: zclassic23 --gen-utxo-snapshot <legacy_datadir> <out_path>
 *
 * Output format (104-byte header + records, all little-endian):
 *   magic[8]="ZCLUTXO\0", version u32=1, reserved u32, height u32,
 *   reserved u32, count u64, total_supply i64,
 *   anchor_block_hash[32], sha3_hash[32]
 * Per record:
 *   txid[32], vout u32, value i64, script_len u32, script[*],
 *   height u32, is_coinbase u8
 *
 * Per-record encoding intentionally matches
 * lib/coins/src/utxo_commitment.c:utxo_commitment_sha3_compute()
 * so the computed SHA3 equals the compile-time checkpoint at
 * lib/chain/src/checkpoints.c (when run at the anchor height).
 */
struct gen_snap_ctx {
    FILE *out;
    struct sha3_256_ctx hasher;
    uint64_t records;
    uint64_t vouts;
    int64_t  total_supply;
    int      min_height;
    int      max_height;
    bool     fatal;
};

static void gen_snap_le32(uint8_t b[4], uint32_t v)
{ b[0]=(uint8_t)v; b[1]=(uint8_t)(v>>8); b[2]=(uint8_t)(v>>16); b[3]=(uint8_t)(v>>24); }
static void gen_snap_le64(uint8_t b[8], uint64_t v)
{ for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8*i)); }

static bool gen_snap_write(struct gen_snap_ctx *g, const void *p, size_t n)
{
    if (fwrite(p, 1, n, g->out) != n) {
        fprintf(stderr, "gen_utxo_snapshot: fwrite failed\n");
        g->fatal = true;
        return false;
    }
    sha3_256_write(&g->hasher, (const unsigned char *)p, n);
    return true;
}

static bool gen_snap_cb(const struct uint256 *txid,
                        const struct legacy_coins *coins,
                        void *ctx)
{
    struct gen_snap_ctx *g = ctx;
    if (g->fatal) return false;
    g->records++;
    if (g->min_height == 0 || coins->height < g->min_height)
        g->min_height = coins->height;
    if (coins->height > g->max_height)
        g->max_height = coins->height;

    for (size_t i = 0; i < coins->num_vouts; i++) {
        const struct legacy_coins_vout *o = &coins->vouts[i];
        uint8_t b[8];
        if (!gen_snap_write(g, txid->data, 32)) return false;
        gen_snap_le32(b, o->n);          if (!gen_snap_write(g, b, 4)) return false;
        gen_snap_le64(b, (uint64_t)o->value); if (!gen_snap_write(g, b, 8)) return false;
        gen_snap_le32(b, (uint32_t)o->script_len);
        if (!gen_snap_write(g, b, 4)) return false;
        if (o->script_len > 0 &&
            !gen_snap_write(g, o->script, o->script_len)) return false;
        gen_snap_le32(b, (uint32_t)coins->height);
        if (!gen_snap_write(g, b, 4)) return false;
        uint8_t cb = (uint8_t)(coins->coinbase ? 1 : 0);
        if (!gen_snap_write(g, &cb, 1)) return false;
        g->total_supply += o->value;
        g->vouts++;
    }
    return true;
}

static int gen_utxo_snapshot_mode(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr,
                "usage: %s --gen-utxo-snapshot <legacy_datadir> "
                "<out_sidecar_path>\n", argv[0]);
        return 2;
    }
    const char *legacy = argv[2];
    const char *out_path = argv[3];

    char chainstate_path[1024];
    snprintf(chainstate_path, sizeof(chainstate_path),
             "%s/chainstate", legacy);
    struct stat st;
    if (stat(chainstate_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "missing chainstate at %s\n", chainstate_path);
        return 2;
    }

    char snap_path[1200];
    snprintf(snap_path, sizeof(snap_path),
             "%s.snap_for_utxo_gen", chainstate_path);
    char snap_err[256] = {0};
    bool snap_ok = false;
    for (int t = 0; t < 3 && !snap_ok; t++) {
        snap_ok = ldb_snapshot_make(chainstate_path, snap_path,
                                    snap_err, sizeof(snap_err));
        if (!snap_ok && strcmp(snap_err, "manifest_changed") != 0) break;
    }
    if (!snap_ok) {
        fprintf(stderr, "ldb_snapshot_make(%s) failed: %s\n",
                chainstate_path, snap_err);
        return 1;
    }
    fprintf(stderr, "[gen_utxo] snapshot built at %s\n", snap_path);

    void *h = NULL;
    if (!chainstate_legacy_open(snap_path, &h) || !h) {
        fprintf(stderr, "chainstate_legacy_open failed\n");
        ldb_snapshot_destroy(snap_path);
        return 1;
    }

    struct uint256 best;
    uint256_set_null(&best);
    if (!chainstate_legacy_get_best_block(h, &best)) {
        fprintf(stderr, "no best block\n");
        chainstate_legacy_close(h);
        ldb_snapshot_destroy(snap_path);
        return 1;
    }
    char hex[65];
    uint256_get_hex(&best, hex);
    fprintf(stderr, "[gen_utxo] chainstate best block: %s\n", hex);

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "fopen(%s) failed\n", out_path);
        chainstate_legacy_close(h);
        ldb_snapshot_destroy(snap_path);
        return 1;
    }
    uint8_t header[104] = {0};
    if (fwrite(header, 1, sizeof(header), out) != sizeof(header)) {
        fclose(out); chainstate_legacy_close(h);
        ldb_snapshot_destroy(snap_path); return 1;
    }

    struct gen_snap_ctx g = { .out = out };
    sha3_256_init(&g.hasher);

    fprintf(stderr, "[gen_utxo] streaming UTXOs ...\n");
    int64_t n = chainstate_legacy_iter(h, gen_snap_cb, &g);
    if (n < 0 || g.fatal) {
        fprintf(stderr, "iter failed (n=%lld fatal=%d)\n",
                (long long)n, g.fatal);
        fclose(out); chainstate_legacy_close(h);
        ldb_snapshot_destroy(snap_path); return 1;
    }

    uint8_t body_sha3[32];
    sha3_256_finalize(&g.hasher, body_sha3);
    char shex[65];
    for (int i = 0; i < 32; i++)
        snprintf(shex + 2*i, 3, "%02x", body_sha3[i]);
    shex[64] = '\0';
    fprintf(stderr,
            "[gen_utxo] records=%llu vouts=%llu total=%.4f ZCL "
            "heights=[%d..%d]\n[gen_utxo] sha3=%s\n",
            (unsigned long long)g.records,
            (unsigned long long)g.vouts,
            (double)g.total_supply / 1e8,
            g.min_height, g.max_height, shex);

    memcpy(header, "ZCLUTXO\x00", 8);
    gen_snap_le32(header + 8, 1);
    gen_snap_le32(header + 16, (uint32_t)g.max_height);
    gen_snap_le64(header + 24, g.vouts);
    gen_snap_le64(header + 32, (uint64_t)g.total_supply);
    memcpy(header + 40, best.data, 32);
    memcpy(header + 72, body_sha3, 32);

    if (fseek(out, 0, SEEK_SET) != 0 ||
        fwrite(header, 1, sizeof(header), out) != sizeof(header)) {
        fprintf(stderr, "header rewrite failed\n");
        fclose(out); chainstate_legacy_close(h);
        ldb_snapshot_destroy(snap_path); return 1;
    }
    fclose(out);
    chainstate_legacy_close(h);
    ldb_snapshot_destroy(snap_path);

    fprintf(stderr, "[gen_utxo] wrote %s (%llu records, %llu vouts)\n",
            out_path, (unsigned long long)g.records,
            (unsigned long long)g.vouts);

    /* POST-WRITE CHECKPOINT ASSERT (mirror boot_mint_anchor's bless step): before
     * treating the artifact as SHIPPABLE, re-open it through the SAME loader the
     * boot path uses and bind expected_sha3 = the compiled checkpoint. uss_open
     * recomputes the full body SHA3 and rejects (NULL) on any header/body
     * mismatch, so this proves the freshly-written snapshot reproduces the
     * compiled checkpoint root + count. A non-anchor chainstate (e.g. the oracle
     * advanced past h=3,056,758) FAILS here — we leave the file on disk but exit
     * non-zero so an operator never ships an unverified set. This re-uses the
     * loader's SHA3 verification; it does not reimplement it. */
    {
        const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
        if (!cp) {
            fprintf(stderr, "[gen_utxo] FATAL: no compiled SHA3 UTXO checkpoint "
                    "to verify the snapshot against\n");
            return 1;
        }
        char verr[256] = {0};
        struct uss_header vhdr;
        struct uss_handle *vh = uss_open(out_path, /*verify_full_sha3=*/true,
                                         cp->sha3_hash, &vhdr, verr, sizeof(verr));
        if (!vh) {
            fprintf(stderr, "[gen_utxo] FATAL: written snapshot %s FAILED the "
                    "compiled-checkpoint SHA3 verify (%s) — NOT shippable. The "
                    "chainstate likely is not the anchor h=%d set.\n",
                    out_path, verr[0] ? verr : "sha3/header mismatch", cp->height);
            return 1;
        }
        bool count_ok = (vhdr.count == cp->utxo_count);
        uss_close(vh);
        if (!count_ok) {
            fprintf(stderr, "[gen_utxo] FATAL: written snapshot count=%llu != "
                    "compiled checkpoint %llu — NOT shippable.\n",
                    (unsigned long long)vhdr.count,
                    (unsigned long long)cp->utxo_count);
            return 1;
        }
        fprintf(stderr, "[gen_utxo] VERIFIED: snapshot reproduces the compiled "
                "checkpoint (h=%d, count=%llu, SHA3 OK) — shippable.\n",
                cp->height, (unsigned long long)cp->utxo_count);
    }
    return 0;
}

/* ── -import-complete-shielded=<zclassicd-datadir> mode ─────────────────
 *
 * Owner-gated, copy-prove-gated: import the COMPLETE historical Sprout+Sapling
 * anchor + nullifier set from a zclassicd chainstate into the TARGET datadir's
 * progress.kv, atomically, and flip both activation cursors to 0 — clearing the
 * utxo_apply.anchor_backfill_gap + nullifier_backfill_gap wedge WITHOUT a
 * from-genesis fold (docs/work/fast-sync-to-tip-plan-2026-07-16.md §4).
 *
 * NOT auto-run on any live datadir: this REFUSES the operator's live canonical
 * (~/.zclassic-c23) and mint (~/.zclassic-c23-mint) datadirs by construction;
 * point it at a -datadir=<COPY> (the copy-prove harness flow, §6). The source
 * chainstate is read through a point-in-time ldb_snapshot (never the live one).
 *
 * Usage: zclassic23 -datadir=<TARGET-COPY>
 *        -import-complete-shielded=<zclassicd-datadir> */
static bool import_shielded_is_live_datadir(const char *target)
{
    const char *home = getenv("HOME");
    if (!home || !target)
        return false;
    char live[600], mint[600];
    snprintf(live, sizeof(live), "%s/.zclassic-c23", home);
    snprintf(mint, sizeof(mint), "%s/.zclassic-c23-mint", home);
    /* Compare with any single trailing slash normalized away. */
    size_t tl = strlen(target);
    while (tl > 1 && target[tl - 1] == '/') tl--;
    return (strlen(live) == tl && strncmp(target, live, tl) == 0) ||
           (strlen(mint) == tl && strncmp(target, mint, tl) == 0);
}

static int import_complete_shielded_mode(int argc, char **argv)
{
    /* UX: glibc block-buffers stdout when it isn't a tty (the common case
     * for a redirected/logged import run), so every printf() below would
     * sit in the buffer until process exit. For a run that goes CPU-bound
     * inside shielded_history_import_from_chainstate() for many minutes,
     * that means the operator watches a blank terminal with no way to
     * tell "running" from "stuck" — a silent stall, which the node's
     * prime invariant forbids. Force line buffering up front so every
     * banner below (Source/Target/Chainstate snapshot/Tip bind/
     * IMPORT COMPLETE|REFUSED) lands on the terminal/log the instant it's
     * printed. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    const char *home = getenv("HOME");
    const char *src = NULL;
    char target[512];
    int tn = snprintf(target, sizeof(target), "%s/.zclassic-c23",
                       home ? home : ".");
    if (tn < 0 || (size_t)tn >= sizeof(target)) {
        fprintf(stderr,
                "-import-complete-shielded: $HOME is too long for the "
                "%zu-byte target datadir buffer — refusing (no silent "
                "truncation); pass -datadir=<short path> explicitly\n",
                sizeof(target));
        return 2;
    }

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-import-complete-shielded=", 26) == 0) {
            src = argv[i] + 26;
        } else if (strncmp(argv[i], "-datadir=", 9) == 0) {
            int n = snprintf(target, sizeof(target), "%s", argv[i] + 9);
            if (n < 0 || (size_t)n >= sizeof(target)) {
                fprintf(stderr,
                        "-datadir=<...> exceeds the %zu-byte path buffer — "
                        "refusing (no silent truncation)\n", sizeof(target));
                return 2;
            }
        }
    }
    if (!src || !src[0]) {
        fprintf(stderr,
                "usage: %s -datadir=<TARGET-COPY> "
                "-import-complete-shielded=<zclassicd-datadir>\n", argv[0]);
        return 2;
    }

    printf("=== ZClassic Complete Shielded-History Import ===\n");
    printf("Source chainstate: %s/chainstate\n", src);
    printf("Target datadir:    %s\n", target);

    /* Containment: NEVER write shielded history into a live datadir. The cure
     * is copy-proven on a COPY first (see §6); the operator points -datadir at
     * the copy, then cuts over by re-running against canonical after a green
     * copy-prove. This mode itself refuses the two known live paths. */
    if (import_shielded_is_live_datadir(target)) {
        fprintf(stderr,
                "REFUSING: %s is a live datadir. Copy-prove on a COPY first "
                "(cp -a the datadir + chainstate, run this against the copy, "
                "gate on H* climb + tip-hash parity vs zclassicd), then cut "
                "over. See docs/work/fast-sync-to-tip-plan-2026-07-16.md §6.\n",
                target);
        return 1;
    }

    /* Point-in-time snapshot of the source chainstate (never read the live
     * LevelDB directly — mirrors --gen-utxo-snapshot). */
    char cs_src[700], snap_path[900];
    int csn = snprintf(cs_src, sizeof(cs_src), "%s/chainstate", src);
    if (csn < 0 || (size_t)csn >= sizeof(cs_src)) {
        fprintf(stderr,
                "-import-complete-shielded=<path> exceeds the %zu-byte "
                "chainstate path buffer — refusing (no silent truncation)\n",
                sizeof(cs_src));
        return 1;
    }
    struct stat st;
    if (stat(cs_src, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "missing chainstate at %s\n", cs_src);
        return 1;
    }
    int spn = snprintf(snap_path, sizeof(snap_path),
                       "%s/shielded_import_cs_snap", target);
    if (spn < 0 || (size_t)spn >= sizeof(snap_path)) {
        fprintf(stderr,
                "-datadir=<...> exceeds the %zu-byte snapshot path buffer — "
                "refusing (no silent truncation)\n", sizeof(snap_path));
        return 1;
    }
    char snap_err[256] = {0};
    bool snap_ok = false;
    for (int t = 0; t < 4 && !snap_ok; t++) {
        snap_ok = ldb_snapshot_make(cs_src, snap_path, snap_err,
                                    sizeof(snap_err));
        if (!snap_ok && strcmp(snap_err, "manifest_changed") != 0) break;
    }
    if (!snap_ok) {
        fprintf(stderr, "ldb_snapshot_make(%s) failed: %s\n", cs_src, snap_err);
        return 1;
    }
    printf("Chainstate snapshot: %s\n", snap_path);

    /* Read the chainstate best block, then bind it to OUR header chain: the
     * block's hashFinalSaplingRoot (blocks.sapling_root) is the chain-committed
     * tip Sapling root the importer verifies the frontier against. */
    struct uint256 best_block;
    uint256_set_null(&best_block);
    {
        void *rh = NULL;
        if (!chainstate_legacy_open(snap_path, &rh) || !rh ||
            !chainstate_legacy_get_best_block(rh, &best_block)) {
            if (rh) chainstate_legacy_close(rh);
            ldb_snapshot_destroy(snap_path);
            fprintf(stderr, "cannot read chainstate best block\n");
            return 1;
        }
        chainstate_legacy_close(rh);
    }

    int64_t tip_height = -1;
    struct uint256 tip_sapling_root;
    uint256_set_null(&tip_sapling_root);
    bool root_populated = false;
    {
        char db_path[600];
        snprintf(db_path, sizeof(db_path), "%s/node.db", target);
        struct node_db ndb;
        if (!node_db_open(&ndb, db_path)) {
            ldb_snapshot_destroy(snap_path);
            fprintf(stderr, "cannot open %s (import --importblockindex the "
                            "header chain first)\n", db_path);
            return 1;
        }
        /* The header-committed hashFinalSaplingRoot lives in the block index
         * projection (blocks.sapling_root), populated from the block HEADER by
         * --importblockindex — NOT a value that requires the block body / full
         * connection. Read it via the model so the bind source is exactly the
         * column the header import writes. */
        struct db_block blk;
        if (db_block_find_by_hash(&ndb, best_block.data, &blk) &&
            blk.status >= BLOCK_VALID_TRANSACTIONS) {
            tip_height = blk.height;
            memcpy(tip_sapling_root.data, blk.sapling_root, 32);
            root_populated = !uint256_is_null(&tip_sapling_root);
        }
        node_db_close(&ndb);
    }
    if (tip_height < 0) {
        ldb_snapshot_destroy(snap_path);
        fprintf(stderr,
                "chainstate best block not found in the target header chain — "
                "cannot bind the tip Sapling root. Run --importblockindex "
                "against the same zclassicd datadir first.\n");
        return 1;
    }
    if (!root_populated) {
        ldb_snapshot_destroy(snap_path);
        fprintf(stderr,
                "tip bind SOURCE is all-zero: blocks.sapling_root at the "
                "chainstate best block (height=%lld) is null. This header "
                "chain was imported by a build that did not persist the block "
                "header's hashFinalSaplingRoot. Re-run `zclassic23 "
                "--importblockindex <zclassicd-datadir>` with the current "
                "build (which writes sapling_root from the header), then retry "
                "-import-complete-shielded. Refusing rather than binding the "
                "tip frontier against zeros.\n", (long long)tip_height);
        return 1;
    }
    char root_hex[65];
    uint256_get_hex(&tip_sapling_root, root_hex);
    printf("Tip bind: height=%lld hashFinalSaplingRoot=%s\n",
           (long long)tip_height, root_hex);

    if (!progress_store_open(target)) {
        ldb_snapshot_destroy(snap_path);
        fprintf(stderr, "cannot open progress.kv in %s\n", target);
        return 1;
    }
    struct shielded_import_report rep = {0};
    bool ok = shielded_history_import_from_chainstate(
        progress_store_db(), snap_path, tip_height, &tip_sapling_root, &rep);

    /* On success, register the cured coins tip as a cold-import TRUST anchor so
     * the next normal boot's Invariant A gate installs it into the active chain
     * (instead of refusing the detached island and stalling getheaders at
     * genesis). Durable node_db seed + in-memory; best-effort — a derive miss
     * logs but never undoes the committed import. progress_store stays open so
     * the coins-best derivation + the durable count token read the canonical
     * store. */
    if (ok) {
        char ndb_path[600];
        int nn = snprintf(ndb_path, sizeof(ndb_path), "%s/node.db", target);
        struct node_db ndb2;
        if (nn > 0 && (size_t)nn < sizeof(ndb_path) &&
            node_db_open(&ndb2, ndb_path)) {
            (void)shielded_history_import_register_cured_tip_trust_anchor(
                progress_store_db(), &ndb2);
            node_db_close(&ndb2);
        } else {
            fprintf(stderr,
                    "WARNING: import committed but could not reopen %s to "
                    "persist the cured-tip trust anchor; the coins tip may need "
                    "one extra boot to install into the active chain.\n",
                    ndb_path);
        }
    }

    /* Re-arm proof_validate over the pre-2026-07-13 NULL-block_hash suffix so the
     * post-cure fold is not wedged at utxo_apply's label_splice guard (which
     * correctly refuses a hashless proof verdict). The rewind FLOOR is
     * utxo_apply's own cursor (LCC-safe — never rewinds an already-applied
     * height). Contained: a rewind beyond the recovery block-rollback cap
     * (default 100) is REFUSED; raise ZCL_MAX_BLOCK_ROLLBACK past the reported
     * depth after a copy proof. progress_store is still open here. */
    if (ok) {
        struct proof_validate_rearm_report rr;
        memset(&rr, 0, sizeof(rr));
        enum proof_validate_rearm_outcome ro =
            proof_validate_null_hash_rearm(progress_store_db(), NULL, &rr);
        if (ro == PV_REARM_REARMED) {
            printf("proof_validate re-arm: rewound cursor %llu->%llu, deleted "
                   "%lld NULL-block_hash rows; the next boot re-derives them.\n",
                   (unsigned long long)rr.pv_cursor_before,
                   (unsigned long long)rr.rewound_to, (long long)rr.deleted_rows);
        } else if (ro == PV_REARM_REFUSED) {
            fprintf(stderr,
                    "WARNING: proof_validate re-arm REFUSED — rewinding to h=%d "
                    "(%lld NULL-block_hash rows) exceeds the recovery "
                    "block-rollback cap. Raise ZCL_MAX_BLOCK_ROLLBACK past it "
                    "and re-run, else the post-cure fold wedges at utxo_apply.\n",
                    rr.lowest_null_height, (long long)rr.null_row_count);
        } else if (ro == PV_REARM_ERROR) {
            fprintf(stderr, "WARNING: proof_validate re-arm ERROR — see "
                            "node.log [proof_validate]; the fold may wedge.\n");
        }
        /* PV_REARM_NOT_NEEDED: the fold above utxo_apply is already
         * hash-complete — nothing to do. */
    }

    progress_store_close();
    ldb_snapshot_destroy(snap_path);

    if (!ok) {
        fprintf(stderr,
                "IMPORT REFUSED — nothing committed, wedge intact (both "
                "activation cursors stay POSITIVE, gap blockers remain). See "
                "node.log [shielded_import] for the exact anomaly.\n");
        return 1;
    }
    printf("IMPORT COMPLETE (committed=%d): sapling_anchors=%lld "
           "sprout_anchors=%lld sapling_nf=%lld sprout_nf=%lld\n"
           "Both activation cursors flipped to 0; the reducer resumes folding "
           "from %lld. Copy-prove H* climb + tip-hash parity vs zclassicd "
           "before cutover.\n",
           rep.committed, (long long)rep.sapling_anchors,
           (long long)rep.sprout_anchors, (long long)rep.sapling_nullifiers,
           (long long)rep.sprout_nullifiers, (long long)tip_height + 1);
    return 0;
}

/* Opt-in log-level filter (Phase E3). -loglevel=<all|info|warn|error|fatal|off>
 * raises the floor the LOG_ and GUARD macros (log_macros.h) emit at. Default
 * stays ZCL_LOG_ALL (zero behavior change) unless the flag is present. An
 * unrecognized value is a warning, never a boot abort — see
 * zcl_log_level_from_string()'s contract in util/log_level.h. */
static void apply_argv_loglevel(void)
{
    const char *raw = GetArg("-loglevel", NULL);
    if (!raw || !raw[0])
        return;

    enum zcl_log_level level;
    if (zcl_log_level_from_string(raw, &level)) {
        zcl_log_level_set(level);
    } else {
        LOG_WARN("boot", "unrecognized -loglevel=%s (want "
                 "all|info|warn|error|fatal|off) — keeping ALL", raw);
    }
}

int main(int argc, char **argv)
{
    ParseParameters(argc, (const char *const *)argv);
    apply_argv_loglevel();

    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "-bench", 6) == 0)
            return bench_mode_main(argc, argv);
    }

    /* CLI UX contract: bare `zclassic23`, zero arguments. The real node
     * service NEVER invokes the binary this way (deploy/zclassic23.service's
     * ExecStart always passes -datadir=/-rpcport=/etc — see
     * docs/NATIVE_COMMAND_INTERFACE.md "CLI UX contract"), so this is a
     * human at a shell prompt, not a boot attempt. Print the ONE-LINE status
     * brief + one suggested next command instead of silently booting a
     * default-config node underfoot. Delegates to cli_main with a synthetic
     * argv so datadir/rpcport resolution (cookie lookup, service exec-arg
     * fallback) is the exact same code path `zclassic23 status --next`
     * already uses — no duplicated logic. */
    if (argc == 1) {
        char *synthetic[] = {
            argv[0], (char *)"status", (char *)"--next",
            NULL,
        };
        return cli_main(3, synthetic);
    }

    /* CLI mode: zclassic23 getblockcount */
    if (argc > 1 && is_cli_mode(argc, argv))
        return cli_main(argc, argv);

    /* --gen-utxo-snapshot: build sidecar UTXO file from legacy datadir */
    if (argc >= 2 && strcmp(argv[1], "--gen-utxo-snapshot") == 0)
        return gen_utxo_snapshot_mode(argc, argv);

    /* -import-complete-shielded=<zclassicd-datadir>: owner-gated, copy-prove-
     * gated complete historical anchor+nullifier import into a TARGET-COPY
     * datadir (refuses live datadirs). Scanned across argv (it follows
     * -datadir=), not positional. */
    for (int i = 1; i < argc; i++)
        if (strncmp(argv[i], "-import-complete-shielded=", 26) == 0)
            return import_complete_shielded_mode(argc, argv);

    /* Wallet backup restore mode — decrypt an encrypted wallet backup.
     * Usage: zclassic23 --decrypt-wallet-backup <src.enc> <dst.sqlite>
     * Password comes from the WALLET_BACKUP_PASSWORD environment
     * variable (the same variable the node encrypts with). This is the
     * disaster-recovery path: without it, encrypted backups would be
     * unusable in the exact key-loss scenario they exist for. */
    if (argc >= 2 && strcmp(argv[1], "--decrypt-wallet-backup") == 0) {
        if (argc < 4) {
            fprintf(stderr,
                "Usage: %s --decrypt-wallet-backup <src.enc> <dst.sqlite>\n",
                argv[0]);
            return 1;
        }
        const char *password = getenv("WALLET_BACKUP_PASSWORD");
        if (!password || !*password) {
            fprintf(stderr,
                "--decrypt-wallet-backup: WALLET_BACKUP_PASSWORD is not "
                "set.\nSet it to the password the backups were encrypted "
                "with and retry.\n");
            return 1;
        }
        struct zcl_result dr =
            wallet_backup_decrypt_file(argv[2], argv[3], password);
        if (!dr.ok) {
            fprintf(stderr, "Decrypt failed: code=%d %s\n",
                    dr.code, dr.message);
            return 1;
        }
        printf("Decrypted %s -> %s\n", argv[2], argv[3]);
        return 0;
    }

    /* UTXO repair mode — fetch missing UTXOs from zclassicd, no full node.
     * Usage: zclassic23 --repair [num_blocks] [port] [creds]
     * Scans blocks ahead of current tip via zclassicd RPC, inserts missing
     * UTXOs into SQLite with correct byte order. Restart node after. */
    if (argc >= 2 && strcmp(argv[1], "--repair") == 0) {
        int num_blocks = argc > 2 ? atoi(argv[2]) : 5000;
        int port = argc > 3 ? atoi(argv[3]) : 8232;
        const char *creds = argc > 4 ? argv[4] : "zcluser:zclpass";
        const char *home = getenv("HOME");
        char db_path[512];
        snprintf(db_path, sizeof(db_path), "%s/.zclassic-c23/node.db",
                 home ? home : ".");

        printf("=== UTXO Repair ===\n");
        printf("DB:     %s\n", db_path);
        printf("Source: zclassicd on port %d\n", port);
        printf("Scan:   %d blocks ahead\n\n", num_blocks);

        sqlite3 *db = NULL;
        if (sqlite3_open(db_path, &db) != SQLITE_OK) {
            fprintf(stderr, "Cannot open %s\n", db_path);
            return 1;
        }

        /* Get current tip */
        int tip = 0;
        { sqlite3_stmt *s = NULL;
          sqlite3_prepare_v2(db, "SELECT MAX(height) FROM blocks", -1, &s, NULL);
          if (s && AR_STEP_ROW_READONLY(s) == SQLITE_ROW) tip = sqlite3_column_int(s, 0);
          if (s) sqlite3_finalize(s);
        }
        printf("Current tip: %d\n", tip);

        /* Get zclassicd tip */
        char rbuf[4096];
        int ztip = 0;
        if (rpc_call_local(port, creds, "getblockcount", "[]",
                            rbuf, sizeof(rbuf)) > 0) {
            const char *body = rpc_http_body(rbuf);
            if (body) {
                const char *rp = strstr(body, "\"result\":");
                if (rp) ztip = (int)strtol(rp + 9, NULL, 10);
            }
        }
        if (ztip == 0) {
            fprintf(stderr, "Cannot reach zclassicd on port %d\n", port);
            sqlite3_close(db);
            return 1;
        }
        int scan_end = tip + num_blocks;
        if (scan_end > ztip) scan_end = ztip;
        printf("zclassicd tip: %d\nScan range: %d → %d\n\n", ztip, tip+1, scan_end);

        sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
        sqlite3_stmt *ins = NULL;
        sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO utxos"
            "(txid,vout,value,script,script_type,address_hash,height,is_coinbase)"
            " VALUES(?,?,?,?,?,?,?,0)", -1, &ins, NULL);

        int fixed = 0, checked = 0;

        for (int h = tip + 1; h <= scan_end; h++) {
            /* Get block hash */
            char params[64];
            snprintf(params, sizeof(params), "[%d]", h);
            char hbuf[256];
            if (rpc_call_local(port, creds, "getblockhash", params,
                                hbuf, sizeof(hbuf)) <= 0) break;
            const char *hbody = rpc_http_body(hbuf);
            if (!hbody) break;
            const char *hp = strstr(hbody, "\"result\":\"");
            if (!hp) break;
            char bhash[65] = "";
            { const char *s = hp + 10; int i = 0;
              while (*s && *s != '"' && i < 64) bhash[i++] = *s++;
              bhash[i] = '\0'; }

            /* Get block with full tx data */
            char bparams[128];
            snprintf(bparams, sizeof(bparams), "[\"%s\",2]", bhash);
            char *bbuf = malloc(2*1024*1024); /* 2MB for block data */
            if (!bbuf) break;
            int brc = rpc_call_local(port, creds, "getblock", bparams,
                                      bbuf, 2*1024*1024);
            if (brc <= 0) { free(bbuf); break; }
            const char *bbody = rpc_http_body(bbuf);
            if (!bbody) { free(bbuf); break; }

            /* Parse inputs: find each "vin":[{"txid":"...","vout":N}] */
            const char *p = bbody;
            while ((p = strstr(p, "\"vin\"")) != NULL) {
                p += 5;
                /* Walk through vin array entries */
                while ((p = strstr(p, "\"txid\"")) != NULL) {
                    p += 6;
                    const char *q = strchr(p, '"');
                    if (!q) break;
                    q++;
                    const char *end = strchr(q, '"');
                    if (!end || end - q != 64) { p = end ? end : q; continue; }

                    char txid_hex[65];
                    memcpy(txid_hex, q, 64);
                    txid_hex[64] = '\0';

                    /* Get vout */
                    const char *vp = strstr(end, "\"vout\"");
                    if (!vp) break;
                    int vout = (int)strtol(vp + 7, NULL, 10);
                    checked++;

                    /* Reverse txid for internal byte order */
                    uint8_t txid_bin[32];
                    for (int i = 0; i < 32; i++) {
                        char hex2[3] = { txid_hex[62-2*i], txid_hex[63-2*i], 0 };
                        txid_bin[i] = (uint8_t)strtol(hex2, NULL, 16);
                    }

                    /* Check if exists */
                    sqlite3_stmt *chk = NULL;
                    sqlite3_prepare_v2(db,
                        "SELECT 1 FROM utxos WHERE txid=? AND vout=?",
                        -1, &chk, NULL);
                    sqlite3_bind_blob(chk, 1, txid_bin, 32, SQLITE_STATIC);
                    sqlite3_bind_int(chk, 2, vout);
                    bool exists = (AR_STEP_ROW_READONLY(chk) == SQLITE_ROW);
                    sqlite3_finalize(chk);

                    if (!exists) {
                        /* Fetch from zclassicd */
                        char txp[128];
                        snprintf(txp, sizeof(txp), "[\"%s\",1]", txid_hex);
                        char txbuf[65536];
                        if (rpc_call_local(port, creds, "getrawtransaction", txp,
                                            txbuf, sizeof(txbuf)) > 0) {
                            const char *txbody = rpc_http_body(txbuf);
                            if (txbody) {
                                /* Find the output at vout index */
                                /* Find "vout":[...{...value...scriptPubKey...}...] */
                                const char *vouts = strstr(txbody, "\"vout\"");
                                if (vouts) {
                                    /* Skip to the vout-th entry */
                                    const char *entry = vouts;
                                    for (int vi = 0; vi <= vout && entry; vi++)
                                        entry = strstr(entry + 1, "\"value\"");
                                    if (entry) {
                                        double val = strtod(entry + 8, NULL);
                                        int64_t val_sat = (int64_t)(val * 1e8 + 0.5);

                                        /* Find scriptPubKey hex */
                                        const char *sp = strstr(entry, "\"hex\":\"");
                                        uint8_t script[520] = {0};
                                        int script_len = 0;
                                        if (sp) {
                                            sp += 7;
                                            const char *se = strchr(sp, '"');
                                            if (se) {
                                                script_len = (int)(se - sp) / 2;
                                                if (script_len > 520) script_len = 520;
                                                for (int si = 0; si < script_len; si++) {
                                                    char h2[3] = { sp[si*2], sp[si*2+1], 0 };
                                                    script[si] = (uint8_t)strtol(h2, NULL, 16);
                                                }
                                            }
                                        }

                                        int stype = 0;
                                        uint8_t addr_hash[20] = {0};
                                        if (script_len == 25 && script[0] == 0x76) {
                                            memcpy(addr_hash, script + 3, 20);
                                        } else if (script_len == 23 && script[0] == 0xa9) {
                                            memcpy(addr_hash, script + 2, 20);
                                            stype = 1;
                                        }

                                        /* Get block height */
                                        int txht = 0;
                                        const char *bhp = strstr(txbody, "\"blockhash\"");
                                        if (bhp) {
                                            /* Get height from block header */
                                            const char *bhs = strchr(bhp + 11, '"');
                                            if (bhs) {
                                                bhs++;
                                                char bhhex[65] = "";
                                                const char *bhe = strchr(bhs, '"');
                                                if (bhe && bhe - bhs == 64) {
                                                    memcpy(bhhex, bhs, 64);
                                                    char bhp2[128];
                                                    snprintf(bhp2, sizeof(bhp2), "[\"%s\"]", bhhex);
                                                    char bhbuf[4096];
                                                    if (rpc_call_local(port, creds, "getblockheader", bhp2,
                                                                        bhbuf, sizeof(bhbuf)) > 0) {
                                                        const char *bhb = rpc_http_body(bhbuf);
                                                        if (bhb) {
                                                            const char *hhp = strstr(bhb, "\"height\":");
                                                            if (hhp) txht = (int)strtol(hhp + 9, NULL, 10);
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        /* Only insert UTXOs created at or below
                                         * our tip. UTXOs from blocks above the
                                         * tip will be created by connect_block
                                         * when those blocks are connected.
                                         * Inserting them early causes BIP30
                                         * violations (duplicate txid). */
                                        if (txht > 0 && txht <= tip) {
                                        sqlite3_reset(ins);
                                        sqlite3_bind_blob(ins, 1, txid_bin, 32, SQLITE_STATIC);
                                        sqlite3_bind_int(ins, 2, vout);
                                        sqlite3_bind_int64(ins, 3, val_sat);
                                        sqlite3_bind_blob(ins, 4, script, script_len, SQLITE_STATIC);
                                        sqlite3_bind_int(ins, 5, stype);
                                        sqlite3_bind_blob(ins, 6, addr_hash, 20, SQLITE_STATIC);
                                        sqlite3_bind_int(ins, 7, txht);
                                        AR_STEP_WRITE(ins);
                                        fixed++;
                                        }
                                    }
                                }
                            }
                        }
                    }

                    p = end + 1;
                }
                break; /* only process first vin array per tx */
            }

            if (h % 200 == 0) {
                sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
                sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
                printf("  h=%d  checked=%d  fixed=%d\n", h, checked, fixed);
                fflush(stdout);
            }

            free(bbuf);
        }

        if (ins) sqlite3_finalize(ins);
        sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);

        /* OFFLINE CACHE RESET: 'coins_best_block' is a
         * projection key — authority = reducer_frontier_derive_coins_best
         * over coins_kv. The --repair modified the utxos mirror directly,
         * so refresh its cached anchor to match; the derivation is
         * unaffected. */
        if (fixed > 0) {
            sqlite3_stmt *tip_s = NULL;
            sqlite3_prepare_v2(db,
                "SELECT hash FROM blocks WHERE height = "
                "(SELECT MAX(height) FROM blocks)",
                -1, &tip_s, NULL);
            if (tip_s && AR_STEP_ROW_READONLY(tip_s) == SQLITE_ROW) {
                const void *tip_hash = sqlite3_column_blob(tip_s, 0);
                int tip_len = sqlite3_column_bytes(tip_s, 0);
                if (tip_hash && tip_len >= 32) {
                    sqlite3_stmt *up = NULL;
                    sqlite3_prepare_v2(db,
                        "INSERT OR REPLACE INTO node_state(key,value)"
                        " VALUES('coins_best_block',?)",
                        -1, &up, NULL);
                    if (up) {
                        sqlite3_bind_blob(up, 1, tip_hash, tip_len,
                                          SQLITE_STATIC);
                        AR_STEP_WRITE(up);
                        sqlite3_finalize(up);
                        printf("Reset coins_best_block to tip\n");
                    }
                }
            }
            if (tip_s) sqlite3_finalize(tip_s);
        }

        sqlite3_exec(db, "PRAGMA wal_checkpoint(TRUNCATE)", NULL, NULL, NULL);
        sqlite3_close(db);

        printf("\nDone: checked %d inputs, fixed %d UTXOs\n", checked, fixed);
        printf("Restart the node to apply.\n");
        return 0;
    }

    /* Direct chainstate import mode — no full node startup needed.
     * Usage: zclassic23 --importchainstate /path/to/chainstate [dbpath] */
    if (argc >= 3 && strcmp(argv[1], "--importchainstate") == 0) {
        const char *cs_path = argv[2];
        const char *home = getenv("HOME");
        char db_path[512];
        if (argc > 3) {
            /* Positional target, NOT a flag: a '-datadir=...' here would be
             * silently treated as a literal db filename (and fed to shell
             * cp/rm commands). */
            if (argv[3][0] == '-') {
                fprintf(stderr,
                        "usage: zclassic23 --importchainstate <chainstate-dir>"
                        " [<target-node.db-path>]\n"
                        "       (got flag-like target '%s'; pass the node.db"
                        " PATH, e.g. ~/.zclassic-c23/node.db)\n", argv[3]);
                return 1;
            }
            snprintf(db_path, sizeof(db_path), "%s", argv[3]);
        } else
            snprintf(db_path, sizeof(db_path), "%s/.zclassic-c23/node.db",
                     home ? home : ".");

        printf("=== ZClassic UTXO Import ===\n");
        printf("Source: %s (LevelDB chainstate)\n", cs_path);
        printf("Target: %s (SQLite)\n\n", db_path);

        struct node_db ndb;
        if (!node_db_open(&ndb, db_path)) {
            fprintf(stderr, "Cannot open SQLite: %s\n", db_path);
            return 1;
        }

        struct coins_view_db cvdb;
        memset(&cvdb, 0, sizeof(cvdb));
        if (!coins_view_db_open(&cvdb, cs_path, 512, false, false)) {
            fprintf(stderr, "Cannot open LevelDB: %s\n", cs_path);
            node_db_close(&ndb);
            return 1;
        }

        int64_t t0 = (int64_t)time(NULL);
        int count = node_db_sync_import_utxos(&ndb, &cvdb);
        int64_t t1 = (int64_t)time(NULL);
        coins_view_db_close(&cvdb);

        if (count < 0) {
            fprintf(stderr, "Import failed\n");
            node_db_close(&ndb);
            return 1;
        }
        printf("\nImported %d UTXOs in %llds\n", count, (long long)(t1 - t0));

        /* Rebuild wallet_utxos from ground truth */
        printf("Rebuilding wallet_utxos...\n");
        sqlite3_exec(ndb.db, "BEGIN", NULL, NULL, NULL);
        if (ar_exec_write_sql(ndb.db, "DELETE FROM wallet_utxos") != SQLITE_OK ||
            ar_exec_write_sql(ndb.db,
            "INSERT INTO wallet_utxos "
            "(txid, vout, value, address_hash, script, height, is_coinbase) "
            "SELECT u.txid, u.vout, u.value, u.address_hash, u.script, "
            "u.height, u.is_coinbase "
            "FROM utxos u INNER JOIN wallet_keys wk "
            "ON u.address_hash = wk.pubkey_hash") != SQLITE_OK) {
            fprintf(stderr, "wallet_utxos rebuild failed: %s\n",
                    sqlite3_errmsg(ndb.db));
            sqlite3_exec(ndb.db, "ROLLBACK", NULL, NULL, NULL);
            node_db_close(&ndb);
            return 1;
        }
        sqlite3_exec(ndb.db, "COMMIT", NULL, NULL, NULL);

        /* Rebuild addresses */
        printf("Rebuilding addresses...\n");
        sqlite3_exec(ndb.db, "BEGIN", NULL, NULL, NULL);
        if (ar_exec_write_sql(ndb.db, "DELETE FROM addresses") != SQLITE_OK ||
            ar_exec_write_sql(ndb.db,
            "INSERT OR REPLACE INTO addresses "
            "(address_hash, script_type, balance, utxo_count, "
            "first_seen_height, last_seen_height) "
            "SELECT address_hash, MAX(script_type), SUM(value), count(*), "
            "MIN(height), MAX(height) "
            "FROM utxos WHERE address_hash IS NOT NULL "
            "GROUP BY address_hash") != SQLITE_OK) {
            fprintf(stderr, "addresses rebuild failed: %s\n",
                    sqlite3_errmsg(ndb.db));
            sqlite3_exec(ndb.db, "ROLLBACK", NULL, NULL, NULL);
            node_db_close(&ndb);
            return 1;
        }
        sqlite3_exec(ndb.db, "COMMIT", NULL, NULL, NULL);

        /* Verify results */
        sqlite3_stmt *s = NULL;
        int64_t utxo_count = 0, utxo_sum = 0;
        sqlite3_prepare_v2(ndb.db,
            "SELECT count(*), COALESCE(SUM(value),0) FROM utxos",
            -1, &s, NULL);
        if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
            utxo_count = sqlite3_column_int64(s, 0);
            utxo_sum = sqlite3_column_int64(s, 1);
        }
        sqlite3_finalize(s);

        int64_t wallet_bal = 0, wallet_cnt = 0;
        s = NULL;
        sqlite3_prepare_v2(ndb.db,
            "SELECT count(*), COALESCE(SUM(value),0) FROM wallet_utxos "
            "WHERE spent_txid IS NULL", -1, &s, NULL);
        if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
            wallet_cnt = sqlite3_column_int64(s, 0);
            wallet_bal = sqlite3_column_int64(s, 1);
        }
        sqlite3_finalize(s);

        int64_t addr_count = 0;
        s = NULL;
        sqlite3_prepare_v2(ndb.db,
            "SELECT count(*) FROM addresses WHERE balance > 0",
            -1, &s, NULL);
        if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW)
            addr_count = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);

        printf("\n=== Results ===\n");
        printf("UTXOs:     %lld (%.8f ZCL)\n",
               (long long)utxo_count, (double)utxo_sum / 1e8);
        printf("Wallet:    %lld UTXOs (%.8f ZCL)\n",
               (long long)wallet_cnt, (double)wallet_bal / 1e8);
        printf("Addresses: %lld with balance\n", (long long)addr_count);
        printf("Time:      %llds\n", (long long)(t1 - t0));

        /* ── SHA3 UTXO integrity verification ── */
        const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
        if (cp && cp->height > 0) {
            printf("\nVerifying UTXO set against SHA3 checkpoint "
                   "(h=%d, %llu expected UTXOs)...\n",
                   cp->height, (unsigned long long)cp->utxo_count);
            uint8_t sha3[32];
            uint64_t sha3_count = 0;
            utxo_commitment_sha3_compute(ndb.db, sha3, &sha3_count);
            if (memcmp(sha3, cp->sha3_hash, 32) == 0 &&
                sha3_count == cp->utxo_count) {
                printf("SHA3 UTXO verification PASSED (%llu UTXOs match)\n",
                       (unsigned long long)sha3_count);
            } else {
                fprintf(stderr,
                    "SHA3 UTXO MISMATCH — imported UTXO set is corrupt!\n"
                    "  Expected: %llu UTXOs, got %llu\n",
                    (unsigned long long)cp->utxo_count,
                    (unsigned long long)sha3_count);
                if (sha3_count != cp->utxo_count)
                    fprintf(stderr, "  Count delta: %lld\n",
                            (long long)sha3_count - (long long)cp->utxo_count);
                node_db_close(&ndb);
                return 1;
            }
        }

        node_db_close(&ndb);
        return 0;
    }

    /* Direct block-index (header chain) import — seeds the SQLite blocks
     * table from a LevelDB block index, header-only (clears HAVE_DATA + file
     * positions so bodies fetch lazily). Pair with --importchainstate (or the
     * boot UTXO auto-import) so an imported UTXO anchor immediately becomes the
     * tip instead of waiting on P2P header sync.
     * Usage: zclassic23 --importblockindex /path/to/datadir [dbpath]
     *   where /path/to/datadir is the PARENT of blocks/index (e.g. ~/.zclassic
     *   for a running zclassicd — the on-disk format is shared). */
    if (argc >= 3 && strcmp(argv[1], "--importblockindex") == 0) {
        const char *snap_dir = argv[2];
        const char *home = getenv("HOME");
        char db_path[512];
        if (argc > 3) {
            /* Positional target, NOT a flag: a '-datadir=...' here would be
             * silently treated as a literal db filename (and fed to shell
             * cp/rm commands). */
            if (argv[3][0] == '-') {
                fprintf(stderr,
                        "usage: zclassic23 --importblockindex <source-datadir>"
                        " [<target-node.db-path>]\n"
                        "       (got flag-like target '%s'; pass the node.db"
                        " PATH, e.g. ~/.zclassic-c23/node.db)\n", argv[3]);
                return 1;
            }
            snprintf(db_path, sizeof(db_path), "%s", argv[3]);
        } else
            snprintf(db_path, sizeof(db_path), "%s/.zclassic-c23/node.db",
                     home ? home : ".");

        printf("=== ZClassic Block-Index (header) Import ===\n");
        printf("Source: %s/blocks/index (LevelDB CDiskBlockIndex)\n", snap_dir);
        printf("Target: %s (SQLite blocks)\n", db_path);
        printf("Mode:   header-only (bodies fetched lazily via P2P)\n\n");

        /* If the source LevelDB is LOCKed (e.g. a running zclassicd owns
         * it), copy blocks/index to a temp dir and remove the copied LOCK —
         * NEVER touch another process's LOCK. Mirrors utxo_recovery_import_ldb.
         * Then import from the temp parent. */
        char import_parent[1024];
        char tmp_cleanup[1100] = "";
        snprintf(import_parent, sizeof(import_parent), "%s", snap_dir);
        {
            char src_lock[1100];
            snprintf(src_lock, sizeof(src_lock), "%s/blocks/index/LOCK", snap_dir);
            struct stat lst;
            if (stat(src_lock, &lst) == 0) {
                /* Derive a temp parent next to the target db. */
                char ddir[700];
                snprintf(ddir, sizeof(ddir), "%s", db_path);
                char *slash = strrchr(ddir, '/');
                if (slash) *slash = '\0'; else snprintf(ddir, sizeof(ddir), ".");
                char tmp_parent[900];
                snprintf(tmp_parent, sizeof(tmp_parent), "%s/bidx_import_tmp", ddir);
                printf("Copying block index (source LOCK present)...\n");
                fflush(stdout);
                /* `rm -rf tmp_parent && mkdir -p tmp_parent/blocks &&
                 *  cp -a snap_dir/blocks/index tmp_parent/blocks/index` — the
                 * fd-based walker, no shell. PRESERVE_TIMES matches cp -a. */
                char src_index[1100], dst_blocks[1100], dst_index[1200];
                snprintf(src_index, sizeof(src_index), "%s/blocks/index", snap_dir);
                snprintf(dst_blocks, sizeof(dst_blocks), "%s/blocks", tmp_parent);
                snprintf(dst_index, sizeof(dst_index), "%s/blocks/index", tmp_parent);
                struct zcl_result rrm = zcl_tree_remove(tmp_parent);
                struct zcl_result rmk = rrm.ok ? zcl_mkdir_p(dst_blocks, 0755)
                                               : rrm;
                struct zcl_result rcp = rmk.ok
                    ? zcl_tree_copy(src_index, dst_index,
                                    ZCL_COPY_PRESERVE_TIMES, NULL, NULL)
                    : rmk;
                if (!rcp.ok) {
                    fprintf(stderr, "Failed to copy block index: %s\n",
                            rcp.message);
                    return 1;
                }
                char tmp_lock[1100];
                snprintf(tmp_lock, sizeof(tmp_lock), "%s/blocks/index/LOCK", tmp_parent);
                unlink(tmp_lock);
                snprintf(import_parent, sizeof(import_parent), "%s", tmp_parent);
                snprintf(tmp_cleanup, sizeof(tmp_cleanup), "%s", tmp_parent);
            }
        }

        int64_t t0 = (int64_t)time(NULL);
        int count = 0;
        bool ok = snapshot_import_block_index(import_parent, db_path, true, &count);
        int64_t t1 = (int64_t)time(NULL);
        if (tmp_cleanup[0]) {
            /* `rm -rf tmp_cleanup` via the fd-based walker (no shell). */
            struct zcl_result rmc = zcl_tree_remove(tmp_cleanup);
            if (!rmc.ok)
                fprintf(stderr, "warning: failed to clean temp %s: %s\n",
                        tmp_cleanup, rmc.message);
        }
        if (!ok) {
            fprintf(stderr, "Block-index import failed\n");
            return 1;
        }
        printf("\nImported %d headers in %llds\n", count, (long long)(t1 - t0));
        return 0;
    }

    /* UTXO commitment MINT ceremony — compute the SHA3 commitment over the
     * current (operator-trusted, synced) UTXO set and emit a paste-ready
     * sha3_utxo_checkpoint for lib/chain/src/checkpoints.c. This is the
     * "fresh checkpoint" half of the trust model: a
     * release ceremony run on a node synced+verified from a trusted source,
     * so that future fast-imports can FATAL-verify their UTXO set against a
     * signed commitment near the tip instead of trusting the source blindly.
     * Read-only. Usage: zclassic23 --mintutxocommitment [dbpath] */
    if (argc >= 2 && strcmp(argv[1], "--mintutxocommitment") == 0) {
        const char *home = getenv("HOME");
        char db_path[512];
        db_path[0] = '\0';
        /* Fail-closed: the operator MUST declare the intended checkpoint
         * height, and the ceremony refuses unless the utxos projection sits
         * EXACTLY there. The original mint had no such assert and committed
         * an off-by-one projection (see checkpoints.c re-bake note). */
        int32_t want_height = -1;
        for (int i = 2; i < argc; i++) {
            if (strncmp(argv[i], "--height=", 9) == 0)
                want_height = atoi(argv[i] + 9);
            else if (argv[i][0] != '-' && db_path[0] == '\0')
                snprintf(db_path, sizeof(db_path), "%s", argv[i]);
        }
        if (db_path[0] == '\0')
            snprintf(db_path, sizeof(db_path), "%s/.zclassic-c23/node.db",
                     home ? home : ".");
        if (want_height < 0) {
            fprintf(stderr, "Refusing to mint: pass --height=N (the intended "
                    "checkpoint height) so the ceremony can assert the utxos "
                    "projection sits exactly there.\n");
            return 1;
        }

        struct node_db ndb;
        if (!node_db_open(&ndb, db_path)) {
            fprintf(stderr, "Cannot open SQLite: %s\n", db_path);
            return 1;
        }

        /* Height = the UTXO set's height (coins best block height). */
        int32_t height = 0;
        sqlite3_stmt *hs = NULL;
        sqlite3_prepare_v2(ndb.db, "SELECT MAX(height) FROM utxos", -1, &hs, NULL);
        if (hs && AR_STEP_ROW_READONLY(hs) == SQLITE_ROW)
            height = sqlite3_column_int(hs, 0);
        if (hs) sqlite3_finalize(hs);

        /* Block hash at that height (prefer a data-bearing canonical row). */
        uint8_t block_hash[32] = {0};
        sqlite3_stmt *bs = NULL;
        sqlite3_prepare_v2(ndb.db,
            "SELECT hash FROM blocks WHERE height = ? "
            "ORDER BY (status & 8) DESC LIMIT 1", -1, &bs, NULL);
        if (bs) {
            sqlite3_bind_int(bs, 1, height);
            if (AR_STEP_ROW_READONLY(bs) == SQLITE_ROW &&
                sqlite3_column_bytes(bs, 0) == 32)
                memcpy(block_hash, sqlite3_column_blob(bs, 0), 32);
            sqlite3_finalize(bs);
        }

        /* Total transparent supply (zatoshi). */
        int64_t total_supply = 0;
        sqlite3_stmt *ts = NULL;
        sqlite3_prepare_v2(ndb.db,
            "SELECT COALESCE(SUM(value),0) FROM utxos", -1, &ts, NULL);
        if (ts && AR_STEP_ROW_READONLY(ts) == SQLITE_ROW)
            total_supply = sqlite3_column_int64(ts, 0);
        if (ts) sqlite3_finalize(ts);

        /* SHA3 over the canonical UTXO set. */
        uint8_t sha3[32];
        uint64_t count = 0;
        utxo_commitment_sha3_compute(ndb.db, sha3, &count);
        node_db_close(&ndb);

        if (height <= 0 || count == 0) {
            fprintf(stderr, "Refusing to mint: empty/incomplete UTXO set "
                    "(height=%d count=%llu). Sync to tip from a trusted "
                    "source first.\n", height, (unsigned long long)count);
            return 1;
        }

        if (height != want_height) {
            fprintf(stderr, "Refusing to mint: utxos projection height %d != "
                    "intended checkpoint height %d — the projection is not at "
                    "the checkpoint height (this is the off-by-one class the "
                    "original corrupt mint hit). Settle the projection to "
                    "exactly h=%d first.\n", height, want_height, want_height);
            return 1;
        }

        char bh_hex[65], s3_hex[65];
        for (int i = 0; i < 32; i++) {
            snprintf(bh_hex + i * 2, 3, "%02x", block_hash[31 - i]); /* display = reversed */
            snprintf(s3_hex + i * 2, 3, "%02x", sha3[i]);
        }
        printf("=== ZClassic SHA3 UTXO Commitment (MINT ceremony) ===\n");
        printf("height=%d count=%llu supply=%lld zatoshi\n",
               height, (unsigned long long)count, (long long)total_supply);
        printf("block_hash=%s\nsha3=%s\n\n", bh_hex, s3_hex);
        printf("/* Paste into lib/chain/src/checkpoints.c as the latest\n"
               " * g_sha3_checkpoint, then commit (signed release). */\n");
        printf("static const struct sha3_utxo_checkpoint g_sha3_checkpoint = {\n");
        printf("    .height = %d,\n", height);
        printf("    .block_hash = { /* %s */\n        ", bh_hex);
        for (int i = 0; i < 32; i++)
            printf("0x%02x,%s", block_hash[i], (i % 8 == 7) ? "\n        " : " ");
        printf("\n    },\n");
        printf("    .sha3_hash = { /* %s */\n        ", s3_hex);
        for (int i = 0; i < 32; i++)
            printf("0x%02x,%s", sha3[i], (i % 8 == 7) ? "\n        " : " ");
        printf("\n    },\n");
        printf("    .utxo_count = %llu,\n", (unsigned long long)count);
        printf("    .total_supply = %lldLL,\n", (long long)total_supply);
        printf("};\n");
        return 0;
    }

    /* Default boot is the headless node (north star: AI-as-interface, no GUI).
     * The WebKit wallet GUI is opt-in via -gui; a plain `zclassic23`
     * and -datadir both run the node, never wallet_gui_main.
     *   build/bin/zclassic23          → headless node
     *   build/bin/zclassic23 -gui     → wallet GUI (needs a display)
     * --self-test is the GUI bot harness (runs the GUI under xvfb), so it
     * is itself an explicit GUI launch; --gui is kept as a back-compat
     * spelling of -gui. */
    {
        bool gui_mode = false;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-gui") == 0 ||
                strcmp(argv[i], "--gui") == 0 ||
                strcmp(argv[i], "--self-test") == 0)
                gui_mode = true;
        }
        if (gui_mode) {
            const char *h = getenv("HOME");
            char dd[512];
            snprintf(dd, sizeof(dd), "%s/.zclassic-c23", h ? h : ".");
            return wallet_gui_main(argc, argv, dd);
        }
    }

    /* Node mode */
    struct app_context ctx;
    app_context_defaults(&ctx);

    const char *home = getenv("HOME");
    char default_datadir[512];
    char default_paramsdir[512];
    if (home) {
        snprintf(default_datadir, sizeof(default_datadir), "%s/.zclassic-c23", home);
        snprintf(default_paramsdir, sizeof(default_paramsdir), "%s/.zcash-params", home);
    } else {
        snprintf(default_datadir, sizeof(default_datadir), ".zclassic-c23");
        snprintf(default_paramsdir, sizeof(default_paramsdir), ".zcash-params");
    }
    ctx.datadir = default_datadir;
    ctx.params_dir = default_paramsdir;
    const char *env_lane = getenv("ZCL_OPERATOR_LANE");
    if (env_lane && env_lane[0] &&
        !app_operator_lane_parse(env_lane, &ctx.operator_lane)) {
        fprintf(stderr, "Ignoring unknown ZCL_OPERATOR_LANE=%s\n", env_lane);
    }
    const char *env_nf_backfill = getenv("ZCL_NULLIFIER_BACKFILL");
    if (env_nf_backfill && strcmp(env_nf_backfill, "1") == 0)
        ctx.backfill_nullifiers = true;

    bool show_metrics = true;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-datadir=", 9) == 0) ctx.datadir = argv[i] + 9;
        else if (strncmp(argv[i], "-paramsdir=", 11) == 0) ctx.params_dir = argv[i] + 11;
        else if (strcmp(argv[i], "-testnet") == 0) ctx.testnet = true;
        else if (strcmp(argv[i], "-regtest") == 0) ctx.regtest = true;
        else if (strcmp(argv[i], "-txindex") == 0) ctx.tx_index = true;
        else if (strcmp(argv[i], "-gen") == 0) ctx.gen = true;
        else if (strncmp(argv[i], "-port=", 6) == 0) { ctx.p2p_port = atoi(argv[i]+6); ctx.listen = true; }
        else if (strncmp(argv[i], "-rpcport=", 9) == 0) ctx.rpc_port = atoi(argv[i]+9);
        else if (strncmp(argv[i], "-httpsport=", 11) == 0) ctx.https_port = atoi(argv[i]+11);
        else if (strncmp(argv[i], "-fsport=", 8) == 0) ctx.fs_port = atoi(argv[i]+8);
        else if (strncmp(argv[i], "-rpcuser=", 9) == 0) ctx.rpc_user = argv[i]+9;
        else if (strncmp(argv[i], "-rpcpassword=", 13) == 0) ctx.rpc_password = argv[i]+13;
        else if (strcmp(argv[i], "-listen") == 0) ctx.listen = true;
        else if (strncmp(argv[i], "-addnode=", 9) == 0) { /* after init */ }
        else if (strncmp(argv[i], "-connect=", 9) == 0) { ctx.connect_only = true; /* after init */ }
        else if (strncmp(argv[i], "-mineraddress=", 14) == 0) ctx.miner_address = argv[i]+14;
        else if (strncmp(argv[i], "-genproclimit=", 14) == 0) ctx.gen_threads = atoi(argv[i]+14);
        else if (strncmp(argv[i], "-par=", 5) == 0) ctx.par_workers = atoi(argv[i]+5);
        else if (strncmp(argv[i], "-snapshot=", 10) == 0) ctx.snapshot_dir = argv[i]+10;
        else if (strcmp(argv[i], "-saplingscan") == 0) ctx.sapling_scan = true;
        else if (strcmp(argv[i], "-reindex-chainstate") == 0) ctx.reindex_chainstate = true;
        else if (strcmp(argv[i], "-refold-staged") == 0) ctx.refold_staged = true;
        else if (strcmp(argv[i], "-refold-from-anchor") == 0) ctx.refold_from_anchor = true;
        else if (strcmp(argv[i], "-load-verify-boot") == 0) ctx.load_verify_boot = true;
        else if (strncmp(argv[i], "-load-snapshot-at-own-height=",
                         sizeof("-load-snapshot-at-own-height=") - 1) == 0)
            ctx.load_snapshot_at_own_height =
                argv[i] + sizeof("-load-snapshot-at-own-height=") - 1;
        else if (strncmp(argv[i], "-install-consensus-bundle=",
                         sizeof("-install-consensus-bundle=") - 1) == 0)
            ctx.install_consensus_bundle =
                argv[i] + sizeof("-install-consensus-bundle=") - 1;
        else if (strncmp(argv[i], "-verify-consensus-bundle=",
                         sizeof("-verify-consensus-bundle=") - 1) == 0)
            ctx.verify_consensus_bundle =
                argv[i] + sizeof("-verify-consensus-bundle=") - 1;
        else if (strcmp(argv[i], "-ratify-mint-anchor") == 0)
            ctx.ratify_mint_anchor = true;
        else if (strcmp(argv[i], "-export-consensus-bundle") == 0)
            ctx.export_consensus_bundle = true;
        else if (strncmp(argv[i], "-promote-shielded-history=",
                         sizeof("-promote-shielded-history=") - 1) == 0)
            ctx.promote_shielded_history =
                argv[i] + sizeof("-promote-shielded-history=") - 1;
        else if (strcmp(argv[i], "-fold-inram") == 0) {
            /* Bulk-fold in-RAM UTXO hot store (storage/coins_ram.h). The
             * storage layer reads ZCL_FOLD_INRAM as the single source of truth
             * (decided once, cached), so the flag just sets the env before any
             * coins_ram_* call. STRICTLY for the bulk fold (from-genesis mint /
             * -refold-from-anchor catch-up): the at-tip steady state (1 block /
             * 2.5 min) does NOT benefit and should run plain SQLite coins_kv. */
            setenv("ZCL_FOLD_INRAM", "1", 1);
        }
        else if (strcmp(argv[i], "-mint-anchor") == 0) ctx.mint_anchor = true;
        else if (strcmp(argv[i], "-mint-anchor-fast") == 0) ctx.mint_anchor_fast = true;
        else if (strcmp(argv[i], "-reindex-explorer") == 0) ctx.reindex_explorer = true;
        else if (strcmp(argv[i], "-backfill-zslp") == 0) ctx.backfill_zslp = true;
        else if (strcmp(argv[i], "-backfill-nullifiers") == 0) ctx.backfill_nullifiers = true;
        else if (strcmp(argv[i], "-reimport-utxos") == 0) ctx.reimport_utxos = true;
        else if (strcmp(argv[i], "-allow-degraded") == 0) ctx.allow_degraded = true;
        else if (strncmp(argv[i], "-showmetrics=", 13) == 0) show_metrics = atoi(argv[i]+13) != 0;
        else if (strcmp(argv[i], "-tor") == 0) ctx.tor = true;
        else if (strncmp(argv[i], "-profile=", 9) == 0) {
            if (!app_runtime_profile_parse(argv[i] + 9,
                                           &ctx.runtime_profile)) {
                fprintf(stderr, "Unknown runtime profile: %s\n", argv[i] + 9);
                return 1;
            }
        }
        else if (strncmp(argv[i], "-operator-lane=", 15) == 0) {
            if (!app_operator_lane_parse(argv[i] + 15,
                                         &ctx.operator_lane)) {
                fprintf(stderr, "Unknown operator lane: %s\n", argv[i] + 15);
                return 1;
            }
        }
        else if (strncmp(argv[i], "-assumevalid", 12) == 0) {
            fprintf(stderr,
                    "-assumevalid has been removed; use "
                    "-deferproofvalidationbelow=<blockhash|0>\n");
            return 1;
        }
        else if (strncmp(argv[i], "-deferproofvalidationbelow=",
                         sizeof("-deferproofvalidationbelow=") - 1) == 0) {
            ctx.defer_proof_validation_below =
                argv[i] + sizeof("-deferproofvalidationbelow=") - 1;
        }
        else if (strncmp(argv[i], "-filesync=", 10) == 0) { /* handled above */ }
        else if (strncmp(argv[i], "-fileservice=", 13) == 0) ctx.file_service_peer = argv[i]+13;
        else if (strcmp(argv[i], "-nofilesync") == 0) ctx.no_file_sync = true;
        else if (strcmp(argv[i], "-allow-clearnet-snapshot-fetch") == 0) ctx.allow_clearnet_snapshot_fetch = true;
        else if (strcmp(argv[i], "-enforce-sapling-root") == 0) {
            /* DEFAULT-OFF Sapling-root parity reject (project_sapling_root
             * _parity_hole). Default behavior rejects ONLY an all-zeros
             * hashFinalSaplingRoot; this flag additionally rejects ANY
             * mismatch vs the locally-recomputed Sapling tree root, matching
             * zclassicd. ⚠ Do NOT pass on the live node until a full-history
             * replay confirms ZERO false-rejects (h=478544 lesson — see
             * validation/connect_block.h). */
            extern _Atomic _Bool g_enforce_sapling_root;
            atomic_store(&g_enforce_sapling_root, true);
        }
        else if (strcmp(argv[i], "-enforce-coinbase-maturity") == 0) {
            /* DEFAULT-OFF coinbase-maturity parity reject on the live reducer
             * fold. Default behavior does NOT reject a spend of a coinbase
             * output younger than COINBASE_MATURITY (100) on that path; this
             * flag adds the reject, matching zclassicd CheckTxInputs
             * (zclassic-cpp/src/main.cpp:2056-2060). ⚠ This is a tightening
             * predicate — do NOT pass on the live node until a full-history
             * replay confirms ZERO false-rejects (h=478544 lesson — see
             * jobs/utxo_apply_delta.h). */
            extern _Atomic _Bool g_enforce_coinbase_maturity;
            atomic_store(&g_enforce_coinbase_maturity, true);
        }
        else if (strcmp(argv[i], "-enforce-checkdatasig-sigops") == 0) {
            /* DEFAULT-OFF CHECKDATASIG_SIGOPS parity. Default connect_block
             * flags are P2SH | CHECKLOCKTIMEVERIFY; this flag also ORs in
             * SCRIPT_VERIFY_CHECKDATASIG_SIGOPS, matching zclassicd
             * ConnectBlock (zclassic-cpp/src/main.cpp:2567), which counts
             * OP_CHECKDATASIG[VERIFY] toward the per-block sigop ceiling.
             * ⚠ Tightening predicate — do NOT pass on the live node until a
             * full-history replay confirms ZERO false-rejects (h=478544
             * lesson — see validation/connect_block.h). */
            extern _Atomic _Bool g_enforce_checkdatasig_sigops;
            atomic_store(&g_enforce_checkdatasig_sigops, true);
        }
        else if (strcmp(argv[i], "-nobgvalidation") == 0) ctx.no_bg_validation = true;
        else if (strcmp(argv[i], "-sandbox=steady") == 0) ctx.sandbox_steady = true;
        else if (strcmp(argv[i], "-sandbox=off") == 0) ctx.sandbox_steady = false;
        else if (strcmp(argv[i], "-hotswap-activate") == 0) {
            /* Arm Tier-1 live hot-swap ACTIVATION for this resident node. This
             * is only ONE of the two required gates: a live swap also needs
             * ZCL_HOTSWAP_ACTIVATE=1 in the environment AND the exact dev
             * datadir (~/.zclassic-c23-dev). The canonical datadir is refused
             * unconditionally. Without this flag every hot-swap is verify-only.
             * See hotswap_activation_authorized() (lib/hotswap). */
            hotswap_set_activate_flag(true);
        }
        else if (strcmp(argv[i], "-nolegacyimport") == 0) ctx.no_legacy_auto_import = true;
        else if (strcmp(argv[i], "-allow-plaintext-wallet") == 0) {
            /* Explicit, loud opt-in to a plaintext wallet at rest. Read
             * by the wallet at-rest creation policy at boot (see
             * lib/wallet/src/wallet_keystore.c). Without this flag AND
             * without ZCL_WALLET_PASSPHRASE, first-run wallet creation
             * refuses rather than silently minting unencrypted keys. */
            setenv("ZCL_ALLOW_PLAINTEXT_WALLET", "1", 1);
        }
        else if (strcmp(argv[i], "-rebuildfromlog") == 0) ctx.boot_from_log = true;
        else if (strcmp(argv[i], "-leveldb-no-verify-checksums") == 0) {
            /* Turns off LevelDB checksum verification for both point
             * reads and iteration.  Use only when chasing a suspected
             * corruption issue — silent truncation returns. */
            setenv("ZCL_LEVELDB_NO_VERIFY_CHECKSUMS", "1", 1);
        }
        else if (strncmp(argv[i], "-externalip=", 12) == 0) ctx.external_ip = argv[i] + 12;
        else if (strncmp(argv[i], "-httpsdomain=", 13) == 0) ctx.https_domain = argv[i] + 13;
        else if (strcmp(argv[i], "-gui") == 0 || strcmp(argv[i], "--gui") == 0) {
            /* Opt-in to the WebKit wallet GUI. Consumed earlier (the GUI
             * launch returns before node mode); recognized here so it is
             * an intentional flag, not silently dropped node-mode noise. */
        }
        else if (strcmp(argv[i], "-help") == 0 || strcmp(argv[i], "--help") == 0 ||
                 strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "-?") == 0) {
            print_usage(argv[0]); return 0;
        }
        else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-version") == 0 ||
                 strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "-V") == 0) {
            /* Print version + exit. Without this, `zclassic23 --version` (a
             * judge's reflex) falls through as an unknown flag and silently
             * boots a full node against the default datadir. */
            printf("zclassic23 v%d.%d.%d (source %.12s)\n",
                   CLIENT_VERSION_MAJOR, CLIENT_VERSION_MINOR,
                   CLIENT_VERSION_REVISION,
                   zcl_build_source_id_sha256());
            return 0;
        }
    }

    /* -mint-anchor (both profiles) defaults onto the in-RAM UTXO overlay: the
     * offline mint drives all eight stages on ONE thread and brackets the drive
     * with coins_ram_mint_drive_enter/exit, so script_validate resolves recent-
     * coin prevouts from the un-flushed overlay via coins_kv_overlay_safe(). The
     * env MUST be set here, before app_init caches coins_ram_enabled() (first
     * read in utxo_apply_stage_init). Opt out with ZCL_FOLD_INRAM=0; the
     * terminal SHA3/count hard-assert is identical on either path. Inert on a
     * live node — the mint-drive marker is entered only by the offline driver. */
    if (ctx.mint_anchor && getenv("ZCL_FOLD_INRAM") == NULL)
        setenv("ZCL_FOLD_INRAM", "1", 1);

    /* OFFLINE-ONLY GUARD (jobs/mint_skip_crypto.h): -mint-anchor-fast (the
     * crypto pass-through) is HONORED ONLY together with -mint-anchor (the
     * one-shot offline mint that never starts P2P/RPC and _exit()s). Refuse the
     * flag standalone so the skip-crypto toggle can never be armed on a path
     * that becomes a running node. This is the FIRST of the four composed
     * guards; the setter call is also nested under ctx->mint_anchor at the
     * boot.c reset site and lint-fenced to the mint driver TUs. */
    if (ctx.mint_anchor_fast && !ctx.mint_anchor) {
        fprintf(stderr,
                "FATAL: -mint-anchor-fast is the OFFLINE FAST-MINT crypto "
                "pass-through and is honored ONLY together with -mint-anchor. "
                "It is never a running-node signature bypass. Re-run with both "
                "-mint-anchor -mint-anchor-fast, or drop -mint-anchor-fast.\n");
        return 1;
    }

    /* Fast file sync: download block files via SHA3 encrypted service
     * BEFORE starting the full node. Wire speed, not block-by-block. */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-filesync=", 10) == 0) {
            const char *host = argv[i] + 10;
            printf("=== SHA3 File Sync from %s:%d ===\n", host, FS_PORT);
            uint8_t utxo_root[32];
            memset(utxo_root, 0, 32);
            char blocks_dir[512];
            snprintf(blocks_dir, sizeof(blocks_dir), "%s/blocks", ctx.datadir);
            mkdir(blocks_dir, 0755);
            int64_t t0 = (int64_t)time(NULL);
            bool ok = fs_client_sync(host, FS_PORT, ctx.datadir, utxo_root);
            int64_t elapsed = (int64_t)time(NULL) - t0;
            if (elapsed < 1) elapsed = 1;
            if (ok) {
                printf("=== File sync complete: %lld seconds ===\n",
                       (long long)elapsed);
            } else {
                fprintf(stderr, "File sync failed from %s\n", host);
            }
            break;
        }
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    /* Install SIGINT/SIGTERM via sigaction, NOT signal(). Under this build's
     * feature-test macros (_POSIX_C_SOURCE without _DEFAULT_SOURCE) glibc's
     * signal() gives System V ONE-SHOT semantics: the disposition resets to
     * SIG_DFL the instant the handler fires. signal_handler() never re-armed
     * it, so the FIRST SIGTERM ran the handler AND reverted SIGTERM to default —
     * and the SECOND SIGTERM (systemd ExecStop sends pulses 2 s apart) then
     * killed the process with default disposition, silently, mid-shutdown,
     * before the WAL checkpoint + clean-shutdown marker. sigaction without
     * SA_RESETHAND keeps the handler installed so repeated SIGTERMs are
     * absorbed idempotently (the handler's own g_shutdown_requested guard). */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART; /* persistent; restart interrupted syscalls */
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }

    /* -connect mode: only connect to specified peers, no seeds */
    if (ctx.connect_only) {
        extern bool g_connect_only;
        g_connect_only = true;
    }

    printf("zclassic23 starting (datadir=%s)...\n", ctx.datadir);
    rpc_agent_set_boot_context(app_operator_lane_name(ctx.operator_lane),
                               app_runtime_profile_name(ctx.runtime_profile),
                               ctx.datadir, ctx.rpc_port, ctx.p2p_port,
                               ctx.https_port, ctx.fs_port);

    /* #8 — capture argv for a possible in-process self-respawn (the watchdog
     * sets the respawn flag for a genuine-liveness stall when off-systemd). */
    g_saved_argv = argv;

    /* ONE preflight naming ALL unmet -mint-anchor producer preconditions
     * upfront (config/src/boot_mint_anchor_preflight.c), BEFORE app_init
     * opens/mutates node.db, progress.kv, or wallet.dat. Replaces the
     * historical one-FATAL-at-a-time surfacing (missing legacy block index ->
     * FATAL on one run; missing bodies -> silent stall on the next). Every
     * check is read-only; a failure here means app_init never runs. */
    if (ctx.mint_anchor && !boot_mint_anchor_preflight_run_all(ctx.datadir, NULL))
        return 1;

    /* -sandbox=steady fail-closed: the node deny-set forbids execve, so the
     * off-systemd self-respawn (the S7 re-exec below) would be KILLED. Under
     * systemd, Restart=always owns respawn and no self-exec happens — so the
     * sandbox is only honored there. Refuse rather than silently disarming the
     * liveness-recovery path. */
    if (ctx.sandbox_steady && !sd_notify_is_active()) {
        fprintf(stderr,
            "FATAL: -sandbox=steady requires a systemd NOTIFY_SOCKET "
            "(the sandbox forbids execve, which the off-systemd self-respawn "
            "needs). Run under systemd, or drop -sandbox=steady.\n");
        return 1;
    }

    if (!app_init(&ctx)) {
        fprintf(stderr, "Initialization failed.\n");
        return 1;
    }

    /* -backfill-zslp is a one-shot: app_init re-derived the zslp_* tables and
     * returned before any service started. Exit now — running the peer-wiring
     * below would call app_add_node() against a NULL connman. The backfill
     * committed through SQLite WAL, so the data is durable without a shutdown. */
    if (ctx.backfill_zslp)
        return 0;
    if (ctx.backfill_nullifiers)
        return 0;

    /* -mint-anchor is a one-shot ceremony: app_init reset the staged reducer to
     * genesis and capped the fold at the SHA3 checkpoint anchor. Drive the fold
     * to the anchor, write + HARD-ASSERT the snapshot artifact, then exit. The
     * driver _exit()s FATAL on a checkpoint mismatch; a clean return means a
     * verified mint (true) or an incomplete fold (false → exit non-zero so the
     * operator knows the bodies were missing). app_init returns before
     * app_init_services on this path, so frontend/P2P/runtime services never
     * start while -mint-anchor-fast can be armed. */
    if (ctx.mint_anchor) {
        bool minted = boot_mint_anchor_run(ctx.datadir);
        app_shutdown_offline();
        return minted ? 0 : 1;
    }

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-addnode=", 9) == 0)
            app_add_node(argv[i] + 9, 0);
        else if (strncmp(argv[i], "-connect=", 9) == 0)
            app_add_node(argv[i] + 9, 0);
        else if (strncmp(argv[i], "-addnode-file=", 14) == 0)
            app_add_nodes_from_file(argv[i] + 14);
    }

    /* Auto-addnode the co-located zclassicd peer (Option F from the
     * fast-sync plan). Local loopback bypasses external network
     * latency entirely; zclassicd is a fully-validated reference node
     * sharing the same chain. Connecting to it gives us tip-tracking
     * resilience even if all internet peers go away.
     *
     * Conservative — only auto-add when:
     *   - $HOME/.zclassic/zclassic.conf is present (zclassicd is set up)
     *   - we haven't been told -connect=… (which means "ONLY these peers")
     *   - no explicit -addnode=127.0.0.1:8034 already on the command line
     *
     * Reads the P2P port out of zclassic.conf (default 8034). */
    if (!ctx.connect_only) {
        const char *home = getenv("HOME");
        if (home && *home) {
            char conf_path[1024];
            snprintf(conf_path, sizeof(conf_path),
                     "%s/.zclassic/zclassic.conf", home);
            FILE *cf = fopen(conf_path, "r");
            if (cf) {
                int p2p_port = 8034;
                char line[256];
                while (fgets(line, sizeof(line), cf)) {
                    int v;
                    if (sscanf(line, " port = %d", &v) == 1 ||
                        sscanf(line, "port=%d", &v) == 1) {
                        if (v > 0 && v < 65536) p2p_port = v;
                    }
                }
                fclose(cf);
                bool already_listed = false;
                char hostport[64];
                snprintf(hostport, sizeof(hostport),
                         "127.0.0.1:%d", p2p_port);
                for (int i = 1; i < argc; i++) {
                    if (strstr(argv[i], hostport) != NULL) {
                        already_listed = true;
                        break;
                    }
                }
                if (!already_listed) {
                    printf("auto-addnode: local zclassicd at %s "
                           "(zclassic.conf detected)\n", hostport);
                    app_add_node("127.0.0.1", p2p_port);
                }
            }
        }
    }

    if (show_metrics) app_start_metrics(ctx.gen);

    while (!g_shutdown_requested &&
           !thread_registry_shutdown_requested() &&
           app_is_running())
        sleep(1);
    if (thread_registry_shutdown_requested())
        g_shutdown_requested = 1;

    if (show_metrics) app_stop_metrics();

    /* #8 — S7: if the tip-watchdog requested a self-respawn (genuine-liveness
     * stall with NO systemd notify socket), shut down cleanly and then re-exec
     * this binary in-process so liveness recovery does not depend on
     * Restart=always. Under systemd the flag is never set (sd_notify_is_active
     * was true), so this is a no-op there and the normal exit happens. The
     * bounded restart budget persisted in progress.kv is reloaded by the fresh
     * boot, so self-respawn is bounded exactly like a systemd restart and
     * cannot loop unbounded.
     *
     * Pillar 7 — "supervise the supervisor": the independent backstop watcher
     * (lib/util/src/supervisor_backstop.c) latches the SAME kind of flag when
     * the root supervisor's sweep heartbeat freezes off-systemd (its on-
     * systemd path instead just stops feeding boot_sd_watchdog's WATCHDOG=1
     * ping, so systemd's own Restart=always recovers the unit — no re-exec
     * needed there). Check both flags here so a frozen supervisor gets the
     * exact same off-systemd recovery as a frozen chain tip. */
    bool do_respawn = (chain_tip_watchdog_respawn_requested() ||
                       supervisor_backstop_respawn_requested()) &&
                      !sd_notify_is_active() && g_saved_argv;
    app_shutdown();
    if (do_respawn) {
        char exe[4096];
        if (os_proc_exe_path(exe, sizeof(exe))) {
            fprintf(stderr,
                "[main] self-respawn: re-exec %s (off-systemd liveness "
                "recovery; bounded by the persisted restart budget)\n", exe);
            fflush(NULL);
            execv(exe, g_saved_argv);
            /* execv only returns on error — fall through to a normal exit so a
             * failed re-exec is at worst a one-time DOWN, never a busy loop. */
            fprintf(stderr, "[main] self-respawn execv failed: %s — exiting "
                "(not looping)\n", strerror(errno));
        }
    }
    return 0;
}
