# Codebase consolidation review — 2026-07-14

> **Status:** dated review evidence, not an execution plan. The sovereign cure
> remains priority #1, `FORWARD_PLAN.md` remains the only ordered plan, and the
> live mint producer must not be disturbed. Promote individual recommendations
> into that plan as bounded, independently reviewed changes.

## Verdict

The architecture has a strong center: sealed consensus code, append-only
progress, fail-closed reducer stages, explicit framework shapes, and unusually
good structural linting. The main maintainability cost is not a missing
framework. It is accumulated compatibility and control-plane duplication around
that framework: MCP beside native commands, two test dispatchers, repeated
command/diagnostic catalogs, overlapping developer loops, historical recovery
paths, and a documentation corpus whose status is not machine-readable.

The right strategy is deletion-first and dependency-aware:

1. Keep the cure producer and canonical data untouched.
2. Finish the already-planned zero-MCP subtraction.
3. Establish one manifest for tests and one manifest for tooling gates.
4. Make the native code navigator reliable enough to replace prose catalogs.
5. Remove unsafe/dead compatibility ingress after fixture proof.
6. Delete borrowed-state recovery families only after the complete-state cure
   has passed full-history and copy-proof gates.
7. Split boot/network monoliths last; moving code without deleting a duplicate
   control plane is lower value.

A conservative implementation can remove roughly 30,000 physical lines across
production, tests, and tooling. The documentation audit supports removing or
consolidating 0.6–0.95 MB, about one third of the Markdown corpus. These are
gross opportunities, not quotas: generated consensus tables, cryptographic
fixtures, and intentionally distinct safety policies must not be compressed
merely to improve a line count.

## Scope and evidence

The review covered tracked C/C headers, Make/build/test/lint surfaces, systemd
units, shell tooling, command and diagnostics registries, the code-index
developer interface, and every tracked Markdown path. Vendor sources were
excluded from architectural recommendations.

- Approximately 736,000 physical C/header lines; `cloc` reports roughly
  567,000 C/header code lines across 1,520 C files and 742 headers.
- The largest production units are `config/src/boot.c` (3,950 lines),
  `src/main.c` (3,799), `lib/chain/src/sha3_windows.c` (3,207),
  `lib/net/src/msgprocessor_snapshot.c` (2,452), `lib/net/src/connman.c`
  (2,435), and `tools/mcp/controllers/ops_controller.c` (2,395).
- The Makefile is 3,810 lines with 238 unique `.PHONY` names. There are 137
  shell scripts totaling about 29,000 lines; 66 are check/gate scripts, while
  only 21 use the common gate helper.
- The unique Markdown corpus is 160 regular files, 2.28 MB, and 34,874 lines.
  `docs/work/` plus its archive is 63.6% of the bytes; the archive alone is
  41.4%.

This was a codebase-wide structural and semantic hotspot review, not a claim
that every consensus branch received a fresh formal proof.

## Priority findings

### 1. Finish zero-MCP W2/W3; do not rename the old server

The existing removal plan has the correct dependency order. Gross deletion is
about 21,782 physical lines: roughly 10,448 under `tools/mcp*`, plus 11,187 in
dedicated MCP/dev-MCP tests and a small generator/config tail. Native handler
snapshots and hot-swap support already exist.

Keep only native assertions that prove behavior. Migrate them into the command
registry/native API tests; do not carry the 6,253-line
`test_mcp_controllers.c` forward under a new filename. Preserve operator-event
semantics and intentionally retire MCP-only auth/rate-limit behavior. Required
proof: native command parity, golden dev cycle, handler-snapshot/hot-swap tests,
zero operational `mcpcall` consumers, build, lint, and `test_parallel`.

### 2. Use one test manifest and retire the serial dispatcher

`test_parallel.c` has 592 test rows plus 28 specs but only 619 unique names;
`test_spawn` is registered twice. Exclusive and params-heavy policy is
hard-coded separately. The 1,575-line `test.c`, 484-line prototype header,
`spec_main.c`, and the registration checker all repeat parts of the same list.

Create one X-macro/data manifest with symbol, public name, and policy flags.
Generate prototypes, registry rows, list output, and runner policy. Migrate
focused targets to `test_parallel --only`, then remove the serial runner and
remove `test_zcl` from the default build. Expected net reduction: 1,800–2,500
lines. Preserve the two currently serial-only filename routes until they are
represented: `block_index_node_db_topup` and `self_folded_anchor`.

### 3. Make lint/tool registration data-driven

The 74 lint prerequisites live on one Makefile line and the 7,592-line
`test_make_lint_gates.c` mixes 93 lint tests with 46 broad source-text
contracts. Introduce one gate manifest with name, command, fast/full tier,
mode, baseline, and hermetic self-test. One aggregate runner should project it
into lint, CI, and the documented gate inventory.

