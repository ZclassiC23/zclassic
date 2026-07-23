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
 *   zclassic23 statecatalog           — diagnostics subsystem catalog
 *   zclassic23 agentlanes             — canonical/soak/dev lane topology
 *   zclassic23 agentliveness          — unified liveness rollup
 *   zclassic23 agentinterface         — preferred AI/operator interface
 *   zclassic23 milestone              — ASCII milestone status from node
 *   zclassic23 refold                 — UTXO anchor rebuild readiness
 *   zclassic23 <method> [params...]   — RPC client to running node */

#include "config/boot.h"
#include "config/boot_cold_start.h"     /* -cold-start staged driver */
#include "config/args.h"                /* flag ladder, -loglevel, usage text */
#include "main_cli_modes.h"             /* bench/cli/import/gen run-and-exit modes */
#include "net/file_service.h"           /* -filesync fast path (fs_client_sync) */
#include "controllers/agent_controller.h" /* rpc_agent_set_boot_context */
#include "views/wallet_gui.h"           /* -gui launch */
#include "config/boot_self_respawn.h"   /* #8/Pillar 7: off-systemd self-respawn */
#include "util/thread_registry.h"
#include "util/util.h"                  /* ParseParameters */
#include "util/sd_notify.h"             /* -sandbox=steady NOTIFY_SOCKET check */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

/* ════════════════════════════════════════════════════════════════
 *  NODE MODE — full node daemon
 * ════════════════════════════════════════════════════════════════ */

volatile sig_atomic_t g_shutdown_requested = 0;

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

