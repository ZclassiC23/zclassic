/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: warm AF_UNIX-only native presentation host and event return. */

#define _GNU_SOURCE
#include "views/ui_present_host.h"
#include "views/ui_present_host_transport.h"

#include "base/serialize_le.h"
#include "encoding/qr.h"
#include "platform/os_proc.h"
#include "platform/time_compat.h"
#include "presentation/model_render.h"
#include "presentation/presentation.h"
#include "presentation/zclassic_brand.h"
#include "util/log_macros.h"
#include "util/spawn.h"
#include "views/qr_popup.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define UI_HOST_START_TIMEOUT_MS 1000
#define UI_HOST_READY_TIMEOUT_MS 3000
#define UI_HOST_EVENT_TIMEOUT_MS (10 * 60 * 1000)
#define UI_HOST_IDLE_EXIT_US (10LL * 60LL * 1000000LL)
#define UI_HOST_SESSIONS_MAX 16u

static struct zcl_result ui_host_error(const char *where)
{
    return ZCL_ERR(-errno, "%s: %s", where, strerror(errno));
}

#if defined(__linux__)
static struct zcl_result ui_host_launch(void)
{
    char executable[PATH_MAX];
    if (!os_proc_exe_path(executable, sizeof(executable)))
        return ui_host_error("presentation host executable path");
    const char *argv[] = {executable, "--ui-present-host", NULL};
    return zcl_spawn_detached(argv, NULL);
}

static int ui_host_connect(bool *reused)
{
    int fd = ui_host_transport_connect_once();
    if (fd >= 0) {
        *reused = true;
        return fd;
    }
    *reused = false;
    struct zcl_result launched = ui_host_launch();
    if (!launched.ok) {
        errno = EIO;
        return -1;
    }
    int64_t deadline = platform_time_monotonic_us() +
                       UI_HOST_START_TIMEOUT_MS * 1000LL;
    do {
        platform_sleep_ms(5);
        fd = ui_host_transport_connect_once();
        if (fd >= 0) return fd;
    } while (platform_time_monotonic_us() < deadline);
    errno = ETIMEDOUT;
    return -1;
}

struct ui_host_ready_context {
    int fd;
    int64_t started_us;
    uint32_t ready_value;
    uint8_t nonce[UI_HOST_NONCE_BYTES];
};

static void ui_host_window_ready(void *context)
{
    struct ui_host_ready_context *ready = context;
    uint8_t reply[UI_HOST_REPLY_BYTES];
    int64_t elapsed = platform_time_monotonic_us() - ready->started_us;
    ui_host_transport_reply(reply, UI_HOST_PHASE_READY, UI_HOST_STATUS_OK,
                            ready->ready_value,
                            elapsed > 0 ? (uint64_t)elapsed : 0,
                            ready->nonce);
    (void)ui_host_transport_send_all(ready->fd, reply, sizeof(reply));
}

static void ui_host_send_rejected(
    int fd, uint16_t phase, const uint8_t nonce[UI_HOST_NONCE_BYTES])
{
    uint8_t reply[UI_HOST_REPLY_BYTES];
    ui_host_transport_reply(reply, phase, UI_HOST_STATUS_REJECTED,
                            UINT32_MAX, 0, nonce);
    (void)ui_host_transport_send_all(fd, reply, sizeof(reply));
}

static bool ui_host_show_pages(
    int client,
    uint16_t flags,
    bool view_replaced,
    const struct zcl_present_window_pages_v1 *pages,
    uint32_t action_count,
    const uint8_t nonce[UI_HOST_NONCE_BYTES])
{
    char why[192];
    struct ui_host_ready_context ready = {
        .fd = client,
        .started_us = platform_time_monotonic_us(),
        .ready_value = view_replaced ? 1u : 0u,
    };
    memcpy(ready.nonce, nonce, UI_HOST_NONCE_BYTES);
    struct zcl_present_window_event_v1 event;
    bool shown = zcl_present_window_run_pages_actions_v1(
        pages, action_count, ui_host_window_ready, &ready,
        &event, why, sizeof(why));
    if (!shown)
        LOG_WARN("presentation.host", "native window failed: %s", why);
    if (shown && (flags & UI_HOST_FLAG_WAIT_EVENT)) {
        uint8_t reply[UI_HOST_REPLY_BYTES];
        uint32_t action = event.outcome == ZCL_PRESENT_WINDOW_ACTION
            ? event.action_index : UINT32_MAX;
        ui_host_transport_reply(reply, UI_HOST_PHASE_EVENT,
                                UI_HOST_STATUS_OK, action, 0, nonce);
        (void)ui_host_transport_send_all(client, reply, sizeof(reply));
    }
    return shown;
}

