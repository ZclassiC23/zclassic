/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tor integration for zclassic23.
 *
 * Architecture: Tor is compiled into zclassic23 as a thread (no external
 * binary). Our forked Tor (RhettCreighton/tor, dynhost branch) routes
 * .onion requests directly into our process via C function calls.
 *
 * Dynhost replaces SOCKS for zclassic23 traffic. Requests arrive as C
 * callbacks, not through a SOCKS proxy or app-owned TCP listener.
 *
 * Current Tor bootstrap workaround: torrc opens a localhost-only SocksPort
 * that nothing in zclassic23 connects to. It exists only because the embedded
 * Tor fork currently refuses to bootstrap with no listener. Once dynhost can
 * satisfy that bootstrap check directly, torrc should move to SocksPort 0.
 *
 * Usage:
 *   tor_integration_set_handler(my_handler, my_ctx);
 *   tor_integration_start(datadir, p2p_port);
 *   // .onion address printed to log
 *   tor_integration_stop(); */

#ifndef ZCL_NET_TOR_INTEGRATION_H
#define ZCL_NET_TOR_INTEGRATION_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Request handler callback — dynhost calls this directly.
 * method: HTTP method (GET, POST, etc.)
 * path: URL path (e.g., "/", "/store/product/1")
 * request_data: raw request body (NULL for GET)
 * request_len: body length
 * response: output buffer (caller allocates)
 * response_max: max response size
 * Returns bytes written to response, or 0 for 404. */
typedef size_t (*tor_request_handler_fn)(const char *method,
                                          const char *path,
                                          const uint8_t *request_data,
                                          size_t request_len,
                                          uint8_t *response,
                                          size_t response_max,
                                          void *ctx);

/* Set the request handler before starting Tor. */
void tor_integration_set_handler(tor_request_handler_fn handler, void *ctx);

/* Start embedded Tor with dynhost. Creates .onion address.
 * zclassic23 traffic is handled via C callbacks; see tor_write_torrc() for
 * the temporary localhost-only Tor bootstrap listener. */
bool tor_integration_start(const char *datadir, uint16_t p2p_port);

/* Stop Tor. */
void tor_integration_stop(void);

/* Get .onion address (NULL if not ready). */
const char *tor_integration_get_onion_address(void);

/* Check if Tor is bootstrapped and .onion is reachable. */
bool tor_integration_is_ready(void);

/* Check if Tor was started (may still be bootstrapping). */
bool tor_integration_is_enabled(void);

/* Write torrc to datadir. We do NOT use SOCKS — dynhost handles
 * everything. A localhost-only SocksPort is opened as a bootstrap
 * workaround (Tor refuses to start without a listener). The port is
 * derived from p2p_port so multiple instances don't collide.
 * Exposed for testing — normally called by tor_integration_start(). */
bool tor_write_torrc(const char *datadir, uint16_t p2p_port);

/* ── Outbound .onion fetch API ─────────────────────────────── */

/* Callback for onion fetch results. Invoked from Tor's thread —
 * the caller must use atomic flags or mutexes for thread safety. */
typedef void (*tor_fetch_callback_fn)(int status,
                                       const uint8_t *body,
                                       size_t body_len,
                                       void *ctx);

/* Fetch a URL from a .onion address via embedded Tor circuits.
 * No SOCKS — uses dynhost's internal circuit management.
 * Thread-safe: queues the request for Tor's event loop.
 * Returns 0 if queued, -1 if Tor not initialized. */
int tor_integration_fetch_onion(const char *onion_address,
                                 const char *path,
                                 tor_fetch_callback_fn callback,
                                 void *ctx,
                                 int timeout_secs);

/* Thread-safe result structure for blocking callers. */
struct onion_fetch_result {
    _Atomic int complete;   /* 0=pending, 1=done, -1=error */
    int status;
    uint8_t *body;          /* caller must free() */
    size_t body_len;
};

/* Helper: blocking fetch with timeout. Allocates and copies body.
 * Returns 0 on success, -1 on error/timeout. */
int tor_integration_fetch_onion_blocking(const char *onion_address,
                                          const char *path,
                                          struct onion_fetch_result *result,
                                          int timeout_secs);

#endif
