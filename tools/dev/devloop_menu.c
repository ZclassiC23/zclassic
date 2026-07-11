/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "devloop.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct dev_menu_node {
    const char *path;
    const char *parent;
    const char *summary;
    const char *risk;
    const char *latency;
    const char *example;
    bool mutates;
};

/* One declarative tree drives discovery.  Keep responses shallow: an LLM sees
 * only the current node and its immediate children, never a 100-command dump. */
static const struct dev_menu_node g_nodes[] = {
    { "dev", "", "native edit-to-proof-to-publish control plane",
      "read", "<10ms", "zclassic23-dev dev status", false },
    { "dev.status", "dev", "one compact current verdict and next action",
      "read", "<10ms", "zclassic23-dev dev status", false },
    { "dev.core", "dev", "immutable consensus/core boundary and guarded path",
      "read", "<10ms", "zclassic23-dev dev core boundary", false },
    { "dev.app", "dev", "Rails-like capability-scoped C application platform",
      "read", "<10ms", "zclassic23-dev dev app describe social", false },
    { "dev.change", "dev", "classify or execute a source change",
      "read", "<10ms", "zclassic23-dev dev change plan FILE", false },
    { "dev.loop", "dev", "persistent save watcher and heartbeat",
      "read", "<10ms", "zclassic23-dev dev loop watch", false },
    { "dev.test", "dev", "fast deterministic foreground proofs",
      "read", "<10ms", "zclassic23-dev dev test sim", false },
    { "dev.generation", "dev", "resident and immutable generation state",
      "read", "<10ms", "zclassic23-dev dev generation current", false },
    { "dev.diagnose", "dev", "failure capsule lookup and command search",
      "read", "<10ms", "zclassic23-dev dev diagnose latest", false },
    { "dev.help", "dev", "describe one tree path and its children",
      "read", "<10ms", "zclassic23-dev dev help dev.change", false },
    { "dev.search", "dev", "rank a few command paths by intent text",
      "read", "<10ms", "zclassic23-dev dev search 'ABI mismatch'", false },

    { "dev.core.boundary", "dev.core",
      "show what only Core may own and why it always reloads",
      "read", "<10ms", "zclassic23-dev dev core boundary", false },
    { "dev.core.proof", "dev.core",
      "show mandatory consensus parity and real-history proof lanes",
      "read", "<10ms", "zclassic23-dev dev core proof", false },

    { "dev.app.describe", "dev.app",
      "describe an app manifest, capabilities, bindings, and simulations",
      "read", "<10ms", "zclassic23-dev dev app describe social", false },
    { "dev.app.plan", "dev.app",
      "plan one conventional model-to-web/onion/ZNAM feature slice",
      "read", "<10ms", "zclassic23-dev dev app plan social posts", false },
    { "dev.app.simulate", "dev.app",
      "run an app's deterministic multi-node scenarios",
      "hermetic", "scenario-dependent",
      "zclassic23-dev dev app simulate social", false },
    { "dev.app.social", "dev.app",
      "reference censorship-resistant signed-event social application",
      "read", "<10ms", "zclassic23-dev dev app describe social", false },
    { "dev.app.social.resources", "dev.app.social",
      "profiles, posts, follows, and locally projected feeds",
      "read", "<10ms", "zclassic23-dev dev app plan social posts", false },
    { "dev.app.social.web", "dev.app.social",
      "one resource surface rendered as HTML and compact JSON",
      "app-only", "hot <=1s target", "zclassic23-dev dev app describe social", false },
    { "dev.app.social.onion", "dev.app.social",
      "bind the same web surface to the node's embedded onion service",
      "app-only", "hot <=1s target", "zclassic23-dev dev app describe social", false },
    { "dev.app.social.znam", "dev.app.social",
      "bind a human ZNAM name to onion, clearnet, or content identity",
      "app-only", "hot <=1s target", "zclassic23-dev dev app describe social", false },
    { "dev.app.social.protocol", "dev.app.social",
      "signed content-addressed events on permissionless P2P topics",
      "app-only", "hot <=1s target", "zclassic23-dev dev app describe social", false },
    { "dev.app.social.storage", "dev.app.social",
      "resident app state and rebuildable local projections, never consensus state",
      "app-only", "hot <=1s target", "zclassic23-dev dev app describe social", false },
    { "dev.app.social.sim", "dev.app.social",
      "prove censorship bypass, partition convergence, signatures, and replay",
      "hermetic", "millisecond target", "zclassic23-dev dev app simulate social", false },

    { "dev.change.plan", "dev.change",
      "classify files and name the smallest safe proof without acting",
      "read", "<10ms", "zclassic23-dev dev change plan app/x.c", false },
    { "dev.change.cycle", "dev.change",
      "run one classify/prove/hot-swap-or-reload transaction",
      "dev-only", "hot <=1s; reload <=8s target",
      "zclassic23-dev dev change cycle app/x.c", true },

    { "dev.loop.watch", "dev.loop",
      "coalesce saves and run native cycles continuously",
      "dev-only", "persistent", "zclassic23-dev dev loop watch", true },
    { "dev.loop.heartbeat", "dev.loop",
      "read watcher PID, source epoch, and latest outcome",
      "read", "<10ms", "zclassic23-dev dev loop heartbeat", false },

    { "dev.test.sim", "dev.test",
      "run the mandatory deterministic hot-swap network proof",
      "hermetic", "~20ms runner; ~1ms core",
      "zclassic23-dev dev test sim", false },
    { "dev.test.focused", "dev.test",
      "run one exact prebuilt focused test group",
      "hermetic", "group-dependent",
      "zclassic23-dev dev test focused hotswap_simnet", false },

    { "dev.generation.current", "dev.generation",
      "show running, current, last-good, and resident hot-swap generation",
      "read", "<10ms", "zclassic23-dev dev generation current", false },
    { "dev.generation.history", "dev.generation",
      "show accepted and rejected generation provenance",
      "read", "<10ms", "zclassic23-dev dev generation history", false },
    { "dev.generation.rollback", "dev.generation",
      "restore the last verified immutable dev generation",
      "dev-destructive", "<=8s target",
      "zclassic23-dev dev generation rollback", true },

    { "dev.diagnose.latest", "dev.diagnose",
      "show only the latest failure capsule and executable next action",
      "read", "<10ms", "zclassic23-dev dev diagnose latest", false },
    { "dev.diagnose.search", "dev.diagnose",
      "search the command tree by symptom or intent",
      "read", "<10ms", "zclassic23-dev dev diagnose search timeout", false },
};