static bool ui_host_show_window(
    int client,
    uint16_t flags,
    bool view_replaced,
    const struct zcl_present_window_v1 *window,
    uint32_t action_count,
    const uint8_t nonce[UI_HOST_NONCE_BYTES])
{
    struct zcl_present_window_pages_v1 pages = {
        .struct_size = sizeof(pages),
        .abi_version = ZCL_PRESENT_ABI_V1,
        .pages = window,
        .page_count = 1,
    };
    return ui_host_show_pages(client, flags, view_replaced, &pages,
                              action_count, nonce);
}

static bool ui_host_worker_model(int client, uint16_t flags,
                                 bool view_replaced,
                                 const uint8_t *wire, uint32_t wire_len,
                                 const uint8_t nonce[UI_HOST_NONCE_BYTES])
{
    struct zcl_present_model_v1 model;
    struct zcl_present_model_bitmap_v1
        bitmaps[ZCL_PRESENT_MODEL_PAGES_MAX] = {{0}};
    struct zcl_present_window_v1 windows[ZCL_PRESENT_MODEL_PAGES_MAX] = {{0}};
    char why[192];
    if (!zcl_present_model_decode_v1(wire, wire_len, &model,
                                     why, sizeof(why))) {
        LOG_WARN("presentation.host", "visual model rejected: %s", why);
        return false;
    }
    uint32_t page_count = 0;
    if (!zcl_present_model_page_count_v1(&model, &page_count,
                                         why, sizeof(why))) {
        LOG_WARN("presentation.host", "visual model pagination failed: %s", why);
        return false;
    }
    uint8_t icon[ZCL_PRESENT_ZCLASSIC_ICON_RGBA_BYTES];
    if (!zcl_present_zclassic_icon_rgba(icon, sizeof(icon))) {
        LOG_WARN("presentation.host", "native window icon is unavailable");
        return false;
    }
    char title[ZCL_PRESENT_TITLE_MAX + 1u];
    (void)snprintf(title, sizeof(title), "ZClassic23 — %s", model.title);
    bool rendered = true;
    for (uint32_t i = 0; i < page_count; i++) {
        if (!zcl_present_model_render_page_v1(
                &model, i, &bitmaps[i], why, sizeof(why))) {
            LOG_WARN("presentation.host",
                     "visual model page render failed: %s", why);
            rendered = false;
            break;
        }
        windows[i] = (struct zcl_present_window_v1){
            .struct_size = sizeof(windows[i]),
            .abi_version = ZCL_PRESENT_ABI_V1,
            .title = title,
            .pixels = bitmaps[i].pixels,
            .width = bitmaps[i].width,
            .height = bitmaps[i].height,
            .pixel_format = ZCL_PRESENT_RGB8,
            .icon_rgba = icon,
            .icon_width = ZCL_PRESENT_ZCLASSIC_ICON_WIDTH,
            .icon_height = ZCL_PRESENT_ZCLASSIC_ICON_HEIGHT,
            .copy_text = model.exact_root[0] ? model.exact_root : NULL,
        };
    }
    bool shown = false;
    if (rendered) {
        struct zcl_present_window_pages_v1 pages = {
            .struct_size = sizeof(pages),
            .abi_version = ZCL_PRESENT_ABI_V1,
            .pages = windows,
            .page_count = page_count,
        };
        shown = ui_host_show_pages(client, flags, view_replaced, &pages,
                                   model.action_count, nonce);
    }
    for (uint32_t i = 0; i < page_count; i++)
        zcl_present_model_bitmap_free_v1(&bitmaps[i]);
    return shown;
}

