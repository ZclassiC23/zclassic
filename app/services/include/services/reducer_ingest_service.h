/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Reducer-ingest service — private cross-TU seam.
 *
 * The reducer-ingest path (reducer_is_authoritative / reducer_kick /
 * reducer_ingest_block and their staged-drain helpers) lives in
 * reducer_ingest_service.c; the activation FSM half lives in
 * chain_activation_service.c. The PUBLIC reducer entry points keep
 * their declarations in services/chain_activation_service.h (unchanged for
 * every caller); this header only re-exports the ONE helper the activation
 * FSM half still needs across the TU boundary:
 *
 *   reducer_drain_to_convergence() — the non-locking staged-Job drain.
 *   activation_request_connect() (the FSM half, in
 *   chain_activation_service.c) calls it directly while already holding
 *   ctl->mutex; the locking reducer_kick / reducer_ingest_block entry points
 *   (in reducer_ingest_service.c) take that same mutex and would deadlock if
 *   used there. Both TUs include this header for the shared forward decl. */

#ifndef ZCL_REDUCER_INGEST_SERVICE_H
#define ZCL_REDUCER_INGEST_SERVICE_H

/* Loop the eight stage step bodies to convergence within a bounded latency
 * budget. The CALLER must hold the chain_activation_controller mutex — this
 * helper does NOT lock. Returns the total number of stage advances. */
int reducer_drain_to_convergence(void);

/* Same staged-Job drain, but with NO latency budget: loop until a no-advance
 * pass (convergence) or the round hard cap. The CALLER must hold the
 * chain_activation_controller mutex — this helper does NOT lock. Used only by
 * the -mint-anchor tight driver (via reducer_kick_unbudgeted) so the
 * genesis..anchor fold drains back-to-back instead of in 2s slices. Returns
 * the total number of stage advances. */
int reducer_drain_to_convergence_unbudgeted(void);

#endif /* ZCL_REDUCER_INGEST_SERVICE_H */
