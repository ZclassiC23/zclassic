/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Composition root for the native command registry (contract §12).
 *
 * This file expands the declarative X-macro files under config/commands into
 * one immutable metadata table and binds the native handler pointers for this
 * build. Metadata is transport-neutral; the handler column is the single
 * explicitly injected native binding per leaf.
 *
 * Read-only checkout inspection handlers are release-safe.  Process execution,
 * watcher ownership, and generation inspection bind only in ZCL_DEV_BUILD;
 * the same entries remain honest COMPAT metadata with NULL handlers in a
 * release catalog.  This keeps one grammar without linking dev mutation code
 * into the release binary.
 */

#include "config/command_catalog.h"

#include "command/native_command.h"
#include "kernel/command_registry.h"

#include <stddef.h>

/* Each macro expands to one designated-initializer struct literal. Designated
 * initializers keep the expansion order-independent and let a leaf omit the
 * fields its shape never sets. */

#define ZCL_COMMAND_BRANCH(path_, parent_, summary_, layer_)                   \
    { .path = (path_), .parent = (parent_), .aliases = "",                     \
      .summary = (summary_), .tags = "", .input_schema = "",                   \
      .output_schema = "", .input_keys = "", .positional_keys = "",            \
      .example = "", .availability_reason = "", .compat_target = "",           \
      .layer = (layer_), .effect = ZCL_COMMAND_EFFECT_READ,                    \
      .risk = ZCL_COMMAND_RISK_READ, .scope = ZCL_COMMAND_SCOPE_LOCAL,         \
      .authority = ZCL_COMMAND_AUTH_PUBLIC,                                    \
      .availability = ZCL_COMMAND_READY, .mode = ZCL_COMMAND_MODE_BRANCH,      \
      .latency = ZCL_COMMAND_LATENCY_INSTANT, .cost = ZCL_COMMAND_COST_TINY,   \
      .confirmation = ZCL_COMMAND_CONFIRM_NONE,                                \
      .allowed_lanes = ZCL_COMMAND_LANE_LOCAL | ZCL_COMMAND_LANE_ALL_NODE,     \
      .required_capabilities = ZCL_COMMAND_CAP_NONE,                           \
      .traits = ZCL_COMMAND_TRAIT_NONE,                                        \
      .transports = ZCL_COMMAND_TRANSPORT_NATIVE, .handler = NULL },

#define ZCL_COMMAND_READY_READ(path_, parent_, aliases_, summary_, tags_,      \
                               in_, out_, in_keys_, pos_keys_, example_,       \
                               layer_, scope_, authority_, latency_, cost_,    \
                               lanes_, caps_, traits_, transports_, handler_)  \
    { .path = (path_), .parent = (parent_), .aliases = (aliases_),             \
      .summary = (summary_), .tags = (tags_), .input_schema = (in_),          \
      .output_schema = (out_), .input_keys = (in_keys_),                       \
      .positional_keys = (pos_keys_), .example = (example_),                   \
      .availability_reason = "", .compat_target = "", .layer = (layer_),       \
      .effect = ZCL_COMMAND_EFFECT_READ, .risk = ZCL_COMMAND_RISK_READ,        \
      .scope = (scope_), .authority = (authority_),                            \
      .availability = ZCL_COMMAND_READY, .mode = ZCL_COMMAND_MODE_SYNC,        \
      .latency = (latency_), .cost = (cost_),                                  \
      .confirmation = ZCL_COMMAND_CONFIRM_NONE, .allowed_lanes = (lanes_),     \
      .required_capabilities = (caps_), .traits = (traits_),                   \
      .transports = (transports_), .handler = (handler_) },

#define ZCL_COMMAND_COMPAT_READ(path_, parent_, aliases_, summary_, tags_,     \
                                in_, out_, in_keys_, pos_keys_, example_,      \
                                layer_, scope_, authority_, latency_, cost_,   \
                                lanes_, caps_, transports_, compat_)           \
    { .path = (path_), .parent = (parent_), .aliases = (aliases_),             \
      .summary = (summary_), .tags = (tags_), .input_schema = (in_),          \
      .output_schema = (out_), .input_keys = (in_keys_),                       \
      .positional_keys = (pos_keys_), .example = (example_),                   \
      .availability_reason =                                                   \
          "native adapter is not executable yet; use the compatibility "      \
          "target",                                                            \
      .compat_target = (compat_), .layer = (layer_),                          \
      .effect = ZCL_COMMAND_EFFECT_READ, .risk = ZCL_COMMAND_RISK_READ,        \
      .scope = (scope_), .authority = (authority_),                            \
      .availability = ZCL_COMMAND_COMPAT, .mode = ZCL_COMMAND_MODE_SYNC,       \
      .latency = (latency_), .cost = (cost_),                                  \
      .confirmation = ZCL_COMMAND_CONFIRM_NONE, .allowed_lanes = (lanes_),     \
      .required_capabilities = (caps_), .traits = ZCL_COMMAND_TRAIT_NONE,      \
      .transports = (transports_), .handler = NULL },