static bool appendf(char *out, size_t cap, size_t *pos,
                    const char *fmt, ...)
{
    if (!out || !pos || *pos >= cap)
        return false;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *pos, cap - *pos, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *pos)
        return false;
    *pos += (size_t)n;
    return true;
}

static bool append_string(char *out, size_t cap, size_t *pos,
                          const char *s)
{
    if (!appendf(out, cap, pos, "\""))
        return false;
    for (const unsigned char *p = (const unsigned char *)(s ? s : "");
         *p; p++) {
        if (*p == '"' || *p == '\\') {
            if (!appendf(out, cap, pos, "\\%c", *p))
                return false;
        } else if (*p < 0x20) {
            if (!appendf(out, cap, pos, "\\u%04x", *p))
                return false;
        } else if (!appendf(out, cap, pos, "%c", *p)) {
            return false;
        }
    }
    return appendf(out, cap, pos, "\"");
}

static const struct dev_menu_node *find_node(const char *path)
{
    const char *wanted = (path && path[0]) ? path : "dev";
    for (size_t i = 0; i < sizeof(g_nodes) / sizeof(g_nodes[0]); i++) {
        if (strcmp(g_nodes[i].path, wanted) == 0)
            return &g_nodes[i];
    }
    return NULL;
}

static bool append_node_summary(char *out, size_t cap, size_t *pos,
                                const struct dev_menu_node *node)
{
    return appendf(out, cap, pos, "{\"path\":") &&
           append_string(out, cap, pos, node->path) &&
           appendf(out, cap, pos, ",\"summary\":") &&
           append_string(out, cap, pos, node->summary) &&
           appendf(out, cap, pos, ",\"risk\":") &&
           append_string(out, cap, pos, node->risk) &&
           appendf(out, cap, pos, ",\"mutates\":%s,\"latency\":",
                   node->mutates ? "true" : "false") &&
           append_string(out, cap, pos, node->latency) &&
           appendf(out, cap, pos, "}");
}