static bool ui_host_qr_decode(const uint8_t *wire, uint32_t wire_len,
                              char payload[ZCL_QR_MAX_PAYLOAD + 1u],
                              char title[81])
{
    if (wire_len < 4u) return false;
    uint16_t payload_len = zcl_read_u16_le(wire);
    uint16_t title_len = zcl_read_u16_le(wire + 2u);
    if (payload_len == 0 || payload_len > ZCL_QR_MAX_PAYLOAD ||
        title_len > 80u ||
        wire_len != 4u + (uint32_t)payload_len + (uint32_t)title_len)
        return false;
    memcpy(payload, wire + 4u, payload_len);
    payload[payload_len] = '\0';
    memcpy(title, wire + 4u + payload_len, title_len);
    title[title_len] = '\0';
    return memchr(payload, '\0', payload_len) == NULL &&
           memchr(title, '\0', title_len) == NULL;
}

static bool ui_host_worker_qr(int client, const uint8_t *wire,
                              uint32_t wire_len,
                              const uint8_t nonce[UI_HOST_NONCE_BYTES])
{
    char payload[ZCL_QR_MAX_PAYLOAD + 1u];
    char requested_title[81];
    if (!ui_host_qr_decode(wire, wire_len, payload, requested_title)) {
        LOG_WARN("presentation.host", "length-framed QR request is invalid");
        return false;
    }
    struct qr_popup_card card;
    char why[192];
    if (!qr_popup_card_render(payload, requested_title, &card,
                              why, sizeof(why))) {
        LOG_WARN("presentation.host", "QR card render failed: %s", why);
        return false;
    }
    uint8_t icon[ZCL_PRESENT_ZCLASSIC_ICON_RGBA_BYTES];
    if (!zcl_present_zclassic_icon_rgba(icon, sizeof(icon))) {
        qr_popup_card_free(&card);
        LOG_WARN("presentation.host", "QR window icon is unavailable");
        return false;
    }
    char title[ZCL_PRESENT_TITLE_MAX + 1u];
    const char *kind = card.is_deposit ? "Deposit ZCL" :
        (requested_title[0] ? requested_title : "QR Code");
    (void)snprintf(title, sizeof(title),
                   "ZClassic23 — %s — C copies, Esc closes", kind);
    struct zcl_present_window_v1 window = {
        .struct_size = sizeof(window),
        .abi_version = ZCL_PRESENT_ABI_V1,
        .title = title,
        .pixels = card.pixels,
        .width = card.width,
        .height = card.height,
        .pixel_format = ZCL_PRESENT_RGB8,
        .icon_rgba = icon,
        .icon_width = ZCL_PRESENT_ZCLASSIC_ICON_WIDTH,
        .icon_height = ZCL_PRESENT_ZCLASSIC_ICON_HEIGHT,
        .copy_text = payload,
    };
    bool shown = ui_host_show_window(client, 0, false, &window, 0, nonce);
    qr_popup_card_free(&card);
    return shown;
}

static int ui_host_worker(int listener, int client, uint16_t flags,
                          bool view_replaced, const uint8_t *wire,
                          uint32_t wire_len,
                          const uint8_t nonce[UI_HOST_NONCE_BYTES])
{
    close(listener);
    bool shown = flags & UI_HOST_FLAG_QR_CARD
        ? ui_host_worker_qr(client, wire, wire_len, nonce)
        : ui_host_worker_model(client, flags, view_replaced, wire, wire_len,
                               nonce);
    if (!shown) ui_host_send_rejected(client, UI_HOST_PHASE_READY, nonce);
    close(client);
    return shown ? 0 : 1;
}

struct ui_host_session {
    pid_t worker;
    char request_id[ZCL_PRESENT_MODEL_ID_MAX + 1u];
};

