# Dev workflow — add a Codex (GPT) executor tier to Claude Code

Relayed from a referenced external workflow (x.com/cjzafir, 2026), adapted to
zclassic23's own doctrine. The point: **Claude orchestrates; a cheaper GPT
executor does the heavy typing**, so you burn far fewer Claude tokens per unit
of work. This session repeatedly hit Claude API session limits during large
subagent swarms — this workflow is the direct mitigation.

> Specifics below (exact plugin path, command names, model versions) are
> relayed from a third-party post — **confirm the current plugin name and
> commands** when you set it up; treat the *pattern* as the durable part.

## Setup (developer runs these — one-time)

1. Install the Codex plugin in Claude Code:
   `/plugin marketplace add openai/codex-plugin-cc`
2. Authenticate with your ChatGPT/OpenAI credentials when prompted.
3. Prefer the strongest available Codex/GPT executor model (the post cites
   "GPT 5.5 extra-high") for implementation tasks.

## The division of labor (maps to this repo's model-tiering rule)

- **Claude (this agent) — orchestrator/reviewer:** planning, repo
  understanding, architecture decisions, task decomposition, consensus/parity
  judgment, and FINAL review. This is exactly the "Fable coordinates/plans;
  delegate implementation down-tier" rule already recorded for this project.
- **Codex/GPT — executor:** the heavy, well-scoped implementation typing,
  invoked (per the post) via `/codex:rescue`. Think of it as another worker
  tier alongside the Claude subagents used this session.

## Guardrails (non-negotiable on this codebase)

- **Do NOT blindly trust Codex output.** Inspect every diff. This codebase's
  bar is higher than "it compiled": consensus parity is inviolable, every fix
  must be copy-proven on a fixture before live, and findings get adversarially
  verified. Codex output is a draft to verify, never a merge-on-faith.
- **Same gates apply regardless of who wrote the code:** `make lint`,
  `make build-only`, the focused `make t-fast ONLY=<group>` tests, and the
  pre-push local-CI gate. New source files still need an `AGENT_IMPACT_RULE`
  mapping. Consensus/script/validation code is off-limits to speculative edits.
- **Worktree isolation** for parallel executors, exactly as the Claude
  subagent swarm did this session — each executor on its own branch, merged
  only after it's verified green.

## Long-horizon tips (from the post)

- Turn the setup into a reusable skill so it's one command to spin up.
- Pair it with explicit goals for long-running tasks.
- Clear/compact conversation history periodically (the post suggests after ~4
  compactions) to avoid context degradation.

## Why this fits zclassic23

The whole node is already built around "AI is a first-class operator" (100+
MCP tools) and this session already ran Claude-subagent swarms with model
tiering. Adding a GPT executor tier is the same shape with a cheaper, separate
token pool for the grunt work — orchestrate with Claude + MCP, implement with
Codex, verify everything through the existing gates before it touches `main`
or the live node.