Immediate candidates are the four nearly identical silent-error recipes,
overlap between narrow `check-malloc` and broader `check-raw-malloc`, and shared
scanner mechanics for raw-SQL policies. Split lint self-tests by policy family.
Expected reduction: 800–1,500 lines. The quality-job guard added during this
server cleanup is registered in the existing lint-test group now; migrate it to
the manifest when that control plane exists.

### 4. Repair the native code navigator before adding more prose

The developer-facing `code` command is promising but is not a whole-tree
navigator today:

- `ci_enumerate_sources()` omits `ports/`, `src/`, and `lib/test/`, so the map
  reports zero port files and cannot find `src/main.c` or test runner symbols.
- Large-file lookup can fail with `RESPONSE_BUDGET_EXCEEDED` instead of a
  cursor/paginated result.
- Purpose extraction can return the MIT distribution sentence because it
  skips `Copyright` but not the remaining license preamble.
- Handler joins are absent when registration macros do not expose the handler
  symbol.

Add all source roots, paginate large responses, parse an explicit `purpose:`
line before fallback prose, and stringify handler symbols in the canonical
command registration. Then generate code/module maps from the same build
manifest rather than mirroring Makefile lists in C. This is the highest-value
developer-experience change because it permits deleting hand-maintained maps.

### 5. Remove or consolidate unsafe compatibility ingress

`src/main.c` contains a roughly 283-line `--repair` path that mutates UTXO
state with raw SQLite and `strstr` JSON parsing; no current non-archive operator
reference was found. Its one-shot `importchainstate` path separately duplicates
the controller implementation while omitting the controller's header binding
and CSR commit behavior.

Prove on isolated fixtures, delete `--repair`, and make any remaining one-shot
entry point call the same service as the controller. Expected immediate
reduction: 380–460 lines. After the sovereign cure, delete both borrowed-state
ingress surfaces if no longer needed.

### 6. Correct the advisory commitment-MMR/XOR truth contract

The docs claimed `rpc_blockchain_maybe_commit()` had zero callers, but
`boot_services.c` calls it once to bootstrap an empty commitment history. No
ordinary per-tip path builds complete history. Nevertheless,
`getcommitmentmmr` says the value verifies imported UTXO snapshots and
`auditchain` includes coverage in `audit_passed`.

An XOR accumulator not committed by ZClassic headers cannot authenticate a
peer snapshot or prove consensus provenance. First prove no admission/loader
trusts the value; then remove or explicitly rename the advisory subsystem while
retaining the independent block-hash MMR/MMB and full SHA3 checkpoint logic.
Likely reduction: 180–250 lines, with a larger benefit in operator honesty.

### 7. Delete production modules that only tests call

A production-zero-caller candidate set totals about 3,311 C/header lines:
`lib/session/`, policy fees, async RPC queue/operation, generic schema
migration, superseded coins-view projection, orphan pool, sigcache,
pagelocker, and `process_block_tip_child.c`.

Do not delete this as one batch. For each module, prove the link/caller map,
classify tests as useful contract tests versus tests of dead code, and remove
one family per review. Session/pagelocker carry security expectations and the
tip-child helper is recovery-sensitive, so those need independent review and a
copy-datadir recovery proof.

### 8. Delete borrowed recovery code only after the cure

The candidate families contain more than 10,000 physical lines, but some files
mix still-needed refill, purge, reorg, body-fetch, and header repair behavior.
The cleanest eventual carve is the 2,281-line coin-backfill family; broader
UTXO recovery is about 3,091 lines and chain restore about 2,391.

This is blocked on a from-genesis transparent+shielded fold, exact checkpoint
and tip parity, complete anchor/nullifier proof, warm reboot, kill-9 resume,
and canonical copy proof. Realistic post-classification reduction is 4,000–8,000
lines. Consensus compatibility and rollback proof outrank deletion size.

### 9. Consolidate descriptors and small repeated policies

These are bounded DRY improvements suitable for independent changes:

- Generate the 39 condition declarations and registrations from one `.def`
  list (about 35–40 lines and a drift class removed).
- Make diagnostics dumper metadata one descriptor source; the catalog
  controller currently repeats stage class, cost, freshness, keys, owner, test,
  and drilldowns through `strcmp` chains (about 180–260 lines).
- Descriptor-drive projection open/catch-up/close sequences with explicit
  exception flags for wallet and failure-policy ordering (about 80–140 lines).
- Extract one descriptor-driven read-only query-filter helper from the name
  service and service-operation controllers (about 150–250 lines).
