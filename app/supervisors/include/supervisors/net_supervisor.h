/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Net domain supervisor children — declarative liveness registration.
 *
 * Owns the net.outbound_floor child: a Round-5 C3 contract that snapshots
 * the outbound-healthy peer count every 15 s and, after 60 s under the
 * floor (2), emits EV_PEER_FLOOR_BREACH + re-kicks seed discovery so the
 * connman outbound thread has fresh targets. Registered in the `net`
 * domain (g_net_sup). */

#ifndef ZCL_NET_SUPERVISOR_H
#define ZCL_NET_SUPERVISOR_H

struct connman;

/* Register the net-domain supervisor children. Idempotent — a second
 * call is a no-op. `cm` is the live connman the peer-floor child reads. */
void net_supervisor_register(struct connman *cm);

#endif /* ZCL_NET_SUPERVISOR_H */