static void ui_host_session_forget_worker(
    struct ui_host_session sessions[UI_HOST_SESSIONS_MAX], pid_t worker)
{
    for (size_t i = 0; i < UI_HOST_SESSIONS_MAX; i++) {
        if (sessions[i].worker == worker)
            sessions[i] = (struct ui_host_session){0};
    }
}

static void ui_host_sessions_reap(
    struct ui_host_session sessions[UI_HOST_SESSIONS_MAX])
{
    for (;;) {
        pid_t worker = waitpid(-1, NULL, WNOHANG);
        if (worker > 0) {
            ui_host_session_forget_worker(sessions, worker);
            continue;
        }
        if (worker < 0 && errno == EINTR) continue;
        break;
    }
}

/* Replace only a still-owned, unreaped display worker. Keeping children as
 * zombies until this parent reaps them prevents PID reuse from ever turning a
 * visual replacement into a signal sent to an unrelated process. */
static bool ui_host_session_replace(
    struct ui_host_session sessions[UI_HOST_SESSIONS_MAX],
    const char *request_id)
{
    ui_host_sessions_reap(sessions);
    for (size_t i = 0; i < UI_HOST_SESSIONS_MAX; i++) {
        if (sessions[i].worker <= 0 ||
            strcmp(sessions[i].request_id, request_id) != 0)
            continue;
        pid_t worker = sessions[i].worker;
        if (kill(worker, SIGTERM) != 0 && errno != ESRCH)
            LOG_WARN("presentation.host",
                     "prior display worker termination failed: %s",
                     strerror(errno));
        while (waitpid(worker, NULL, 0) < 0 && errno == EINTR) {}
        sessions[i] = (struct ui_host_session){0};
        return true;
    }
    return false;
}

static bool ui_host_session_remember(
    struct ui_host_session sessions[UI_HOST_SESSIONS_MAX],
    const char *request_id, pid_t worker)
{
    /* Do not reap here: this worker may have exited between fork and this
     * bookkeeping step. Leaving it waitable until a later reap prevents its
     * PID from being reused while the table records ownership. */
    for (size_t i = 0; i < UI_HOST_SESSIONS_MAX; i++) {
        if (sessions[i].worker != 0) continue;
        sessions[i].worker = worker;
        (void)snprintf(sessions[i].request_id,
                       sizeof(sessions[i].request_id), "%s", request_id);
        return true;
    }
    return false;
}

static bool ui_host_session_has_slot(
    struct ui_host_session sessions[UI_HOST_SESSIONS_MAX],
    const char *request_id)
{
    ui_host_sessions_reap(sessions);
    for (size_t i = 0; i < UI_HOST_SESSIONS_MAX; i++) {
        if (sessions[i].worker == 0 ||
            strcmp(sessions[i].request_id, request_id) == 0)
            return true;
    }
    return false;
}

