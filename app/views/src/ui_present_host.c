/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: warm AF_UNIX-only native presentation host and event return. */

#define _GNU_SOURCE
#include "views/ui_present_host.h"

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
#include <sys/un.h>
#include <unistd.h>
#endif

#define UI_HOST_PROTOCOL_VERSION 1u
#define UI_HOST_REQUEST_BYTES 16u
#define UI_HOST_REPLY_BYTES 24u
#define UI_HOST_FLAG_WAIT_EVENT 1u
#define UI_HOST_FLAG_QR_CARD 2u
#define UI_HOST_PHASE_READY 1u
#define UI_HOST_PHASE_EVENT 2u
#define UI_HOST_STATUS_OK 0u
#define UI_HOST_STATUS_REJECTED 1u
#define UI_HOST_START_TIMEOUT_MS 1000
#define UI_HOST_READY_TIMEOUT_MS 3000
#define UI_HOST_EVENT_TIMEOUT_MS (10 * 60 * 1000)
#define UI_HOST_IDLE_EXIT_US (10LL * 60LL * 1000000LL)

static const uint8_t UI_HOST_REQUEST_MAGIC[4] = {'Z', 'P', 'H', 'R'};
static const uint8_t UI_HOST_REPLY_MAGIC[4] = {'Z', 'P', 'H', 'A'};

static struct zcl_result ui_host_error(const char *where)
{
    return ZCL_ERR(-errno, "%s: %s", where, strerror(errno));
}

#if defined(__linux__)
static uint32_t ui_host_display_hash(void)
{
    const unsigned char *display =
        (const unsigned char *)(getenv("DISPLAY") ? getenv("DISPLAY") : "");
    uint32_t hash = 2166136261u;
    while (*display) {
        hash ^= *display++;
        hash *= 16777619u;
    }
    return hash;
}

static socklen_t ui_host_address(struct sockaddr_un *address)
{
    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    int length = snprintf(address->sun_path + 1,
                          sizeof(address->sun_path) - 1u,
                          "zclassic23-present-v1-%lu-%08x",
                          (unsigned long)geteuid(), ui_host_display_hash());
    if (length <= 0 || (size_t)length >= sizeof(address->sun_path) - 1u)
        return 0;
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1u +
                       (size_t)length);
}

