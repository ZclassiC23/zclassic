/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Per-node community content moderation for marketplace listings.
 *
 * Three layers stay separate:
 *   1. Protocol validity — the signed offer wire. Never filtered here.
 *   2. Local hosting policy — the node still stores, serves, and trades
 *      every valid offer it ingested. Moderation never deletes.
 *   3. View filtering — this module: each node applies its OWN local
 *      listing-visibility policy to its own listing surfaces
 *      (zmarket_list / app market list / GET /api/market).
 *
 * There are no network-wide bans and no deletion authority anywhere:
 * a profile only decides which locally-known offers the LOCAL listing
 * view shows. The active profile persists per datadir at
 * market/moderation.v1 (mode 0600, atomic rename) and defaults to the
 * immutable named profile general-audience.v1. The named opt-in
 * profile open-view shows everything ingested. Profiles are immutable
 * and named — operators pick one; they never edit one.
 *
 * review_state is local-only metadata on file_offers rows
 * (unreviewed / reviewed_ok / sensitive). It is set by the node's own
 * curation action (zmarket_review_set), never gossiped, and never part
 * of the signed wire. */

#ifndef ZCL_SERVICES_MARKET_MODERATION_SERVICE_H
#define ZCL_SERVICES_MARKET_MODERATION_SERVICE_H

#include "base/result.h"
#include "models/review_state.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MARKET_MODERATION_POLICY_FILE "market/moderation.v1"
#define MARKET_MODERATION_PROFILE_GENERAL_AUDIENCE_V1 "general-audience.v1"
#define MARKET_MODERATION_PROFILE_OPEN_VIEW "open-view"

enum market_moderation_profile {
    MARKET_MODERATION_PROFILE_DEFAULT = 0, /* general-audience.v1 */
    MARKET_MODERATION_PROFILE_OPEN = 1,
    MARKET_MODERATION_PROFILE_COUNT = 2
};

const char *market_moderation_profile_string(
    enum market_moderation_profile profile);
/* -1 when the name is not a known immutable profile. */
int market_moderation_profile_from_string(const char *name);

static inline bool market_moderation_profile_valid(int profile)
{
    return profile >= 0 && profile < MARKET_MODERATION_PROFILE_COUNT;
}

/* Per-datadir policy persistence. Load never creates directories and
 * answers the boot default (general-audience.v1) when the file is
 * absent; it fails (ok_out=false) on unreadable/corrupt content so an
 * operator edit cannot silently widen the view. Save writes 0600 via
 * tmp+rename and fsyncs the directory. */
enum market_moderation_profile market_moderation_profile_load(
    const char *datadir, bool *ok_out, char *error, size_t error_capacity);
struct zcl_result market_moderation_profile_save(
    const char *datadir, enum market_moderation_profile profile);

struct node_db;

/* Node-process context: registered once at boot (rpc_market_set_state)
 * so the listing filter, the status surface, and the dumpstate dumper
 * share the live profile and the review-state store. */
void market_moderation_set_context(struct node_db *ndb,
                                   const char *datadir);
enum market_moderation_profile market_moderation_active_profile(void);
/* Set + persist the active profile. */
struct zcl_result market_moderation_set_active_profile(
    enum market_moderation_profile profile);

/* Review-state store access through the context db. get answers
 * MARKET_REVIEW_UNREVIEWED when the db is absent or the offer row is
 * missing (an offer known only to the gossip cache is unreviewed). */
int market_moderation_review_state_for_root(const uint8_t root_hash[32]);
struct zcl_result market_moderation_review_counts(
    int64_t counts[MARKET_REVIEW_STATE_COUNT]);
/* The node's own curation action: mark one signed offer by offer_id.
 * Fails when the state is invalid or no signed offer carries that id. */
struct zcl_result market_moderation_set_review_state(
    const uint8_t offer_id[32], enum market_review_state state);

struct json_value;
/* See AGENTS.md "Adding state introspection". Reentrant-safe. */
bool market_moderation_dump_state_json(struct json_value *out,
                                       const char *key);

#endif /* ZCL_SERVICES_MARKET_MODERATION_SERVICE_H */
