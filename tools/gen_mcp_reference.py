#!/usr/bin/env python3
# Copyright 2026 Rhett Creighton - Apache License 2.0
"""
Generate MCP_REFERENCE.md from the live `build/bin/zclassic23 -mcp` tool surface.

This is invoked by `make docs-mcp`, which pipes the JSON-RPC `tools/list`
response through stdin.  Grouped by domain (ops / chain / net / wallet / app),
one entry per tool with the description, parameter table, and required-arg
list.  Stable Markdown so repeated runs produce identical diffs unless the
router surface actually changed.

Design notes:
- Pure stdlib (no pip installs required for docs generation).
- Sort tools alphabetically within each domain.  The router itself returns
  them in registration order, but alphabetical is more stable across
  reorderings and easier for humans to scan.
- Parameters render as a small GFM table.  Required params are marked with
  `**yes**` so they visually pop; optional params show their default if the
  schema declared one.
"""

import json
import sys
from typing import Any


DOMAIN_ORDER = ["ops", "chain", "net", "wallet", "app"]

DOMAIN_HEADERS = {
    "ops":    ("Ops & Observability",
               "Node health, diagnostics, metrics, and MCP introspection."),
    "chain":  ("Chain & Consensus",
               "Block/transaction lookup and consensus state."),
    "net":    ("Network & Peers",
               "P2P peer management, latency, and onion service."),
    "wallet": ("Wallet",
               "Transparent + shielded balance, keys, and transactions."),
    "app":    ("Apps (ZSLP, Names, Messaging, Market, Swaps)",
               "Higher-level apps built on the chain."),
}


def fmt_type(prop: dict[str, Any]) -> str:
    """Render the JSONSchema type + a small enum/min/max hint."""
    t = prop.get("type", "any")
    if "enum" in prop:
        enum = " \\| ".join(prop["enum"])
        return f"{t} ({enum})"
    hints = []
    if "minimum" in prop:
        hints.append(f"min {prop['minimum']}")
    if "maximum" in prop:
        hints.append(f"max {prop['maximum']}")
    if "minLength" in prop:
        hints.append(f"len ≥ {prop['minLength']}")
    if "maxLength" in prop:
        hints.append(f"len ≤ {prop['maxLength']}")
    if hints:
        return f"{t} ({', '.join(hints)})"
    return t


def render_params(schema: dict[str, Any]) -> str:
    """Render an inputSchema as a small Markdown table, or '(none)'."""
    props = schema.get("properties", {}) or {}
    required = set(schema.get("required", []) or [])
    if not props:
        return "_No parameters._"
    rows = ["| Parameter | Type | Required | Default | Description |",
            "|-----------|------|----------|---------|-------------|"]
    for name, prop in props.items():
        t = fmt_type(prop)
        req = "**yes**" if name in required else "no"
        default = prop.get("default", "")
        if isinstance(default, bool):
            default = "true" if default else "false"
        elif default == "":
            default = "—"
        desc = (prop.get("description") or "").replace("|", "\\|")
        rows.append(f"| `{name}` | {t} | {req} | `{default}` | {desc} |")
    return "\n".join(rows)


def render_tool(tool: dict[str, Any]) -> str:
    name = tool["name"]
    desc = (tool.get("description") or "").strip()
    params = render_params(tool.get("inputSchema") or {})
    return f"### `{name}`\n\n{desc}\n\n{params}\n"


def render(tools: list[dict[str, Any]]) -> str:
    by_domain: dict[str, list[dict[str, Any]]] = {d: [] for d in DOMAIN_ORDER}
    unknown: list[dict[str, Any]] = []
    for t in tools:
        d = t.get("domain", "")
        if d in by_domain:
            by_domain[d].append(t)
        else:
            unknown.append(t)

    parts: list[str] = []
    parts.append("# MCP Reference — ZClassic23\n")
    parts.append(
        "Auto-generated from `build/bin/zclassic23 -mcp` via `make docs-mcp`. "
        "Do not edit by hand; regenerate after changing the router surface.\n"
    )
    parts.append(f"**Tool count:** {len(tools)}\n")

    for d in DOMAIN_ORDER:
        entries = by_domain[d]
        if not entries:
            continue
        title, subtitle = DOMAIN_HEADERS[d]
        parts.append(f"\n## {title}\n\n{subtitle}  \n**Tools:** {len(entries)}\n")
        for t in sorted(entries, key=lambda x: x["name"]):
            parts.append(render_tool(t))

    if unknown:
        parts.append(f"\n## Other\n\n**Tools:** {len(unknown)}\n")
        for t in sorted(unknown, key=lambda x: x["name"]):
            parts.append(render_tool(t))

    return "\n".join(parts).rstrip() + "\n"


def main() -> int:
    raw = sys.stdin.read()
    if not raw.strip():
        print("gen_mcp_reference: empty stdin", file=sys.stderr)
        return 1
    try:
        doc = json.loads(raw)
    except json.JSONDecodeError as e:
        print(f"gen_mcp_reference: JSON parse failed: {e}", file=sys.stderr)
        return 1
    tools = (doc.get("result") or {}).get("tools")
    if not isinstance(tools, list):
        print("gen_mcp_reference: no result.tools array in input", file=sys.stderr)
        return 1
    sys.stdout.write(render(tools))
    return 0


if __name__ == "__main__":
    sys.exit(main())