#define ZCL_COMMAND_PLANNED_READ(path_, parent_, aliases_, summary_, tags_,    \
                                 in_, out_, in_keys_, pos_keys_, example_,     \
                                 layer_, scope_, authority_, latency_, cost_,  \
                                 lanes_, caps_, reason_)                       \
    { .path = (path_), .parent = (parent_), .aliases = (aliases_),             \
      .summary = (summary_), .tags = (tags_), .input_schema = (in_),          \
      .output_schema = (out_), .input_keys = (in_keys_),                       \
      .positional_keys = (pos_keys_), .example = (example_),                   \
      .availability_reason = (reason_), .compat_target = "",                   \
      .layer = (layer_), .effect = ZCL_COMMAND_EFFECT_READ,                    \
      .risk = ZCL_COMMAND_RISK_READ, .scope = (scope_),                        \
      .authority = (authority_), .availability = ZCL_COMMAND_PLANNED,          \
      .mode = ZCL_COMMAND_MODE_SYNC, .latency = (latency_), .cost = (cost_),   \
      .confirmation = ZCL_COMMAND_CONFIRM_NONE, .allowed_lanes = (lanes_),     \
      .required_capabilities = (caps_), .traits = ZCL_COMMAND_TRAIT_NONE,      \
      .transports = ZCL_COMMAND_TRANSPORT_NATIVE, .handler = NULL },

#define ZCL_COMMAND_PLANNED_COMMAND(path_, parent_, aliases_, summary_, tags_, \
                                    in_, out_, in_keys_, pos_keys_, example_,  \
                                    layer_, effect_, risk_, scope_,            \
                                    authority_, mode_, latency_, cost_,        \
                                    confirmation_, lanes_, caps_, traits_,     \
                                    reason_)                                   \
    { .path = (path_), .parent = (parent_), .aliases = (aliases_),             \
      .summary = (summary_), .tags = (tags_), .input_schema = (in_),          \
      .output_schema = (out_), .input_keys = (in_keys_),                       \
      .positional_keys = (pos_keys_), .example = (example_),                   \
      .availability_reason = (reason_), .compat_target = "",                   \
      .layer = (layer_), .effect = (effect_), .risk = (risk_),                 \
      .scope = (scope_), .authority = (authority_),                            \
      .availability = ZCL_COMMAND_PLANNED, .mode = (mode_),                    \
      .latency = (latency_), .cost = (cost_), .confirmation = (confirmation_), \
      .allowed_lanes = (lanes_), .required_capabilities = (caps_),             \
      .traits = (traits_), .transports = ZCL_COMMAND_TRANSPORT_NATIVE,         \
      .handler = NULL },

/* A mutating leaf whose canonical native adapter is not executable yet: it is
 * COMPAT (NULL handler, points at a compatibility target) but carries the full
 * effect/risk/mode policy of the mutation it will one day own. Used by dev.def
 * for `dev.change.apply` (the cycle still shells out to Make targets). */
#define ZCL_COMMAND_COMPAT_COMMAND(path_, parent_, aliases_, summary_, tags_,  \
                                   in_, out_, in_keys_, pos_keys_, example_,   \
                                   layer_, effect_, risk_, scope_,             \
                                   authority_, mode_, latency_, cost_,         \
                                   confirmation_, lanes_, caps_, traits_,      \
                                   reason_, compat_)                           \
    { .path = (path_), .parent = (parent_), .aliases = (aliases_),             \
      .summary = (summary_), .tags = (tags_), .input_schema = (in_),          \
      .output_schema = (out_), .input_keys = (in_keys_),                       \
      .positional_keys = (pos_keys_), .example = (example_),                   \
      .availability_reason = (reason_), .compat_target = (compat_),            \
      .layer = (layer_), .effect = (effect_), .risk = (risk_),                 \
      .scope = (scope_), .authority = (authority_),                            \
      .availability = ZCL_COMMAND_COMPAT, .mode = (mode_),                     \
      .latency = (latency_), .cost = (cost_), .confirmation = (confirmation_), \
      .allowed_lanes = (lanes_), .required_capabilities = (caps_),             \
      .traits = (traits_), .transports = ZCL_COMMAND_TRANSPORT_NATIVE,         \
      .handler = NULL },

/* One declarative dev leaf, two build-specific bindings.  A dev binary owns
 * the executable handler; a release binary exposes only an honest COMPAT
 * description pointing at the dev binary. */
