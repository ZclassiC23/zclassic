/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Internal addrman lookup contract shared by its implementation units. */

#ifndef ZCL_NET_ADDRMAN_INTERNAL_H
#define ZCL_NET_ADDRMAN_INTERNAL_H

#include "net/addrman.h"

/* Caller holds am->cs. The address index is private to addrman.c; narrow
 * helpers in sibling translation units use this entry point rather than
 * reintroducing an O(n) scan. */
struct addr_info *addrman_find_addr_locked(struct addr_man *am,
                                           const struct net_addr *addr,
                                           int *id_out);

#endif
