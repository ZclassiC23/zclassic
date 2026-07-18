#!/usr/bin/env python3
"""debug_bundle_triage.py — one-screen triage for a zcl.debug_bundle.v1 JSON.

`ops debug bundle` (or the supervisor-stall auto-capture) writes a 100+KB
JSON with every registered state dumper's body; this script extracts the
handful of fields that answer "why is the node wedged?" in seconds:

  - header: captured_at_utc, trigger (+ stall child/reason), build identity
  - H* / served floor / gap vs network tip      (subsystems.reducer_frontier)
  - coins_best vs H*                            (subsystems.reducer_frontier)
  - first H*+1 blocker + repair owner           (subsystems.reducer_frontier)
  - active named blockers                       (subsystems.blocker)
  - stalled/fired supervisor children           (supervisor_stalls)
  - sovereignty posture, when present           (subsystems.sovereignty)
  - a one-line "likely story", only when trivially derivable

argv[1] is a bundle file, or a directory containing debug-bundle-*.json
(newest by mtime wins). Read-only; never touches the datadir otherwise.
Exit 0 on success, 1 on a malformed/unreadable bundle, 2 on usage.
"""

import json
import sys
from pathlib import Path

BUNDLE_FORMAT = "zcl.debug_bundle.v1"
BUNDLE_GLOB = "debug-bundle-*.json"
MAX_BLOCKERS_SHOWN = 5


def die(msg: str) -> int:
    print(f"REFUSE: {msg}", file=sys.stderr)
    return 1


def pick_bundle(path_str: str) -> Path | None:
    """Resolve argv[1] to a bundle file; dir -> newest debug-bundle-*.json."""
    p = Path(path_str)
    if p.is_dir():
        candidates = [c for c in p.glob(BUNDLE_GLOB) if c.is_file()]
        if not candidates:
            return None
        return max(candidates, key=lambda c: (c.stat().st_mtime, c.name))
    return p if p.is_file() else None


def load_bundle(path: Path) -> dict:
    try:
        with open(path, "r", encoding="utf-8") as f:
            doc = json.load(f)
    except OSError as e:
        raise ValueError(f"cannot read {path}: {e}") from e
    except json.JSONDecodeError as e:
        raise ValueError(f"{path} is not valid JSON: {e}") from e
    if not isinstance(doc, dict):
        raise ValueError(f"{path}: top level is not a JSON object")
    fmt = doc.get("format")
    if fmt != BUNDLE_FORMAT:
        raise ValueError(
            f"{path}: format is {fmt!r}, expected {BUNDLE_FORMAT!r} — "
            "not a debug bundle (or a newer schema this tool predates)")
    subs = doc.get("subsystems")
    if not isinstance(subs, dict):
        raise ValueError(f"{path}: missing object key 'subsystems'")
    return doc


def short(s, n=12):
    """Short id for hashes; '<missing>' for absent values."""
    if not isinstance(s, str) or not s:
        return "<missing>"
    return s[:n]


def age_s(us) -> str:
    """Microseconds -> compact human age."""
    if not isinstance(us, (int, float)) or us < 0:
        return "?"
    secs = us / 1_000_000
    if secs < 90:
        return f"{secs:.1f}s"
    if secs < 5400:
        return f"{secs / 60:.1f}m"
    return f"{secs / 3600:.1f}h"


def get(d: dict, *keys, default=None):
    """Nested dict walk; any miss returns default."""
    cur = d
    for k in keys:
        if not isinstance(cur, dict):
            return default
        cur = cur.get(k)
        if cur is None:
            return default
    return cur


def h(v) -> str:
    """Height formatting: -1/absent -> '-'."""
    return str(v) if isinstance(v, int) and v >= 0 else "-"


def jb(v) -> str:
    """JSON-style bool; non-bools pass through as '?'."""
    return str(v).lower() if isinstance(v, bool) else "?"


