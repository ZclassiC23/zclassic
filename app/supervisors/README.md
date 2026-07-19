# app/supervisors/

**Shape:** Supervisor — declared liveness tree with restart/stall policy.

Each source file in `src/` owns one supervisor domain or one narrow liveness
registration surface. Supervisors register `struct liveness_contract` children
through `lib/util/supervisor.h`, grouped by domain in `domains.c`.

Current roots include network, chain, staged-sync, legacy-mirror, and
self-heal/condition-engine liveness. Boot still wires lifecycle dependencies,
so this shape is partial; do not add placeholder roots or macro-only scaffold.
A supervisor file should make a running child visible through the root
liveness tree or it should not exist.

See [`docs/FRAMEWORK.md`](../../docs/FRAMEWORK.md) for the destination shape
and its §9 debt board for remaining supervisor cleanup.