static int ui_host_connect_once(void)
{
    struct sockaddr_un address;
    socklen_t address_len = ui_host_address(&address);
    if (address_len == 0) {
        errno = EINVAL;
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (const struct sockaddr *)&address, address_len) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static bool ui_host_send_all(int fd, const uint8_t *bytes, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        ssize_t count = send(fd, bytes + sent, length - sent, MSG_NOSIGNAL);
        if (count > 0) {
            sent += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

static bool ui_host_recv_all(int fd, uint8_t *bytes, size_t length,
                             int timeout_ms)
{
    size_t received = 0;
    while (received < length) {
        struct pollfd wait = {.fd = fd, .events = POLLIN};
        int ready;
        do {
            ready = poll(&wait, 1, timeout_ms);
        } while (ready < 0 && errno == EINTR);
        if (ready <= 0 || !(wait.revents & (POLLIN | POLLHUP))) {
            if (ready == 0) errno = ETIMEDOUT;
            return false;
        }
        ssize_t count = recv(fd, bytes + received, length - received, 0);
        if (count > 0) {
            received += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count == 0) errno = ECONNRESET;
        return false;
    }
    return true;
}

static void ui_host_request_header(uint8_t out[UI_HOST_REQUEST_BYTES],
                                   uint16_t flags, uint32_t model_len)
{
    memcpy(out, UI_HOST_REQUEST_MAGIC, sizeof(UI_HOST_REQUEST_MAGIC));
    zcl_write_u16_le(out + 4u, UI_HOST_PROTOCOL_VERSION);
    zcl_write_u16_le(out + 6u, flags);
    zcl_write_u32_le(out + 8u, model_len);
    zcl_write_u32_le(out + 12u, 0);
}

static bool ui_host_parse_request_header(
    const uint8_t in[UI_HOST_REQUEST_BYTES], uint16_t *flags,
    uint32_t *model_len)
{
    if (memcmp(in, UI_HOST_REQUEST_MAGIC,
               sizeof(UI_HOST_REQUEST_MAGIC)) != 0 ||
        zcl_read_u16_le(in + 4u) != UI_HOST_PROTOCOL_VERSION ||
        zcl_read_u32_le(in + 12u) != 0)
        return false;
    *flags = zcl_read_u16_le(in + 6u);
    *model_len = zcl_read_u32_le(in + 8u);
    return (*flags & ~(UI_HOST_FLAG_WAIT_EVENT | UI_HOST_FLAG_QR_CARD)) == 0 &&
           !((*flags & UI_HOST_FLAG_WAIT_EVENT) &&
             (*flags & UI_HOST_FLAG_QR_CARD)) &&
           *model_len > 0 && *model_len <= ZCL_PRESENT_MODEL_WIRE_MAX;
}

static void ui_host_reply_bytes(uint8_t out[UI_HOST_REPLY_BYTES],
                                uint16_t phase, uint32_t status,
                                uint32_t value, uint64_t elapsed_us)
{
    memcpy(out, UI_HOST_REPLY_MAGIC, sizeof(UI_HOST_REPLY_MAGIC));
    zcl_write_u16_le(out + 4u, UI_HOST_PROTOCOL_VERSION);
    zcl_write_u16_le(out + 6u, phase);
    zcl_write_u32_le(out + 8u, status);
    zcl_write_u32_le(out + 12u, value);
    zcl_write_u64_le(out + 16u, elapsed_us);
}

static bool ui_host_reply_parse(const uint8_t in[UI_HOST_REPLY_BYTES],
                                uint16_t expected_phase,
                                uint32_t *status, uint32_t *value,
                                uint64_t *elapsed_us)
{
    if (memcmp(in, UI_HOST_REPLY_MAGIC, sizeof(UI_HOST_REPLY_MAGIC)) != 0 ||
        zcl_read_u16_le(in + 4u) != UI_HOST_PROTOCOL_VERSION ||
        zcl_read_u16_le(in + 6u) != expected_phase)
        return false;
    *status = zcl_read_u32_le(in + 8u);
    *value = zcl_read_u32_le(in + 12u);
    *elapsed_us = zcl_read_u64_le(in + 16u);
    return true;
}

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
    int fd = ui_host_connect_once();
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
        fd = ui_host_connect_once();
        if (fd >= 0) return fd;
    } while (platform_time_monotonic_us() < deadline);
    errno = ETIMEDOUT;
    return -1;
}

struct ui_host_ready_context {
    int fd;
    int64_t started_us;
};

static void ui_host_window_ready(void *context)
{
    struct ui_host_ready_context *ready = context;
    uint8_t reply[UI_HOST_REPLY_BYTES];
    int64_t elapsed = platform_time_monotonic_us() - ready->started_us;
    ui_host_reply_bytes(reply, UI_HOST_PHASE_READY, UI_HOST_STATUS_OK, 0,
                        elapsed > 0 ? (uint64_t)elapsed : 0);
    (void)ui_host_send_all(ready->fd, reply, sizeof(reply));
}

static void ui_host_send_rejected(int fd, uint16_t phase)
{
    uint8_t reply[UI_HOST_REPLY_BYTES];
    ui_host_reply_bytes(reply, phase, UI_HOST_STATUS_REJECTED, UINT32_MAX, 0);
    (void)ui_host_send_all(fd, reply, sizeof(reply));
}

static bool ui_host_show_window(
    int client,
    uint16_t flags,
    const struct zcl_present_window_v1 *window,
    uint32_t action_count)
{
    char why[192];
    struct ui_host_ready_context ready = {
        .fd = client,
        .started_us = platform_time_monotonic_us(),
    };
    struct zcl_present_window_event_v1 event;
    bool shown = zcl_present_window_run_actions_v1(
        window, action_count, ui_host_window_ready, &ready,
        &event, why, sizeof(why));
    if (!shown)
        LOG_WARN("presentation.host", "native window failed: %s", why);
    if (shown && (flags & UI_HOST_FLAG_WAIT_EVENT)) {
        uint8_t reply[UI_HOST_REPLY_BYTES];
        uint32_t action = event.outcome == ZCL_PRESENT_WINDOW_ACTION
            ? event.action_index : UINT32_MAX;
        ui_host_reply_bytes(reply, UI_HOST_PHASE_EVENT, UI_HOST_STATUS_OK,
                            action, 0);
        (void)ui_host_send_all(client, reply, sizeof(reply));
    }
    return shown;
}

static bool ui_host_worker_model(int client, uint16_t flags,
                                 const uint8_t *wire, uint32_t wire_len)
{
    struct zcl_present_model_v1 model;
    struct zcl_present_model_bitmap_v1 bitmap;
    char why[192];
    if (!zcl_present_model_decode_v1(wire, wire_len, &model,
                                     why, sizeof(why))) {
        LOG_WARN("presentation.host", "visual model rejected: %s", why);
        return false;
    }
    if (!zcl_present_model_render_v1(&model, &bitmap, why, sizeof(why))) {
        LOG_WARN("presentation.host", "visual model render failed: %s", why);
        return false;
    }
    uint8_t icon[ZCL_PRESENT_ZCLASSIC_ICON_RGBA_BYTES];
    if (!zcl_present_zclassic_icon_rgba(icon, sizeof(icon))) {
        zcl_present_model_bitmap_free_v1(&bitmap);
        LOG_WARN("presentation.host", "native window icon is unavailable");
        return false;
    }
    char title[ZCL_PRESENT_TITLE_MAX + 1u];
    (void)snprintf(title, sizeof(title), "ZClassic23 — %s", model.title);
    struct zcl_present_window_v1 window = {
        .struct_size = sizeof(window),
        .abi_version = ZCL_PRESENT_ABI_V1,
        .title = title,
        .pixels = bitmap.pixels,
        .width = bitmap.width,
        .height = bitmap.height,
        .pixel_format = ZCL_PRESENT_RGB8,
        .icon_rgba = icon,
        .icon_width = ZCL_PRESENT_ZCLASSIC_ICON_WIDTH,
        .icon_height = ZCL_PRESENT_ZCLASSIC_ICON_HEIGHT,
        .copy_text = model.exact_root[0] ? model.exact_root : NULL,
    };
    bool shown = ui_host_show_window(client, flags, &window,
                                     model.action_count);
    zcl_present_model_bitmap_free_v1(&bitmap);
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
                              uint32_t wire_len)
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
    bool shown = ui_host_show_window(client, 0, &window, 0);
    qr_popup_card_free(&card);
    return shown;
}

