/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * The hardcoded clearnet file-service seed set — ONE shared list so
 * config/src/boot_bundle_fetch.c (bbf_assemble_seeds, the instant-on bundle
 * discovery/fetch path) and config/src/boot_services.c (the legacy
 * -allow-clearnet-snapshot-fetch probe loop) can never drift apart. Both
 * ports default to FS_PORT (net/file_service.h).
 *
 * Trust note: these seeds are unauthenticated (clearnet, no TLS, no
 * ZClassic state commitment) — an unreachable, hostile, or simply
 * operator-unknown seed can only waste a fetch attempt, never forge
 * accepted state. Every byte served through either caller is content-
 * verified against a committed manifest (per-chunk or whole-file SHA3)
 * before landing, and the sovereign install path independently re-checks
 * transparent/shielded state against the compiled checkpoint — a seed's
 * identity is not part of the trust boundary. See
 * docs/CONSENSUS_PARITY_DOCTRINE.md's "validate against the CHAIN, not the
 * reference text" doctrine for the same shape of reasoning applied to
 * content instead of network identity.
 *
 * Per-seed provenance:
 *   - 205.209.104.118, 140.174.189.3: pre-existing seeds (operator of the
 *     latter is unknown to this project). Owner decision 2026-07-24: KEEP
 *     both under the trust note above — "best with what we have."
 *   - 74.50.74.102: this project's own host, added 2026-07-24 after a
 *     LOCAL reachability probe: confirmed bound on a local interface
 *     (`ip addr`) and a real file_service fs_handshake nonce exchange
 *     answered on FS_PORT from this host. A local probe proves bind +
 *     local-route ONLY, not external firewall/NAT traversal from a remote
 *     peer — that verification is a follow-up, not established here.
 *
 * Do not add a new seed IP without the same explicit owner review — this
 * file is the single place that decision is recorded. */

#ifndef ZCL_CONFIG_BUNDLE_FETCH_SEEDS_H
#define ZCL_CONFIG_BUNDLE_FETCH_SEEDS_H

/* NULL-terminated so callers loop `for (i = 0; arr[i]; i++)` with no
 * separate count constant to keep in sync. */
static const char *const ZCL_BUNDLE_FETCH_CLEARNET_SEEDS[] = {
    "205.209.104.118",
    "140.174.189.3",
    "74.50.74.102",
    NULL,
};

#endif /* ZCL_CONFIG_BUNDLE_FETCH_SEEDS_H */
