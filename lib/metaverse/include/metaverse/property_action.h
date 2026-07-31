/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * property_action — the closed action vocabulary of the metaverse. Pure
 * rules: no I/O, no allocation.
 *
 * This is a SEPARATE header from property_id.h on purpose. The capability
 * /grant engine and the confined agent broker need the action names and
 * nothing else about properties; keeping the vocabulary standalone means
 * neither has to include the catalog's view type to name what it is
 * granting.
 *
 * Actions are a bitmask (uint32_t) because both sides of the system ask
 * set questions: a grant carries the set of actions it allows, and a
 * property view carries the set its CURRENT state supports. The
 * intersection is what may actually be attempted. The mask is 32-bit with
 * 13 actions defined, so there is room for the world/object verbs without
 * a wire change.
 *
 * AVAILABILITY IS NOT AUTHORITY. A view's action set answers only "does
 * the object's present state make this action meaningful" (you cannot
 * DELIVER bytes you do not hold; you cannot TRANSFER title to an object
 * that records no title). Whether a given principal MAY do it is the
 * grant engine's question, asked separately and always ANDed with this.
 * Neither side is sufficient alone.
 */

#ifndef ZCL_METAVERSE_PROPERTY_ACTION_H
#define ZCL_METAVERSE_PROPERTY_ACTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One row per action: enum suffix, wire name, bit. The bit values are a
 * WIRE CONTRACT — receipts and grants persist them — so rows are appended,
 * never renumbered or removed. */
#define METAVERSE_ACTION_TABLE(X)                                            \
    X(INSPECT,          "inspect",           0x00000001u)                    \
    X(HOST,             "host",              0x00000002u)                    \
    X(PUBLISH_REVISION, "publish_revision",  0x00000004u)                    \
    X(UPDATE_POINTER,   "update_pointer",    0x00000008u)                    \
    X(LIST,             "list",              0x00000010u)                    \
    X(BUY,              "buy",               0x00000020u)                    \
    X(SELL,             "sell",              0x00000040u)                    \
    X(DELIVER,          "deliver",           0x00000080u)                    \
    X(LEASE,            "lease",             0x00000100u)                    \
    X(TRANSFER,         "transfer",          0x00000200u)                    \
    X(ACCEPT_PAYMENT,   "accept_payment",    0x00000400u)                    \
    X(DELEGATE,         "delegate",          0x00000800u)                    \
    X(REVOKE,           "revoke",            0x00001000u)

enum {
#define METAVERSE_ACTION_BIT(id_, name_, bit_) METAVERSE_ACTION_##id_ = bit_,
    METAVERSE_ACTION_TABLE(METAVERSE_ACTION_BIT)
#undef METAVERSE_ACTION_BIT
};

/* Count of defined actions, and the union of every defined bit. A mask
 * with any bit outside METAVERSE_ACTION_ALL is malformed, not "a future
 * action" — a reader that guessed otherwise would silently permit
 * something it cannot name. */
enum { METAVERSE_ACTION_COUNT = 13 };

#define METAVERSE_ACTION_ALL                                                 \
    (METAVERSE_ACTION_INSPECT | METAVERSE_ACTION_HOST |                      \
     METAVERSE_ACTION_PUBLISH_REVISION | METAVERSE_ACTION_UPDATE_POINTER |   \
     METAVERSE_ACTION_LIST | METAVERSE_ACTION_BUY |                          \
     METAVERSE_ACTION_SELL | METAVERSE_ACTION_DELIVER |                      \
     METAVERSE_ACTION_LEASE | METAVERSE_ACTION_TRANSFER |                    \
     METAVERSE_ACTION_ACCEPT_PAYMENT | METAVERSE_ACTION_DELEGATE |           \
     METAVERSE_ACTION_REVOKE)

/* The subset that mutates something outside this node's own cache. Every
 * one of these must go through PLAN -> COMMIT -> RECEIPT; INSPECT and HOST
 * are the two that do not. Stated here so the grant engine and the broker
 * agree on which verbs need a receipt without each keeping a list. */
#define METAVERSE_ACTION_MUTATING                                            \
    (METAVERSE_ACTION_PUBLISH_REVISION | METAVERSE_ACTION_UPDATE_POINTER |   \
     METAVERSE_ACTION_LIST | METAVERSE_ACTION_BUY |                          \
     METAVERSE_ACTION_SELL | METAVERSE_ACTION_DELIVER |                      \
     METAVERSE_ACTION_LEASE | METAVERSE_ACTION_TRANSFER |                    \
     METAVERSE_ACTION_ACCEPT_PAYMENT | METAVERSE_ACTION_DELEGATE |           \
     METAVERSE_ACTION_REVOKE)

/* Wire name for exactly one action bit. NULL when `bit` is zero, has more
 * than one bit set, or names no defined action — the caller must not
 * render an unnamed bit as an action. */
const char *metaverse_action_name(uint32_t bit);

/* Single action bit for a wire name, or 0 when unknown/NULL/empty. */
uint32_t metaverse_action_from_name(const char *name);

/* Iterate the defined actions in table order: 0 <= i < COUNT yields the
 * bit, out of range yields 0. Lets a renderer walk the vocabulary without
 * repeating the list. */
uint32_t metaverse_action_at(size_t i);

/* True when every set bit of `mask` is a defined action. An empty mask is
 * well-formed (it means "no action is available"). */
bool metaverse_action_mask_valid(uint32_t mask);

/* Comma-separated wire names of the set bits, in table order, into out.
 * Writes "" for an empty mask. False (with out[0] = 0 when cap > 0) on a
 * NULL/zero-cap out, a malformed mask, or a buffer too small for the whole
 * list — a truncated action list must never be mistaken for a shorter
 * one. */
bool metaverse_action_mask_format(uint32_t mask, char *out, size_t cap);

/* Buffer size that always fits METAVERSE_ACTION_ALL. */
#define METAVERSE_ACTION_LIST_MAX 192u

#endif /* ZCL_METAVERSE_PROPERTY_ACTION_H */