#ifdef ZCL_DEV_BUILD
#define ZCL_DEV_AVAILABILITY ZCL_COMMAND_READY
#define ZCL_DEV_REASON(reason_) ""
#define ZCL_DEV_COMPAT(target_) ""
#define ZCL_DEV_HANDLER(handler_) (handler_)
#else
#define ZCL_DEV_AVAILABILITY ZCL_COMMAND_COMPAT
#define ZCL_DEV_REASON(reason_) (reason_)
#define ZCL_DEV_COMPAT(target_) (target_)
#define ZCL_DEV_HANDLER(handler_) NULL
#endif

#define ZCL_COMMAND_DEV_READ(path_, parent_, aliases_, summary_, tags_,       \
                             in_, out_, in_keys_, pos_keys_, example_,        \
                             scope_, authority_, latency_, cost_, lanes_,     \
                             caps_, traits_, handler_, release_reason_,       \
                             compat_)                                         \
    { .path = (path_), .parent = (parent_), .aliases = (aliases_),             \
      .summary = (summary_), .tags = (tags_), .input_schema = (in_),          \
      .output_schema = (out_), .input_keys = (in_keys_),                       \
      .positional_keys = (pos_keys_), .example = (example_),                   \
      .availability_reason = ZCL_DEV_REASON(release_reason_),                  \
      .compat_target = ZCL_DEV_COMPAT(compat_),                                \
      .layer = ZCL_COMMAND_LAYER_DEV, .effect = ZCL_COMMAND_EFFECT_READ,       \
      .risk = ZCL_COMMAND_RISK_READ, .scope = (scope_),                        \
      .authority = (authority_), .availability = ZCL_DEV_AVAILABILITY,         \
      .mode = ZCL_COMMAND_MODE_SYNC, .latency = (latency_), .cost = (cost_),   \
      .confirmation = ZCL_COMMAND_CONFIRM_NONE, .allowed_lanes = (lanes_),     \
      .required_capabilities = (caps_), .traits = (traits_),                   \
      .transports = ZCL_COMMAND_TRANSPORT_NATIVE,                              \
      .handler = ZCL_DEV_HANDLER(handler_) },

#define ZCL_COMMAND_DEV_COMMAND(path_, parent_, aliases_, summary_, tags_,    \
                                in_, out_, in_keys_, pos_keys_, example_,     \
                                effect_, risk_, scope_, authority_, mode_,    \
                                latency_, cost_, confirmation_, lanes_, caps_,\
                                traits_, handler_, release_reason_, compat_)  \
    { .path = (path_), .parent = (parent_), .aliases = (aliases_),             \
      .summary = (summary_), .tags = (tags_), .input_schema = (in_),          \
      .output_schema = (out_), .input_keys = (in_keys_),                       \
      .positional_keys = (pos_keys_), .example = (example_),                   \
      .availability_reason = ZCL_DEV_REASON(release_reason_),                  \
      .compat_target = ZCL_DEV_COMPAT(compat_),                                \
      .layer = ZCL_COMMAND_LAYER_DEV, .effect = (effect_), .risk = (risk_),    \
      .scope = (scope_), .authority = (authority_),                            \
      .availability = ZCL_DEV_AVAILABILITY, .mode = (mode_),                   \
      .latency = (latency_), .cost = (cost_),                                  \
      .confirmation = (confirmation_), .allowed_lanes = (lanes_),              \
      .required_capabilities = (caps_), .traits = (traits_),                   \
      .transports = ZCL_COMMAND_TRANSPORT_NATIVE,                              \
      .handler = ZCL_DEV_HANDLER(handler_) },

static const struct zcl_command_spec g_catalog_commands[] = {
#include "../commands/root.def"
#include "../commands/core.def"
#include "../commands/apps.def"
#include "../commands/ops.def"
#include "../commands/dev.def"
#include "../commands/code.def"
};

#undef ZCL_COMMAND_BRANCH
#undef ZCL_COMMAND_READY_READ
#undef ZCL_COMMAND_COMPAT_READ
#undef ZCL_COMMAND_PLANNED_READ
#undef ZCL_COMMAND_PLANNED_COMMAND
#undef ZCL_COMMAND_COMPAT_COMMAND
#undef ZCL_COMMAND_DEV_READ
#undef ZCL_COMMAND_DEV_COMMAND
#undef ZCL_DEV_AVAILABILITY
#undef ZCL_DEV_REASON
#undef ZCL_DEV_COMPAT
#undef ZCL_DEV_HANDLER

static const struct zcl_command_registry g_catalog_registry = {
    .commands = g_catalog_commands,
    .count = sizeof(g_catalog_commands) / sizeof(g_catalog_commands[0]),
};

const struct zcl_command_registry *zcl_command_catalog(void)
{
    return &g_catalog_registry;
}