static struct zcl_result ui_host_submit_wire(
    const uint8_t *wire,
    size_t wire_len,
    uint16_t flags,
    struct ui_present_host_result *result)
{
    bool reused = false;
    char display_why[96];
    if (!ui_present_host_display_ready(display_why, sizeof(display_why)))
        return ZCL_ERR(-1, "%s", display_why);
    int fd = ui_host_connect(&reused);
    if (fd < 0) return ui_host_error("presentation host connect");
    uint8_t header[UI_HOST_REQUEST_BYTES];
    uint8_t nonce[UI_HOST_NONCE_BYTES];
    if (!ui_host_transport_nonce(nonce)) {
        close(fd);
        return ui_host_error("presentation host request nonce");
    }
    ui_host_transport_request_header(header, flags, (uint32_t)wire_len,
                                     nonce);
    if (!ui_host_transport_send_all(fd, header, sizeof(header)) ||
        !ui_host_transport_send_all(fd, wire, wire_len)) {
        int saved = errno;
        close(fd);
        errno = saved;
        return ui_host_error("presentation host request");
    }
    uint8_t reply[UI_HOST_REPLY_BYTES];
    uint32_t status = 0;
    uint32_t value = 0;
    uint64_t elapsed_us = 0;
    if (!ui_host_transport_recv_all(fd, reply, sizeof(reply),
                                    UI_HOST_READY_TIMEOUT_MS) ||
        !ui_host_transport_parse_reply(reply, UI_HOST_PHASE_READY, &status,
                                       &value, &elapsed_us, nonce) ||
        status != UI_HOST_STATUS_OK) {
        close(fd);
        return ZCL_ERR(-1, "presentation host rejected the native window");
    }
    result->resident_host = true;
    result->host_reused = reused;
    result->view_replaced = value == 1u;
    result->ready_us = elapsed_us > INT64_MAX ? INT64_MAX
                                               : (int64_t)elapsed_us;
    if (!(flags & UI_HOST_FLAG_WAIT_EVENT)) {
        close(fd);
        return ZCL_OK;
    }
    if (!ui_host_transport_recv_all(fd, reply, sizeof(reply),
                                    UI_HOST_EVENT_TIMEOUT_MS) ||
        !ui_host_transport_parse_reply(reply, UI_HOST_PHASE_EVENT, &status,
                                       &value, &elapsed_us, nonce) ||
        status != UI_HOST_STATUS_OK) {
        close(fd);
        return ZCL_ERR(-1, "presentation host event channel closed");
    }
    close(fd);
    result->event_received = true;
    result->action_index = value;
    return ZCL_OK;
}
#endif /* __linux__ */

struct zcl_result ui_present_host_submit(
    const struct zcl_present_model_v1 *model,
    bool wait_for_event,
    struct ui_present_host_result *result)
{
    if (!result) return ZCL_ERR(-1, "presentation host result is missing");
    *result = (struct ui_present_host_result){
        .action_index = UINT32_MAX,
    };
    char why[192];
    if (!zcl_present_model_validate_v1(model, why, sizeof(why)))
        return ZCL_ERR(-1, "presentation host model: %s", why);
#if !defined(__linux__)
    (void)wait_for_event;
    return ZCL_ERR(-1, "resident presentation host is not yet available on this platform");
#else
    uint8_t wire[ZCL_PRESENT_MODEL_WIRE_MAX];
    size_t wire_len = 0;
    if (!zcl_present_model_encode_v1(model, wire, sizeof(wire), &wire_len,
                                     why, sizeof(why)))
        return ZCL_ERR(-1, "presentation host model encode: %s", why);
    return ui_host_submit_wire(
        wire, wire_len, wait_for_event ? UI_HOST_FLAG_WAIT_EVENT : 0,
        result);
#endif
}

struct zcl_result ui_present_host_submit_qr(
    const char *payload,
    const char *title,
    struct ui_present_host_result *result)
{
    if (!result) return ZCL_ERR(-1, "presentation host result is missing");
    *result = (struct ui_present_host_result){
        .action_index = UINT32_MAX,
    };
    size_t payload_len = payload
        ? strnlen(payload, ZCL_QR_MAX_PAYLOAD + 1u) : 0;
    size_t title_len = title ? strnlen(title, 81u) : 0;
    if (payload_len == 0 || payload_len > ZCL_QR_MAX_PAYLOAD)
        return ZCL_ERR(-1, "QR payload is empty or exceeds 2048 bytes");
    if (title_len > 80u)
        return ZCL_ERR(-1, "QR title exceeds 80 bytes");
#if !defined(__linux__)
    return ZCL_ERR(-1, "resident presentation host is not yet available on this platform");
#else
    uint8_t wire[4u + ZCL_QR_MAX_PAYLOAD + 80u];
    zcl_write_u16_le(wire, (uint16_t)payload_len);
    zcl_write_u16_le(wire + 2u, (uint16_t)title_len);
    memcpy(wire + 4u, payload, payload_len);
    if (title_len > 0) memcpy(wire + 4u + payload_len, title, title_len);
    return ui_host_submit_wire(wire, 4u + payload_len + title_len,
                               UI_HOST_FLAG_QR_CARD, result);
#endif
}

