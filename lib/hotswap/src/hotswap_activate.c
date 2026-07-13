/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tier-1 hot-swap — the REAL (activatable) single-handler module loader.
 *
 * See hotswap/hotswap_module.h for the ABI. The pure surface (swappable
 * allowlist, activation flag, the activation GATE, and telemetry) compiles in
 * every build; only the dlopen/dlsym/dlclose activation core is DEV-ONLY. A
 * release build links the refusal stub at the bottom.
 *
 * Safety of the dlclose-after-drain reclamation: the superseded module .so is
 * closed ONLY after the resident quiesced_cb confirms every retired command
 * registry override snapshot has drained (no in-flight dispatch can still enter
 * the old handler). If drain cannot be confirmed within a bounded window the
 * old .so is KEPT mapped forever — the pilot's never-close behavior, always
 * memory-safe. dlclose is thus best-effort reclamation, never a correctness
 * dependency.
 */

#define _GNU_SOURCE
#include "hotswap/hotswap_module.h"
#include "hotswap/hotswap.h"

#include "json/json.h"
#include "platform/time_compat.h"
#include "util/log_macros.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── swappable allowlist, compiled from config/hotswap_swappable.def ─────── */
static const struct {
    const char *handler;
    const char *source;
} g_swappable[] = {
#define HOTSWAP_SWAPPABLE(handler_, source_) { .handler = handler_, .source = source_ },
#include "../../../config/hotswap_swappable.def"
#undef HOTSWAP_SWAPPABLE
};

bool hotswap_handler_is_swappable(const char *handler_name)
{
    if (!handler_name || !handler_name[0])
        return false;
    for (size_t i = 0; i < sizeof(g_swappable) / sizeof(g_swappable[0]); i++) {
        if (strcmp(handler_name, g_swappable[i].handler) == 0)
            return true;
    }
    return false;
}

static void act_copy(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0)
        return;
    snprintf(dst, cap, "%s", src ? src : "");
}

bool hotswap_module_admit(const struct zcl_hotswap_module *module,
                          char *stage, size_t stage_cap,
                          char *why, size_t why_cap)
{
    if (stage && stage_cap) stage[0] = '\0';
    if (why && why_cap) why[0] = '\0';
    if (!module) {
        act_copy(stage, stage_cap, "abi");
        act_copy(why, why_cap, "null module");
        return false;
    }
    if (module->abi_version != ZCL_HOTSWAP_MODULE_ABI_V1) {
        act_copy(stage, stage_cap, "abi");
        if (why && why_cap)
            snprintf(why, why_cap, "module abi_version %u != required %u",
                     module->abi_version, ZCL_HOTSWAP_MODULE_ABI_V1);
        return false;
    }
    if (!module->handler_name || !module->handler_name[0] || !module->fn ||
        !module->self_test) {
        act_copy(stage, stage_cap, "fields");
        act_copy(why, why_cap,
                 "module fields incomplete (handler_name/fn/self_test)");
        return false;
    }
    if (!hotswap_handler_is_swappable(module->handler_name)) {
        act_copy(stage, stage_cap, "allowlist");
        if (why && why_cap)
            snprintf(why, why_cap,
                     "handler '%s' is not on the swappable shape-leaf allowlist",
                     module->handler_name);
        return false;
    }
    char st_err[192] = {0};
    if (!module->self_test(st_err, sizeof(st_err))) {
        act_copy(stage, stage_cap, "self_test");
        act_copy(why, why_cap,
                 st_err[0] ? st_err : "module self_test returned false");
        return false;
    }
    return true;
}

/* ── activation flag + gate (pure; compiled in every build) ─────────────── */
static _Atomic bool g_activate_flag = false;

void hotswap_set_activate_flag(bool enabled)
{
    atomic_store_explicit(&g_activate_flag, enabled, memory_order_release);
}

bool hotswap_activate_flag(void)
{
    return atomic_load_explicit(&g_activate_flag, memory_order_acquire);
}

static bool env_opt_in(void)
{
    const char *v = getenv("ZCL_HOTSWAP_ACTIVATE");
    return v && strcmp(v, "1") == 0;
}

static bool dir_equals(const char *a, const char *b)
{
    if (!a || !a[0] || !b || !b[0])
        return false;
    char ra[PATH_MAX], rb[PATH_MAX];
    const char *pa = realpath(a, ra) ? ra : a;
    const char *pb = realpath(b, rb) ? rb : b;
    size_t la = strlen(pa), lb = strlen(pb);
    if (la && pa[la - 1] == '/') la--;
    if (lb && pb[lb - 1] == '/') lb--;
    return la == lb && strncmp(pa, pb, la) == 0;
}

