# deploy/examples — operator-specific unit files

Reference units from the original operator's machine. Unlike the portable
units in `deploy/` (which use `%h`), these hardcode operator paths or name
operator-specific peers. Copy and adapt before installing:

- `zclassicd-rhett.service` — legacy C++ `zclassicd` dev peer used as a
  drift/consensus oracle (see CLAUDE.md "Services" and docs/SYNC.md).
- `zclassic23-soak.service` / `.timer` — 10-minute three-guarantees soak
  probe driving `tools/scripts/soak_assert.sh` (hardcoded repo path).