int main(int argc, char **argv)
{
    ParseParameters(argc, (const char *const *)argv);
    apply_argv_loglevel();

    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "-bench", 6) == 0)
            return bench_mode_main(argc, argv);
    }

    /* --importblockindex: scanned ANYWHERE in argv, not just argv[1]. The
     * historical dispatch only matched a literal argv[1] strcmp, so any
     * other ordering (e.g. `-datadir=X --importblockindex Y`) fell through
     * every check below it and silently ran a normal node boot instead —
     * an operator ran a multi-hour band-path boot believing headers were
     * importing. This scan takes priority over every other CLI/boot mode
     * below: --importblockindex never boots the node in the same process,
     * so there is nothing it could conflict with. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--importblockindex") == 0)
            return importblockindex_cli_mode(argc, argv, i);
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

    /* -cold-start: one-command, staged, resumable driver that takes a fresh
     * datadir to a serving node (header import -> snapshot seed -> optional
     * consensus-bundle install -> serve) without operator choreography. It
     * COMPOSES the existing verbs as child processes with durable per-stage
     * receipts, then exec()s the plain serving boot. Scanned across argv (it
     * follows -datadir=), not positional. See config/boot_cold_start.h. */
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "-cold-start") == 0)
            return boot_cold_start_run(argc, argv);

    /* Wallet backup restore mode — decrypt an encrypted wallet backup.
     * Usage: zclassic23 --decrypt-wallet-backup <src.enc> <dst.sqlite>
     * Password comes from the WALLET_BACKUP_PASSWORD environment
     * variable (the same variable the node encrypts with). This is the
     * disaster-recovery path: without it, encrypted backups would be
     * unusable in the exact key-loss scenario they exist for. */
    if (argc >= 2 && strcmp(argv[1], "--decrypt-wallet-backup") == 0)
        return wallet_backup_decrypt_mode(argc, argv);

    /* UTXO repair mode — fetch missing UTXOs from zclassicd, no full node.
     * Usage: zclassic23 --repair [num_blocks] [port] [creds]
     * Scans blocks ahead of current tip via zclassicd RPC, inserts missing
     * UTXOs into SQLite with correct byte order. Restart node after. */
    if (argc >= 2 && strcmp(argv[1], "--repair") == 0)
        return repair_utxos_mode(argc, argv);

    /* Direct chainstate import mode — no full node startup needed.
     * Usage: zclassic23 --importchainstate /path/to/chainstate [dbpath] */
    if (argc >= 3 && strcmp(argv[1], "--importchainstate") == 0)
        return importchainstate_mode(argc, argv);

    /* UTXO commitment MINT ceremony — compute the SHA3 commitment over the
     * current (operator-trusted, synced) UTXO set and emit a paste-ready
     * sha3_utxo_checkpoint for lib/chain/src/checkpoints.c. This is the
     * "fresh checkpoint" half of the trust model: a
     * release ceremony run on a node synced+verified from a trusted source,
     * so that future fast-imports can FATAL-verify their UTXO set against a
     * signed commitment near the tip instead of trusting the source blindly.
     * Read-only. Usage: zclassic23 --mintutxocommitment [dbpath] */
    if (argc >= 2 && strcmp(argv[1], "--mintutxocommitment") == 0)
        return mintutxocommitment_mode(argc, argv);

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

    /* Node-mode flag ladder lives in config/src/args.c. It fills ctx +
     * show_metrics and returns -1 to continue, or an exit code to return
     * (--help/--version -> 0, a bad -profile=/-operator-lane= -> 1). */
    int argrc = args_parse_node_options(argc, argv, &ctx, &show_metrics);
    if (argrc >= 0)
        return argrc;

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
     * sets the respawn flag for a genuine-liveness stall when off-systemd).
     * The decision + re-exec live in config/src/boot_self_respawn.c so every
     * shutdown exit point (here AND the straggler-guard _exit in
     * boot_services_shutdown.c) honors an armed request identically. */
    boot_self_respawn_set_argv(argv);

    /* ONE preflight naming ALL unmet -mint-anchor producer preconditions
     * upfront (config/src/boot_mint_anchor_preflight.c), BEFORE app_init
     * opens/mutates node.db, progress.kv, or wallet.dat. Replaces the
     * historical one-FATAL-at-a-time surfacing (missing legacy block index ->
     * FATAL on one run; missing bodies -> silent stall on the next). Every
     * check is read-only; a failure here means app_init never runs. */
    /* -full-fold reuses ctx.mint_anchor but folds toward the local header tip,
     * not the compiled checkpoint — the mint preflight's checkpoint/anchor-bound
     * body-coverage checks would misfire. boot_full_fold_reset does its own
     * header-tip presence check and the fold walls loud on a missing body. */
    if (ctx.mint_anchor && !ctx.full_fold &&
        !boot_mint_anchor_preflight_run_all(ctx.datadir, NULL))
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

    /* -confine and -sandbox=steady are two distinct confinement mechanisms (a
     * strict seccomp ALLOW-list vs the steady-state deny-list) applied at the
     * same boundary. Refuse both at once rather than silently letting one win. */
    if (ctx.confine && ctx.sandbox_steady) {
        fprintf(stderr,
            "FATAL: -confine and -sandbox=steady are mutually exclusive "
            "(distinct seccomp confinements applied at the same boundary). "
            "Pick one.\n");
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

    /* -coldstart-seed-oneshot: app_init applied the snapshot seed and returned
     * before services (config/src/boot.c). Cleanly WAL-checkpoint + write the
     * clean-shutdown marker and exit so the cold-start driver's next stage
     * boots warm on a durable, clean-stopped datadir. */
    if (ctx.cold_start_seed_oneshot) {
        app_shutdown_offline();
        return 0;
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

    /* #8 — S7 / Pillar 7: if the chain-tip watchdog OR the supervisor backstop
     * requested a self-respawn (genuine-liveness stall / frozen root sweep with
     * NO systemd notify socket), shut down cleanly and then re-exec this binary
     * in-process so liveness recovery does not depend on Restart=always. Under
     * systemd the flags are never honored (sd_notify_is_active()==true), so
     * boot_self_respawn_exec_or_return() is a no-op here and the normal exit
     * happens. The bounded restart budget persisted in progress.kv is reloaded
     * by the fresh boot, so self-respawn is bounded exactly like a systemd
     * restart and cannot loop unbounded.
     *
     * The decision + execv are centralized in config/src/boot_self_respawn.c
     * so the straggler-guard _exit path in boot_services_shutdown.c (a
     * background worker missed its join window; the destructive frees are
     * skipped and the process exits early) honors an armed respawn the SAME
     * way — an early exit there used to silently drop the request off-systemd,
     * leaving the node DOWN. */
    app_shutdown();
    boot_self_respawn_exec_or_return();
    return 0;
}