static int ui_host_worker(int listener, int client)
{
    close(listener);
    uint8_t header[UI_HOST_REQUEST_BYTES];
    uint16_t flags = 0;
    uint32_t model_len = 0;
    if (!ui_host_recv_all(client, header, sizeof(header),
                          UI_HOST_READY_TIMEOUT_MS) ||
        !ui_host_parse_request_header(header, &flags, &model_len)) {
        ui_host_send_rejected(client, UI_HOST_PHASE_READY);
        close(client);
        return 2;
    }
    uint8_t wire[ZCL_PRESENT_MODEL_WIRE_MAX];
    if (!ui_host_recv_all(client, wire, model_len,
                          UI_HOST_READY_TIMEOUT_MS)) {
        ui_host_send_rejected(client, UI_HOST_PHASE_READY);
        close(client);
        return 2;
    }
    bool shown = flags & UI_HOST_FLAG_QR_CARD
        ? ui_host_worker_qr(client, wire, model_len)
        : ui_host_worker_model(client, flags, wire, model_len);
    if (!shown) ui_host_send_rejected(client, UI_HOST_PHASE_READY);
    close(client);
    return shown ? 0 : 1;
}

static bool ui_host_peer_allowed(int client)
{
    struct ucred peer;
    socklen_t peer_len = sizeof(peer);
    return getsockopt(client, SOL_SOCKET, SO_PEERCRED,
                      &peer, &peer_len) == 0 &&
           peer_len == sizeof(peer) && peer.uid == geteuid();
}