static bool datadir_is_canonical(const char *datadir)
{
    if (!datadir || !datadir[0])
        return false;
    const char *home = getenv("HOME");
    if (!home || home[0] != '/')
        return false;
    char canonical[PATH_MAX];
    snprintf(canonical, sizeof(canonical), "%s/.zclassic-c23", home);
    return dir_equals(datadir, canonical);
}

bool hotswap_activation_authorized(const char *resolved_datadir,
                                   char *why, size_t why_sz)
{
    if (why && why_sz)
        why[0] = '\0';
    if (!hotswap_activate_flag()) {
        if (why) snprintf(why, why_sz,
            "activation refused: -hotswap-activate flag is not set");
        return false;
    }
    if (!env_opt_in()) {
        if (why) snprintf(why, why_sz,
            "activation refused: ZCL_HOTSWAP_ACTIVATE=1 is not set");
        return false;
    }
    if (datadir_is_canonical(resolved_datadir)) {
        if (why) snprintf(why, why_sz,
            "activation refused on the canonical datadir ~/.zclassic-c23 "
            "(canonical activation stays behind the owner's Phase-3 ritual)");
        return false;
    }
    if (!hotswap_datadir_is_dev(resolved_datadir)) {
        if (why) snprintf(why, why_sz,
            "activation requires the exact dev datadir ~/.zclassic-c23-dev, got '%s'",
            resolved_datadir ? resolved_datadir : "");
        return false;
    }
    return true;
}

/* ── activation telemetry state (written only on the dev activate path) ──── */
#define HOTSWAP_ACT_MAX_SLOTS 32

struct hotswap_act_slot {
    char handler[128];
    void *handle;            /* currently-live module .so for this handler */
    int artifact_fd;
    char artifact_sha256[65];
    uint32_t generation;
    time_t activated_at;
    uint64_t swaps;
    bool in_use;
};

struct hotswap_act_event {
    bool present;
    time_t at;
    char handler[128];
    char stage[64];
    char error[256];
    bool activated;
    bool ok;
};

static pthread_mutex_t g_act_lock = PTHREAD_MUTEX_INITIALIZER;
static struct hotswap_act_slot g_slots[HOTSWAP_ACT_MAX_SLOTS];
static size_t g_slot_count;
static struct hotswap_act_event g_last_activation;
static struct hotswap_act_event g_last_rejection;
static _Atomic uint64_t g_activation_count;
static _Atomic uint64_t g_rollback_count;
static _Atomic uint64_t g_verify_count;
static _Atomic uint64_t g_dlclose_count;
static _Atomic uint64_t g_retained_mapped_count;

uint64_t hotswap_activation_count(void)
{
    return atomic_load_explicit(&g_activation_count, memory_order_acquire);
}

static void event_json(struct json_value *obj, const struct hotswap_act_event *ev)
{
    json_set_object(obj);
    json_push_kv_bool(obj, "present", ev->present);
    if (!ev->present)
        return;
    json_push_kv_int(obj, "at", (int64_t)ev->at);
    json_push_kv_str(obj, "handler", ev->handler);
    json_push_kv_str(obj, "stage", ev->stage);
    if (ev->error[0])
        json_push_kv_str(obj, "error", ev->error);
    json_push_kv_bool(obj, "activated", ev->activated);
    json_push_kv_bool(obj, "ok", ev->ok);
}

