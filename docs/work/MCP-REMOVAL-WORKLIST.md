# MCP removal — DONE

The zero-MCP program is complete. The legacy MCP stdio server has been deleted:
no `-mcp` server mode, no `mcpcall` CLI subcommand, no `make agent-mcp-call*`
targets, no `tools/mcp/` tree. The native typed command registry
(`zclassic23 <command>`) is the sole agent interface; REST (`/api/v1/...`)
remains the public read-only mirror.

What survived, by design:
- The node RPC transport (`rpc_client`/`rpc_params`) was re-homed under
  `app/controllers/`.
- The agent contract registry still carries `mcp`/`mcp_tool` interface-taxonomy
  metadata (the tool-name a command maps to) — durable contract metadata, not a
  live server.
- `lib/hotswap/` keeps its generic stateless-provider (`mcp.routes`) scaffolding
  as dead-but-valid infrastructure (a separate follow-up simplification lane).

The removal history lives in git. See
[`MCP-REMOVAL-PLAN.md`](MCP-REMOVAL-PLAN.md) for the original rationale.
