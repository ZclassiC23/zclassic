# UX program — beautiful end-to-end UX for Z23 (2026-07-27)

Owner directive: build a beautiful UX into z23 end to end, agent-built.

Two lanes, one design language:

1. **Web lane (agent-9, branch `work/ux-design-system`, worktree
   `zclassic23-ux`).** One compact hand-written design system shared by
   every server-rendered page family — explorer, ZNAM name site, store,
   blog/social — served over HTTPS and the embedded onion. Hard
   constraints: zero required client JS; no external assets or webfonts
   (offline-sovereign, Tor-friendly); every page fits the onion response
   budget; semantic HTML + keyboard nav + contrast; presentation only —
   no controller/model/route/wire changes. The future `/zcode*` routes
   inherit the same system (see `docs/work/ZCODE_PLAN.md` slice 13).
2. **Terminal lane (queued, starts when the web lane reports).** The
   typed native command registry stays the canonical agent contract
   (bounded typed JSON, `discover schema`). This lane owns the
   human-rendered terminal experience: consistent table/pill formatting
   for `status` and `ops state`, readable `discover help` output,
   error bodies that name the failed rule and the next action, sane
   TTY/color detection with a plain non-TTY fallback that agents can
   parse. No new command surface — polish what exists.

Both lanes gate on `make build-only` + mapped `make t-fast` groups +
`make lint`, land as reviewed commits on main, and push in batches.