void hotswap_activate_dump_json(struct json_value *out)
{
    if (!out)
        return;
    struct json_value act = {0};
    json_set_object(&act);
    json_push_kv_str(&act, "abi", "zcl.hotswap_module.v1");
    json_push_kv_int(&act, "abi_version", (int64_t)ZCL_HOTSWAP_MODULE_ABI_V1);
#ifdef ZCL_DEV_BUILD
    json_push_kv_bool(&act, "available", true);
#else
    json_push_kv_bool(&act, "available", false);
    json_push_kv_str(&act, "note", "activation unavailable in release build");
#endif
    bool flag = hotswap_activate_flag();
    bool env = env_opt_in();
    json_push_kv_bool(&act, "activate_flag", flag);
    json_push_kv_bool(&act, "env_opt_in", env);
    /* Containment state: only "armed" once BOTH gates are on; the datadir/
     * canonical check is still applied per-activation. */
    json_push_kv_str(&act, "containment",
                     (flag && env) ? "armed_dev_lane_only" : "verify_only");
    json_push_kv_int(&act, "activation_count",
                     (int64_t)atomic_load(&g_activation_count));
    json_push_kv_int(&act, "verify_only_count",
                     (int64_t)atomic_load(&g_verify_count));
    json_push_kv_int(&act, "rollback_count",
                     (int64_t)atomic_load(&g_rollback_count));
    json_push_kv_int(&act, "dlclose_count",
                     (int64_t)atomic_load(&g_dlclose_count));
    json_push_kv_int(&act, "retained_mapped_count",
                     (int64_t)atomic_load(&g_retained_mapped_count));

    struct json_value allow = {0};
    json_set_array(&allow);
    for (size_t i = 0; i < sizeof(g_swappable) / sizeof(g_swappable[0]); i++) {
        struct json_value s = {0};
        json_set_str(&s, g_swappable[i].handler);
        json_push_back(&allow, &s);
        json_free(&s);
    }
    json_push_kv(&act, "swappable_allowlist", &allow);
    json_free(&allow);

    pthread_mutex_lock(&g_act_lock);
    struct json_value slots = {0};
    json_set_array(&slots);
    for (size_t i = 0; i < g_slot_count; i++) {
        if (!g_slots[i].in_use)
            continue;
        struct json_value s = {0};
        json_set_object(&s);
        json_push_kv_str(&s, "handler", g_slots[i].handler);
        json_push_kv_int(&s, "generation", (int64_t)g_slots[i].generation);
        json_push_kv_str(&s, "artifact_sha256", g_slots[i].artifact_sha256);
        json_push_kv_int(&s, "activated_at", (int64_t)g_slots[i].activated_at);
        json_push_kv_int(&s, "swaps", (int64_t)g_slots[i].swaps);
        json_push_back(&slots, &s);
        json_free(&s);
    }
    json_push_kv(&act, "active_slots", &slots);
    json_free(&slots);

    struct json_value last_a = {0}, last_r = {0};
    event_json(&last_a, &g_last_activation);
    event_json(&last_r, &g_last_rejection);
    pthread_mutex_unlock(&g_act_lock);
    json_push_kv(&act, "last_activation", &last_a);
    json_push_kv(&act, "last_rejection", &last_r);
    json_free(&last_a);
    json_free(&last_r);

    json_push_kv(out, "activation", &act);
    json_free(&act);
}

#ifdef ZCL_DEV_BUILD

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>

#include "crypto/sha256.h"

static void record_event(struct hotswap_act_event *ev, const char *handler,
                         const char *stage, const char *error,
                         bool activated, bool ok)
{
    pthread_mutex_lock(&g_act_lock);
    memset(ev, 0, sizeof(*ev));
    ev->present = true;
    ev->at = platform_time_wall_time_t();
    act_copy(ev->handler, sizeof(ev->handler), handler);
    act_copy(ev->stage, sizeof(ev->stage), stage);
    act_copy(ev->error, sizeof(ev->error), error);
    ev->activated = activated;
    ev->ok = ok;
    pthread_mutex_unlock(&g_act_lock);
}

static bool artifact_sha256_fd(int fd, char hex_out[65])
{
    if (fd < 0 || !hex_out || lseek(fd, 0, SEEK_SET) < 0)
        return false;
    hex_out[0] = '\0';
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    unsigned char buf[64 * 1024];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) { sha256_write(&ctx, buf, (size_t)n); continue; }
        if (n == 0) break;
        if (errno == EINTR) continue;
        return false;
    }
    if (lseek(fd, 0, SEEK_SET) < 0)
        return false;
    unsigned char digest[SHA256_OUTPUT_SIZE];
    sha256_finalize(&ctx, digest);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < SHA256_OUTPUT_SIZE; i++) {
        hex_out[i * 2] = hex[digest[i] >> 4];
        hex_out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    hex_out[64] = '\0';
    return true;
}

/* Find (or, if activating a not-yet-seen handler, add) the per-handler slot.
 * ASSUMES g_act_lock held. Returns NULL only when the fixed table is full. */
static struct hotswap_act_slot *slot_for_handler_locked(const char *handler)
{
    for (size_t i = 0; i < g_slot_count; i++) {
        if (g_slots[i].in_use && strcmp(g_slots[i].handler, handler) == 0)
            return &g_slots[i];
    }
    if (g_slot_count >= HOTSWAP_ACT_MAX_SLOTS)
        return NULL;
    struct hotswap_act_slot *slot = &g_slots[g_slot_count++];
    memset(slot, 0, sizeof(*slot));
    slot->artifact_fd = -1;
    act_copy(slot->handler, sizeof(slot->handler), handler);
    slot->in_use = true;
    return slot;
}