static int ui_host_listen(void)
{
    struct sockaddr_un address;
    socklen_t address_len = ui_host_address(&address);
    if (address_len == 0) return -1;
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) return -1;
    if (bind(listener, (const struct sockaddr *)&address, address_len) != 0 ||
        listen(listener, 16) != 0) {
        int saved = errno;
        close(listener);
        errno = saved;
        return -1;
    }
    return listener;
}

static struct zcl_result ui_host_submit_wire(
    const uint8_t *wire,
    size_t wire_len,
    uint16_t flags,
    struct ui_present_host_result *result)
{
    bool reused = false;
    int fd = ui_host_connect(&reused);
    if (fd < 0) return ui_host_error("presentation host connect");
    uint8_t header[UI_HOST_REQUEST_BYTES];
    ui_host_request_header(header, flags, (uint32_t)wire_len);
    if (!ui_host_send_all(fd, header, sizeof(header)) ||
        !ui_host_send_all(fd, wire, wire_len)) {
        int saved = errno;
        close(fd);
        errno = saved;
        return ui_host_error("presentation host request");
    }
    uint8_t reply[UI_HOST_REPLY_BYTES];
    uint32_t status = 0;
    uint32_t value = 0;
    uint64_t elapsed_us = 0;
    if (!ui_host_recv_all(fd, reply, sizeof(reply),
                          UI_HOST_READY_TIMEOUT_MS) ||
        !ui_host_reply_parse(reply, UI_HOST_PHASE_READY, &status,
                             &value, &elapsed_us) ||
        status != UI_HOST_STATUS_OK) {
        close(fd);
        return ZCL_ERR(-1, "presentation host rejected the native window");
    }
    result->resident_host = true;
    result->host_reused = reused;
    result->ready_us = elapsed_us > INT64_MAX ? INT64_MAX
                                               : (int64_t)elapsed_us;
    if (!(flags & UI_HOST_FLAG_WAIT_EVENT)) {
        close(fd);
        return ZCL_OK;
    }
    if (!ui_host_recv_all(fd, reply, sizeof(reply),
                          UI_HOST_EVENT_TIMEOUT_MS) ||
        !ui_host_reply_parse(reply, UI_HOST_PHASE_EVENT, &status,
                             &value, &elapsed_us) ||
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
    int listener = ui_host_listen();
    if (listener < 0) return 2;
    (void)signal(SIGCHLD, SIG_IGN);
    int64_t last_request_us = platform_time_monotonic_us();
    for (;;) {
        struct pollfd wait = {.fd = listener, .events = POLLIN};
        int ready = poll(&wait, 1, 1000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) break;
        if (ready == 0) {
            if (platform_time_monotonic_us() - last_request_us >
                UI_HOST_IDLE_EXIT_US)
                break;
            continue;
        }
        int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0) continue;
        last_request_us = platform_time_monotonic_us();
        if (!ui_host_peer_allowed(client)) {
            close(client);
            continue;
        }
        pid_t worker = fork();
        if (worker == 0)
            _exit(ui_host_worker(listener, client));
        close(client);
    }
    close(listener);
    return 0;
#endif
}