- Move generic hex validation out of `app/views/`; 35 C files currently include
  the view helper, and manual byte-to-hex loops coexist with `HexStr`.
- Remove duplicate fresh-schema explorer DDL after exact new-DB and migration
  schema comparison (about 65–75 lines).

### 10. Decompose monoliths after subtraction

The long-function lint misses the largest functions because it focuses on app
controllers/services and MCP code. `app_init()` is about 2,843 lines,
`main()` about 1,210, and `msg_send_messages()` about 520.

After compatibility deletion, extract coherent boot phases, move one-shot CLI
handlers out of `main()`, and split network send logic by protocol phase.
Extend the long-function gate to `config/`, `src/`, and `lib/net/`. This is a
legibility project rather than a major net-line reduction and requires the
strongest boot-copy and network regression gates.

## Documentation reduction

The current control plane has too many implied plans and too little status
metadata. Twenty-one of 33 non-archive work docs are not reachable from
`HANDOFF`, `MVP`, `FORWARD_PLAN`, or the docs index. Several documents call
themselves the plan of record. Live wedge heights and performance conclusions
are repeated across durable references, where they become contradictions.

Use this information architecture:

```text
docs/HANDOFF.md            current facts only
docs/MVP.md                stable acceptance contract
docs/work/FORWARD_PLAN.md  sole ordered execution queue
docs/work/active/          only plans linked by FORWARD_PLAN
docs/reference/            durable architecture and doctrine
docs/operations/           runbooks and recovery
docs/design/backlog/       proposals without a priority claim
docs/evidence/             dated immutable receipts
docs/archive/INDEX.md      old path -> successor/commit/verdict
```

Recommended sequence:

1. Add a linted document manifest with `kind`, `status`, `successor`,
   `last_verified_commit`, and `expires_when`; require active plans to be
   reachable and links to resolve.
2. Convert fold prose into dated experiment receipts and leave one current
   conclusion in `HANDOFF`/`BENCHMARKS_LOG`.
3. Extract unique artifact/commit/verdict records, then delete 22 unreferenced
   archive narratives (346 KB); Git history remains available.
4. Collapse the remaining handoff history (about another 100 KB).
5. Generate command reference from `config/commands/*.def`; reduce or retire
   the overlapping 199 KB API/reference family after W3.
6. Move completed plans and off-v1 designs out of the active queue.

Never put a live height or ETA in a durable architecture/interface example.
Only `HANDOFF` or explicitly dated evidence should state such values.

## Developer surface to converge on

Future developers should need four ideas, not dozens of aliases:

- one native discovery/status interface (`discover`, `code`, `dumpstate`);
- one persistent dev loop plus one one-shot check command;
- one test runner with `--only` and one manifest;
- three control docs: `HANDOFF`, `MVP`, and `FORWARD_PLAN`.

The two dev-loop classifiers (`agent_fast_ci.sh` and native
`devloop_plan.c`) currently duplicate impact and fixture policy. Make the C
planner emit stable JSON and reduce shell to a compatibility shim, then
deprecate redundant Make aliases. Template only the four genuinely identical
quality services; keep canonical node, soak, and evidence services distinct
because their safety/resource policies are meaningful.

## Changes safe enough to land during this audit

- Removed four already-shrunk files from the E1 oversize baseline, reducing it
  from 21 to 17 without changing runtime code.
- Added a recursion sentinel and bounded regression case to the E14 condition
  cooldown self-test after an accidental recursive invocation created a large
  process tree.
- Added fail-closed mint-aware guards and bounded log retention to heavyweight
  background quality services, plus a hermetic registered test.
- Corrected stale producer, worktree, and advisory commitment documentation.

No node binary, consensus path, canonical datadir, or running producer was
changed by this review.

## What not to consolidate

- Do not combine node/soak/evidence systemd units merely because their syntax
  is similar.
- Do not replace generated cryptographic windows or consensus fixtures with
  runtime generators without deterministic byte-identity proof.
- Do not merge purpose-per-file modules back into mega-files to reduce file
  count.
- Do not delete `legacy_*` by name; `LEGACY_LIFECYCLE.md` documents several
  load-bearing paths.
- Do not weaken SQLite durability or place the cure datadir on volatile RAM.
- Do not change consensus predicates without full-history replay against the
  real chain and exact zclassicd parity.

## Promotion gates

Every recommendation becomes its own bounded change with a deletion proof,
focused negative control, `make lint`, build, focused tests, and
`test_parallel`. Boot/recovery changes additionally require a datadir-copy
proof, H\* climb, exact same-height parity, warm restart, and kill-9 resume.
Consensus-adjacent changes remain owner-gated. Documentation deletion requires
an evidence index proving each removed narrative's unique receipt was retained.