size_t zcl_devloop_menu_json(const char *path, char *out, size_t out_sz)
{
    size_t pos = 0;
    const struct dev_menu_node *node = find_node(path);
    if (!out || out_sz == 0)
        return 0;
    if (!node) {
        if (!appendf(out, out_sz, &pos,
                     "{\"schema\":\"zcl.dev_menu.v1\",\"error\":\"unknown_path\",\"path\":") ||
            !append_string(out, out_sz, &pos, path ? path : "") ||
            !appendf(out, out_sz, &pos,
                     ",\"agent_next_action\":\"zclassic23-dev dev search <intent>\"}"))
            return 0;
        return pos;
    }

    if (!appendf(out, out_sz, &pos,
                 "{\"schema\":\"zcl.dev_menu.v1\",\"path\":") ||
        !append_string(out, out_sz, &pos, node->path) ||
        !appendf(out, out_sz, &pos, ",\"summary\":") ||
        !append_string(out, out_sz, &pos, node->summary) ||
        !appendf(out, out_sz, &pos, ",\"risk\":") ||
        !append_string(out, out_sz, &pos, node->risk) ||
        !appendf(out, out_sz, &pos,
                 ",\"mutates\":%s,\"latency\":",
                 node->mutates ? "true" : "false") ||
        !append_string(out, out_sz, &pos, node->latency) ||
        !appendf(out, out_sz, &pos, ",\"example\":") ||
        !append_string(out, out_sz, &pos, node->example) ||
        !appendf(out, out_sz, &pos, ",\"children\":["))
        return 0;

    bool first = true;
    for (size_t i = 0; i < sizeof(g_nodes) / sizeof(g_nodes[0]); i++) {
        if (strcmp(g_nodes[i].parent, node->path) != 0)
            continue;
        if ((!first && !appendf(out, out_sz, &pos, ",")) ||
            !append_node_summary(out, out_sz, &pos, &g_nodes[i]))
            return 0;
        first = false;
    }
    if (!appendf(out, out_sz, &pos,
                 "],\"agent_next_action\":\"descend one branch or keep editing; watch mode is automatic\"}"))
        return 0;
    return pos;
}

static bool contains_folded(const char *haystack, const char *needle)
{
    if (!needle || !needle[0])
        return true;
    if (!haystack)
        return false;
    size_t nlen = strlen(needle);
    for (const char *h = haystack; *h; h++) {
        size_t i = 0;
        while (i < nlen && h[i] &&
               tolower((unsigned char)h[i]) ==
               tolower((unsigned char)needle[i]))
            i++;
        if (i == nlen)
            return true;
    }
    return false;
}

size_t zcl_devloop_menu_search_json(const char *query,
                                    char *out, size_t out_sz)
{
    size_t pos = 0;
    if (!out || out_sz == 0 || !query || !query[0])
        return 0;
    if (!appendf(out, out_sz, &pos,
                 "{\"schema\":\"zcl.dev_menu_search.v1\",\"query\":") ||
        !append_string(out, out_sz, &pos, query) ||
        !appendf(out, out_sz, &pos, ",\"matches\":["))
        return 0;

    size_t matches = 0;
    for (size_t i = 1; i < sizeof(g_nodes) / sizeof(g_nodes[0]) &&
                       matches < 8; i++) {
        const struct dev_menu_node *node = &g_nodes[i];
        if (!contains_folded(node->path, query) &&
            !contains_folded(node->summary, query) &&
            !contains_folded(node->risk, query))
            continue;
        if ((matches && !appendf(out, out_sz, &pos, ",")) ||
            !append_node_summary(out, out_sz, &pos, node))
            return 0;
        matches++;
    }
    if (!appendf(out, out_sz, &pos,
                 "],\"count\":%zu,\"agent_next_action\":", matches) ||
        !append_string(out, out_sz, &pos,
                       matches ? "run dev help <matched-path>"
                               : "run dev to inspect root branches") ||
        !appendf(out, out_sz, &pos, "}"))
        return 0;
    return pos;
}
