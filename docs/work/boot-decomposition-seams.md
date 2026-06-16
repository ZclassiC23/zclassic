# Boot decomposition — seam plan (the `S`-into-handle route)

The boot decomposition and the `S`-into-handle seam redesign **LANDED** (units
extracted to `boot_frontend_services.c`, `boot_msg_callbacks.c`,
`boot_background_workers.c`, `boot_sd_watchdog.c`, `boot_runtime_sync_services.c`;
accessors now take `svc`, e.g. `boot_node_db(struct boot_svc_ctx *svc)`). Live
status board: `docs/REFACTOR_STATUS.md` Rank 1.

## Open deferred item — boot_projections.c descriptor table

`config/src/boot_projections.c` still repeats a 7× inline block (set_event_log →
open → NULL-check → catch_up → `UINT64_MAX` branch → fprintf) for mempool, peers,
znam, wallet, contacts, onion_ann, hodl_history. A real dedup is a typed
`{name, open_fn, set_event_log_fn, catch_up_fn}` projection-descriptor table —
a NEW surface, not an inline helper (each block calls differently-named
per-type functions with no shared vtable, plus call-site-specific
`obs-ok:phase4-storage` diagnostics). This behavior-sensitive refactor must be
its own boot-validated pass on a datadir copy, not churn on the extraction.

Per-pass checklist:

- `config/src/*.c` is wildcard-built (no Makefile edit).
- Add the new file to the scaffold-label `files[]` at
  `lib/test/src/test_make_lint_gates.c:2252`.
- Boot-validate on a copy: `tools/repro_on_copy.sh <tag> --port=18299
  --p2p-port=18933 --deadline=480 -- -nobgvalidation` (deadline ≥480 to clear
  the ~328s `mmb_register` boot cost). Confirm tip restores, no crash.