/* Drain then dlclose a superseded module .so. Bounded wait; on doubt, keep it
 * mapped forever (always memory-safe). */
static void retire_handle(void *handle, int fd,
                          hotswap_quiesced_cb quiesced_cb, void *ctx)
{
    if (!handle)
        return;
    bool drained = false;
    if (quiesced_cb) {
        /* ~2 s worst case: cheap sched_yield spin, escalating to a 1 ms sleep. */
        for (int i = 0; i < 20000; i++) {
            if (quiesced_cb(ctx)) { drained = true; break; }
            if (i % 1000 == 999) {
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
                nanosleep(&ts, NULL);
            } else {
                sched_yield();
            }
        }
    }
    if (drained) {
        dlclose(handle);
        if (fd >= 0) close(fd);
        atomic_fetch_add_explicit(&g_dlclose_count, 1, memory_order_relaxed);
        LOG_INFO("hotswap.activate",
                 "retired superseded module .so after drain (dlclosed)");
    } else {
        atomic_fetch_add_explicit(&g_retained_mapped_count, 1,
                                  memory_order_relaxed);
        LOG_WARN("hotswap.activate",
                 "drain unconfirmed; keeping superseded module .so mapped "
                 "(safe leak, no quiesce callback or timeout)");
    }
}

bool hotswap_activate(const char *so_path, const char *resolved_datadir,
                      bool request_activate,
                      hotswap_commit_handler_cb commit_cb,
                      hotswap_quiesced_cb quiesced_cb, void *cb_ctx,
                      struct hotswap_activate_report *report)
{
    if (!report)
        return false;
    memset(report, 0, sizeof(*report));
    report->verify_only = !request_activate;

/* Populate the report, log, record the rejection, count it, and return false.
 * Cleanup (dlclose/close) is done at the call site BEFORE invoking this. */
#define ACT_REJECT(stage_, ...)                                              \
    do {                                                                     \
        act_copy(report->stage, sizeof(report->stage), (stage_));            \
        snprintf(report->error, sizeof(report->error), __VA_ARGS__);         \
        report->ok = false;                                                  \
        report->rolled_back = true;                                          \
        atomic_fetch_add_explicit(&g_rollback_count, 1, memory_order_relaxed); \
        record_event(&g_last_rejection, report->handler_name, (stage_),      \
                     report->error, false, false);                           \
        LOG_WARN("hotswap.activate", "reject stage=%s: %s", (stage_),        \
                 report->error);                                             \
        return false;                                                        \
    } while (0)

    char why[256] = {0};
    if (!hotswap_path_is_acceptable(so_path, why, sizeof(why)))
        ACT_REJECT("precheck", "rejected so_path: %s", why);
    if (!hotswap_datadir_is_dev(resolved_datadir))
        ACT_REJECT("precheck",
                   "hot-swap requires the exact dev datadir ~/.zclassic-c23-dev, got '%s'",
                   resolved_datadir ? resolved_datadir : "");

    if (request_activate &&
        !hotswap_activation_authorized(resolved_datadir, why, sizeof(why)))
        ACT_REJECT("authorize", "%s", why);

    int fd = open(so_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        !artifact_sha256_fd(fd, report->artifact_sha256)) {
        if (fd >= 0) close(fd);
        ACT_REJECT("dlopen", "could not pin and hash a regular module artifact");
    }

    char pinned[64];
    (void)snprintf(pinned, sizeof(pinned), "/proc/self/fd/%d", fd);
    void *handle = dlopen(pinned, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char *dl = dlerror();
        char msg[200];
        snprintf(msg, sizeof(msg), "dlopen failed: %s", dl ? dl : "(unknown)");
        close(fd);
        ACT_REJECT("dlopen", "%s", msg);
    }

