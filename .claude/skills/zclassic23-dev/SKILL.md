---
name: zclassic23-dev
description: Use when developing on the ZClassic23 codebase (this repo) — onboarding, understanding the architecture, or making any code change. Covers the verify-only native dev loop, runtime-publication containment, typed-commands-over-bash, workflows of tiered subagents, the push traps, the node's state-machine model, the eight code shapes / where things live, the inviolable rules (consensus parity, copy-prove before live, defensive-coding gates), build/test/deploy, and the don't-re-chase traps. Invoke for "how does this codebase work", "how do I develop efficiently here", "how do I add or change X here", "be a zclassic23 developer", or before editing zclassic23 source.
---

The developer operating manual is [`docs/DEVELOPING.md`](../../../docs/DEVELOPING.md).
It lives in `docs/` so that every agent and every documentation-accuracy gate sees
it, not only Claude Code. This file holds the skill frontmatter and nothing else —
do not copy manual content here; edit `docs/DEVELOPING.md`.

The import below inlines it. If it did not expand, read `docs/DEVELOPING.md`.

@docs/DEVELOPING.md