def subsys_error(body) -> str | None:
    """A dumper that failed in the writer degrades to {"error": ...}."""
    if isinstance(body, dict) and "error" in body:
        return str(body["error"])
    return None


def likely_story(rf: dict, blockers: dict) -> str | None:
    """One-line heuristic — only fire on patterns that need no judgement."""
    if not isinstance(rf, dict):
        return None
    hstar = rf.get("hstar", -1)
    coins_best = rf.get("coins_best_height", -1)
    active = blockers.get("active_count", 0) if isinstance(blockers, dict) else 0
    pending = rf.get("hstar_next_pending_edge") is True
    pending_stage = rf.get("hstar_next_pending_stage", "")
    blocked = rf.get("hstar_next_blocked") is True

    if (active == 0 and pending and isinstance(coins_best, int)
            and isinstance(hstar, int) and coins_best > hstar):
        return (f"0 blockers + {pending_stage or 'tip_finalize'} edge pending "
                "+ coins_best > H* -> upstream starvation; check utxo_apply "
                "(`dumpstate reducer_frontier`)")
    if blocked:
        stage = rf.get("hstar_next_primary_stage", "?")
        kind = rf.get("hstar_next_primary_kind", "?")
        owner = rf.get("hstar_next_primary_repair_owner", "")
        return (f"H*+1 blocked at {stage} ({kind}); repair owner: "
                f"{owner or 'unknown'}")
    return None