int ui_present_host_main(void)
{
#if !defined(__linux__)
    (void)fprintf(stderr, "Resident presentation host unsupported.\n"); // obs-ok:detached-child-terminal-diagnostic
    return 2;
#else
    int listener = ui_host_transport_listen();
    if (listener < 0) return 2;
    struct ui_host_session sessions[UI_HOST_SESSIONS_MAX] = {{0}};
    int64_t last_request_us = platform_time_monotonic_us();
    for (;;) {
        struct pollfd wait = {.fd = listener, .events = POLLIN};
        int ready = poll(&wait, 1, 1000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) break;
        if (ready == 0) {
            ui_host_sessions_reap(sessions);
            if (platform_time_monotonic_us() - last_request_us >
                UI_HOST_IDLE_EXIT_US)
                break;
            continue;
        }
        int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0) continue;
        last_request_us = platform_time_monotonic_us();
        if (!ui_host_transport_peer_allowed(client)) {
            close(client);
            continue;
        }
        uint8_t header[UI_HOST_REQUEST_BYTES];
        uint16_t flags = 0;
        uint32_t wire_len = 0;
        uint8_t nonce[UI_HOST_NONCE_BYTES] = {0};
        if (!ui_host_transport_recv_all(client, header, sizeof(header),
                                        UI_HOST_READY_TIMEOUT_MS) ||
            !ui_host_transport_parse_request_header(
                header, &flags, &wire_len, nonce)) {
            ui_host_send_rejected(client, UI_HOST_PHASE_READY, nonce);
            close(client);
            continue;
        }
        uint8_t wire[ZCL_PRESENT_MODEL_WIRE_MAX];
        if (!ui_host_transport_recv_all(client, wire, wire_len,
                                        UI_HOST_READY_TIMEOUT_MS)) {
            ui_host_send_rejected(client, UI_HOST_PHASE_READY, nonce);
            close(client);
            continue;
        }
        bool replaceable = false;
        bool view_replaced = false;
        char request_id[ZCL_PRESENT_MODEL_ID_MAX + 1u] = {0};
        if (!(flags & UI_HOST_FLAG_QR_CARD)) {
            struct zcl_present_model_v1 model;
            char why[192];
            if (!zcl_present_model_decode_v1(wire, wire_len, &model,
                                             why, sizeof(why)) ||
                (!!(flags & UI_HOST_FLAG_WAIT_EVENT) !=
                 (model.action_count > 0))) {
                LOG_WARN("presentation.host",
                         "resident visual request rejected before fork");
                ui_host_send_rejected(client, UI_HOST_PHASE_READY, nonce);
                close(client);
                continue;
            }
            replaceable = model.action_count == 0;
            (void)snprintf(request_id, sizeof(request_id), "%s",
                           model.request_id);
            if (replaceable &&
                !ui_host_session_has_slot(sessions, request_id)) {
                ui_host_send_rejected(client, UI_HOST_PHASE_READY, nonce);
                close(client);
                continue;
            }
            if (replaceable)
                view_replaced = ui_host_session_replace(sessions,
                                                        request_id);
        }
        pid_t worker = fork();
        if (worker == 0)
            _exit(ui_host_worker(listener, client, flags, view_replaced,
                                 wire, wire_len, nonce));
        if (worker < 0 ||
            (replaceable &&
             !ui_host_session_remember(sessions, request_id, worker))) {
            if (worker > 0) {
                (void)kill(worker, SIGTERM);
                while (waitpid(worker, NULL, 0) < 0 && errno == EINTR) {}
            }
            ui_host_send_rejected(client, UI_HOST_PHASE_READY, nonce);
            close(client);
            continue;
        }
        close(client);
    }
    for (size_t i = 0; i < UI_HOST_SESSIONS_MAX; i++) {
        if (sessions[i].worker <= 0) continue;
        (void)kill(sessions[i].worker, SIGTERM);
        while (waitpid(sessions[i].worker, NULL, 0) < 0 && errno == EINTR) {}
    }
    close(listener);
    ui_host_transport_cleanup();
    return 0;
#endif
}