    dlerror();
    const struct zcl_hotswap_module *mod = dlsym(handle, ZCL_HOTSWAP_MODULE_SYMBOL);
    const char *sym_err = dlerror();
    if (!mod || sym_err) {
        char msg[200];
        snprintf(msg, sizeof(msg), "missing %s symbol: %s",
                 ZCL_HOTSWAP_MODULE_SYMBOL, sym_err ? sym_err : "not found");
        dlclose(handle);
        close(fd);
        ACT_REJECT("abi", "%s", msg);
    }
    if (mod->handler_name)
        act_copy(report->handler_name, sizeof(report->handler_name),
                 mod->handler_name);
    /* ABI version + required fields + swappable allowlist + module self_test,
     * all in one pure gauntlet (also unit-tested directly with a fabricated
     * struct). Any failure => refuse LOUDLY, old handler untouched. */
    char admit_stage[64] = {0};
    if (!hotswap_module_admit(mod, admit_stage, sizeof(admit_stage),
                              why, sizeof(why))) {
        dlclose(handle);
        close(fd);
        ACT_REJECT(admit_stage[0] ? admit_stage : "abi", "%s", why);
    }

    /* Verification passed. */
    if (!request_activate) {
        dlclose(handle);
        close(fd);
        report->ok = true;
        report->verify_only = true;
        report->activated = false;
        act_copy(report->stage, sizeof(report->stage), "verified");
        atomic_fetch_add_explicit(&g_verify_count, 1, memory_order_relaxed);
        record_event(&g_last_activation, report->handler_name, "verified", "",
                     false, true);
        LOG_INFO("hotswap.activate",
                 "verify-only OK handler=%s sha=%s (not activated)",
                 report->handler_name, report->artifact_sha256);
        return true;
    }

    /* Authorized live activation. */
    if (!commit_cb) {
        dlclose(handle);
        close(fd);
        ACT_REJECT("commit", "no registry commit callback supplied");
    }
    uint32_t gen = 0;
    why[0] = '\0';
    if (!commit_cb(cb_ctx, mod->handler_name, mod->fn, &gen, why, sizeof(why))) {
        /* Rollback: the registry never published, old handler untouched. */
        dlclose(handle);
        close(fd);
        ACT_REJECT("commit", "%s", why[0] ? why : "registry commit failed");
    }

    void *prev_handle = NULL;
    int prev_fd = -1;
    pthread_mutex_lock(&g_act_lock);
    struct hotswap_act_slot *slot = slot_for_handler_locked(mod->handler_name);
    if (slot) {
        prev_handle = slot->handle;
        prev_fd = slot->artifact_fd;
        slot->handle = handle;
        slot->artifact_fd = fd;
        slot->generation = gen;
        slot->activated_at = platform_time_wall_time_t();
        slot->swaps++;
        act_copy(slot->artifact_sha256, sizeof(slot->artifact_sha256),
                 report->artifact_sha256);
    }
    pthread_mutex_unlock(&g_act_lock);

    if (!slot) {
        /* Committed but untrackable (table full): keep this .so mapped. */
        atomic_fetch_add_explicit(&g_retained_mapped_count, 1,
                                  memory_order_relaxed);
        LOG_WARN("hotswap.activate",
                 "activation slot table full; keeping module .so mapped");
    }

    report->ok = true;
    report->activated = true;
    report->verify_only = false;
    report->generation = gen;
    act_copy(report->stage, sizeof(report->stage), "activated");
    atomic_fetch_add_explicit(&g_activation_count, 1, memory_order_relaxed);
    record_event(&g_last_activation, report->handler_name, "activated", "",
                 true, true);
    LOG_INFO("hotswap.activate",
             "activated handler=%s gen=%u sha=%s",
             report->handler_name, gen, report->artifact_sha256);

    /* Retire the previous module .so for this handler once dispatch drains. */
    retire_handle(prev_handle, prev_fd, quiesced_cb, cb_ctx);
    return true;
#undef ACT_REJECT
}

#else /* !ZCL_DEV_BUILD — release: no dynamic activation surface */

bool hotswap_activate(const char *so_path, const char *resolved_datadir,
                      bool request_activate,
                      hotswap_commit_handler_cb commit_cb,
                      hotswap_quiesced_cb quiesced_cb, void *cb_ctx,
                      struct hotswap_activate_report *report)
{
    (void)so_path;
    (void)resolved_datadir;
    (void)request_activate;
    (void)commit_cb;
    (void)quiesced_cb;
    (void)cb_ctx;
    if (!report)
        return false;
    memset(report, 0, sizeof(*report));
    report->verify_only = true;
    report->rolled_back = true;
    act_copy(report->stage, sizeof(report->stage), "release");
    act_copy(report->error, sizeof(report->error),
             "hot-swap activation unavailable in release build");
    return false;
}

#endif /* ZCL_DEV_BUILD */