def render(path: Path, doc: dict) -> list[str]:
    out = []
    subs = doc["subsystems"]

    # ── header ───────────────────────────────────────────────────────
    trig = doc.get("trigger", "manual")
    trig_extra = ""
    if doc.get("trigger_child"):
        trig_extra = (f" child={doc['trigger_child']}"
                      f" reason={doc.get('trigger_stall_reason', '?')}")
    build = doc.get("build") if isinstance(doc.get("build"), dict) else {}
    out.append(f"bundle {path.name}")
    out.append(f"  captured {doc.get('captured_at_utc', '?')}  "
               f"trigger={trig}{trig_extra}")
    out.append(f"  build v{build.get('version', '?')} "
               f"commit={short(build.get('build_commit'))} "
               f"src={short(build.get('source_id_sha256'))}")

    # ── frontier (H*/floor/gap + H*+1 blocker) ───────────────────────
    rf = subs.get("reducer_frontier")
    err = subsys_error(rf)
    out.append("== frontier ==")
    if err:
        out.append(f"  reducer_frontier dump failed: {err}")
        rf = {}
    if not isinstance(rf, dict) or not rf:
        out.append("  reducer_frontier: absent")
        rf = {}
    if rf:
        hstar = rf.get("hstar", -1)
        floor = rf.get("served_floor", -1)
        gap = rf.get("served_gap", 0)
        if rf.get("network_tip_read_ok"):
            tip = (f"network_tip={h(rf.get('network_tip'))} "
                   f"tail_gap={rf.get('hstar_to_network_tip_gap', 0)}")
        else:
            tip = "network_tip=- (no peer height)"
        out.append(f"  H*={h(hstar)} served_floor={h(floor)} "
                   f"gap={gap} provable_tip={h(rf.get('cached_provable_tip'))}"
                   f"  {tip}")
        if rf.get("hstar_next_blocked"):
            out.append(f"  H*+1 BLOCKED stage="
                       f"{rf.get('hstar_next_primary_stage', '?')} "
                       f"kind={rf.get('hstar_next_primary_kind', '?')} "
                       f"detail={rf.get('hstar_next_primary_detail', '')}")
            owner = rf.get("hstar_next_primary_repair_owner", "")
            if owner:
                out.append(f"    repair_owner: {owner}")
        elif rf.get("hstar_next_pending_edge"):
            out.append(f"  H*+1 pending edge stage="
                       f"{rf.get('hstar_next_pending_stage', '?')} "
                       f"({rf.get('hstar_next_pending_detail', '')})")
        else:
            out.append("  H*+1: no blocker")

        # ── coins vs H* ──────────────────────────────────────────────
        coins_best = rf.get("coins_best_height", -1)
        if not isinstance(coins_best, int) or coins_best < 0:
            cover = "coins_applied_height absent (fresh datadir)"
        elif rf.get("coins_best_above_hstar") is True:
            cover = "coins AHEAD of H*"
        elif isinstance(hstar, int) and coins_best >= hstar - 1:
            cover = "coins cover H*"
        else:
            cover = "coins BEHIND H*"
        out.append(f"  coins_best={h(coins_best)} vs H*={h(hstar)}  ({cover})")

    # ── named blockers ───────────────────────────────────────────────
    blk = subs.get("blocker")
    err = subsys_error(blk)
    out.append("== blockers ==")
    if err:
        out.append(f"  blocker dump failed: {err}")
        blk = {}
    if not isinstance(blk, dict):
        blk = {}
    entries = blk.get("blockers") if isinstance(blk.get("blockers"), list) \
        else []
    active = blk.get("active_count", len(entries))
    if active == 0 and not entries:
        out.append("  none active")
    else:
        out.append(f"  active={active} permanent={blk.get('permanent_count', 0)}"
                   f" dependency={blk.get('dependency_count', 0)}"
                   f" transient={blk.get('transient_count', 0)}"
                   f" resource={blk.get('resource_count', 0)}")
        for e in entries[:MAX_BLOCKERS_SHOWN]:
            if not isinstance(e, dict):
                continue
            reason = str(e.get("reason", ""))
            if len(reason) > 72:
                reason = reason[:69] + "..."
            out.append(f"  - {e.get('id', '?')} owner={e.get('owner', '?')} "
                       f"class={e.get('class', '?')} age={age_s(e.get('age_us'))}"
                       f" fires={e.get('fire_count', 0)}")
            out.append(f"      reason: {reason}")
        if len(entries) > MAX_BLOCKERS_SHOWN:
            out.append(f"  ... and {len(entries) - MAX_BLOCKERS_SHOWN} more")

    # ── supervisor stalls ────────────────────────────────────────────
    sup = doc.get("supervisor_stalls")
    out.append("== supervisor ==")
    if not isinstance(sup, dict):
        out.append("  supervisor_stalls section absent")
    else:
        children = sup.get("children") if isinstance(sup.get("children"), list) \
            else []
        out.append(f"  children={sup.get('child_count', '?')} "
                   f"stalled_or_fired={sup.get('stalled_or_fired_count', 0)}")
        for c in children:
            if not isinstance(c, dict):
                continue
            out.append(f"  - {c.get('name', '?')} "
                       f"reason={c.get('stall_reason', '?')} "
                       f"fires={c.get('stall_fires', 0)} "
                       f"last_tick_age={age_s(c.get('last_tick_age_us'))} "
                       f"progress={c.get('progress_marker', 0)}")

    # ── sovereignty (only when the dumper is present) ────────────────
    sov = subs.get("sovereignty")
    if isinstance(sov, dict) and not subsys_error(sov):
        out.append("== sovereignty ==")
        out.append(f"  trust_mode={sov.get('trust_mode', '?')} "
                   f"self_folded={jb(sov.get('self_folded_marker'))} "
                   f"proven_authority={jb(sov.get('coins_kv_proven_authority'))}"
                   f" self_derived={jb(sov.get('self_derived_tip_static_checks'))}"
                   f" ({sov.get('self_derived_reason', '')})")

    story = likely_story(rf, blk)
    if story:
        out.append(f"likely story: {story}")
    return out


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <bundle.json | datadir>", file=sys.stderr)
        return 2
    path = pick_bundle(sys.argv[1])
    if path is None:
        return die(f"no {BUNDLE_GLOB} under {sys.argv[1]}")
    try:
        doc = load_bundle(path)
    except ValueError as e:
        return die(f"malformed bundle: {e}")
    for line in render(path, doc):
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
