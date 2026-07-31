/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * property_action — pure implementation of the closed action vocabulary.
 * See metaverse/property_action.h for the contract. */

#include "metaverse/property_action.h"

#include <string.h>

static const uint32_t k_action_bits[] = {
#define METAVERSE_ACTION_BIT_ROW(id_, name_, bit_) bit_,
    METAVERSE_ACTION_TABLE(METAVERSE_ACTION_BIT_ROW)
#undef METAVERSE_ACTION_BIT_ROW
};

_Static_assert(sizeof(k_action_bits) / sizeof(k_action_bits[0]) ==
                   (size_t)METAVERSE_ACTION_COUNT,
               "METAVERSE_ACTION_COUNT must equal the table's row count: a "
               "mismatch would hide an action from every mask renderer");

const char *metaverse_action_name(uint32_t bit)
{
    /* Exactly one bit, or nothing. A multi-bit value has no single name and
     * must not be rendered as whichever action happens to match first. */
    if (bit == 0 || (bit & (bit - 1u)) != 0)
        return NULL;
#define METAVERSE_ACTION_NAME_ROW(id_, name_, bit_)                          \
    if (bit == (bit_)) return name_;
    METAVERSE_ACTION_TABLE(METAVERSE_ACTION_NAME_ROW)
#undef METAVERSE_ACTION_NAME_ROW
    return NULL;
}

uint32_t metaverse_action_from_name(const char *name)
{
    if (!name || !*name)
        return 0;
#define METAVERSE_ACTION_FROM_ROW(id_, name_, bit_)                          \
    if (strcmp(name, name_) == 0) return (bit_);
    METAVERSE_ACTION_TABLE(METAVERSE_ACTION_FROM_ROW)
#undef METAVERSE_ACTION_FROM_ROW
    return 0;
}

uint32_t metaverse_action_at(size_t i)
{
    if (i >= (size_t)METAVERSE_ACTION_COUNT)
        return 0;
    return k_action_bits[i];
}

bool metaverse_action_mask_valid(uint32_t mask)
{
    return (mask & ~(uint32_t)METAVERSE_ACTION_ALL) == 0;
}

bool metaverse_action_mask_format(uint32_t mask, char *out, size_t cap)
{
    size_t used = 0;

    if (!out || cap == 0)
        return false;
    out[0] = '\0';
    if (!metaverse_action_mask_valid(mask))
        return false;

    for (size_t i = 0; i < (size_t)METAVERSE_ACTION_COUNT; i++) {
        uint32_t bit = k_action_bits[i];
        const char *name;
        size_t nlen;
        size_t need;

        if ((mask & bit) == 0)
            continue;
        name = metaverse_action_name(bit);
        if (!name)
            return false;
        nlen = strlen(name);
        need = (used ? 1u : 0u) + nlen;
        if (used + need + 1u > cap) {
            out[0] = '\0';
            return false;
        }
        if (used)
            out[used++] = ',';
        memcpy(out + used, name, nlen);
        used += nlen;
        out[used] = '\0';
    }
    return true;
}
