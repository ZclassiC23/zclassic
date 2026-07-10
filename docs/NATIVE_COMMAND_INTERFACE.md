# ZClassic23 Native Command Interface

Status: frozen v1 contract; implementation in progress
Audience: LLM coding agents, application developers, node operators, and UI
adapters

## 1. North star

The primary interface to ZClassic23 is one shallow, searchable command tree
owned by the C binary. An LLM loads only the branch needed for its current
task. It never has to ingest a flat catalog of 100+ tools.

For development, the steady-state interaction is simpler still:

1. The agent edits code.
2. The persistent native dev loop notices and coalesces the save.
3. ZClassic23 classifies the change as Core or App.
4. It runs the smallest mandatory deterministic proof.
5. It atomically publishes an App generation or transactionally reloads Core.
6. The agent reads one compact verdict only when the cycle is not green.

No watcher script, deploy script, broad test command, MCP catalog, or service
command belongs in the ordinary agent loop.

## 2. Architectural law

> Core owns truth. Apps consume capabilities.

| Core | Apps |
|---|---|
| Consensus and validation | Resources and controllers |
| Chain-state mutation | Signed application events |
| Block and transaction primitives | Services, jobs, and projections |
| Wallet keys and cryptography | Wallet requests through an opaque capability |
| P2P wire, peers, and sockets | Capability-scoped application topics |
| Raw storage, reducers, boot, and process ownership | Web, onion, ZNAM, and UI bindings |
| Never hot-swappable | Transactionally hot-swappable after ABI/state proof |

Apps compile against `sdk/include/zclassic23/app.h`, not project internals.
The public App ABI intentionally exposes no consensus mutation, raw SQL,
filesystem, socket, private-key, peer-state, boot, or process capability.

## 3. Command grammar

The canonical form is:

```text
zclassic23 [global-options] <branch> [sub-branch ...] [leaf-options]
```

Examples:

```bash
zclassic23 status
zclassic23 core chain block get --height=478544
zclassic23 app invoke names resolve --name=alice
zclassic23 dev app describe social
zclassic23 dev search "ABI mismatch"
```

Normative behavior:

- Invoking a branch returns only that branch and its immediate children.
- Invoking a leaf executes it.
- `help [path]` describes one branch or leaf.
- `search <text>` returns at most five ranked paths.
- Stable machine IDs use dots, for example `core.chain.block.get`; CLI paths
  use spaces.
- The parser resolves the longest registered command path. Leaf arguments
  cannot be mistaken for command names.
- Named options are preferred. Positional arguments are reserved for a single
  obvious identifier such as an app ID and remain documented in the schema.
- Every leaf normalizes its arguments to one JSON object. Complex arguments use
  `--input='<object>'` or `--input=-` for stdin; convenient typed flags compile
  to the same object. Unknown keys and out-of-range values are rejected before
  side effects. A handler never parses shell syntax.
- Unknown branches fail with nearby valid paths and one executable next
  action; they never fall through to an unrelated RPC method.

Standard response controls are `--view=summary|normal|full`,
`--max-items=<n>`, `--cursor=<opaque>`, and `--budget-bytes=<n>`. Truncation is
always explicit and returns a cursor plus a structured retrieval command.

Stable process exit codes are:

| Code | Meaning |
|---:|---|
| 0 | Passed or accepted |
| 1 | Executed and failed |
| 2 | Invalid input or unknown command |
| 3 | Blocked by a named precondition |
| 4 | Authentication or capability denied |
| 5 | Transiently unavailable |
| 6 | Internal contract failure |

## 4. Root tree

```text
zclassic23
├── status
├── core
│   ├── status
│   ├── chain
│   ├── sync
│   ├── consensus
│   ├── network
│   ├── wallet
│   ├── storage
│   └── mining
├── app
│   ├── list
│   ├── inspect <id>
│   ├── protocols
│   └── invoke <id> [path...]
├── dev
│   ├── status
│   ├── core
│   ├── app
│   ├── change
│   ├── loop
│   ├── test
│   ├── generation
│   └── diagnose
├── ops
│   ├── health
│   ├── diagnose
│   ├── lanes
│   ├── jobs
│   ├── logs
│   ├── timeline
│   ├── metrics
│   ├── postmortem
│   ├── config
│   └── recovery
└── discover
    ├── help
    ├── search
    ├── describe
    └── schema
```

The root has six choices: `status`, `core`, `app`, `dev`, `ops`, and
`discover`. `help` and `search` remain convenience aliases for
`discover help` and `discover search`, but are not extra ontology branches.
All other operations live under their owner.

## 5. Core tree

```text
core
├── status
├── chain
│   ├── tip
│   ├── block get
│   ├── transaction get
│   ├── mempool status|list
│   └── wait height|blocker|halt
├── sync
│   ├── status
│   ├── validation
│   ├── blockers
│   └── diagnose
├── consensus
│   ├── report
│   ├── integrity
│   ├── utxo commitment|audit
│   ├── mmb
│   └── block invalidate|reconsider
├── network
│   ├── status
│   ├── peers list|incidents|latency|add
│   └── onion status|health
├── wallet
│   ├── status|balance
│   ├── address new|list|import|export-key
│   ├── utxo list
│   ├── transaction list|get|send
│   ├── shielded address|balance|notes|send
│   ├── backup status|now
│   └── audit|rescan|replay
├── storage
│   ├── stats
│   ├── integrity
│   └── query
└── mining
    ├── status
    └── benchmark
```

Consensus, validation, chain mutation, raw storage, wallet keys, network
ownership, and boot remain Core even when an App requests a bounded service
from them.

## 6. App tree

Every installed App contributes one subtree from its manifest. The Core host
generates discovery, route help, optional transport adapters, and bindings from
that manifest.

```text
app
├── list
├── inspect <id>
├── protocols
└── invoke <id>
    └── <manifest-derived subtree>
```

Installed applications are dynamic children from `apps/<id>/app.def`; they
are not hardcoded forever into the global registry. For example,
`zclassic23 app invoke social` returns only Social's immediate children:

```text
social
├── status
├── profile get|update
├── posts get|publish|reply
├── follows list|add|remove
├── feed get
├── web status
├── onion status
└── znam status
```

The same pattern yields manifest-owned subtrees for explorer, names, messages,
market, swaps, games, and blog without bloating root discovery.

The explorer currently serving zclnet.net becomes a built-in App using the
same host ABI available to external developers. Local HTTP, clearnet HTTPS,
and embedded onion traffic bind to the same resource/controller functions.
ZNAM can bind a human name to the App's onion, clearnet endpoint, or content
identity.

## 7. Development tree

```text
dev
├── status
├── core
│   ├── boundary
│   └── proof
├── app
│   ├── list
│   ├── describe <app>
│   ├── plan <app> <resource>
│   ├── scaffold <app> <resource>
│   ├── simulate <app> [scenario]
│   ├── inspect <app>
│   └── publish <app>
├── change
│   ├── plan [files...]
│   └── apply [files...]
├── loop
│   ├── ensure
│   ├── status
│   ├── wait
│   ├── events
│   └── stop
├── test
│   ├── plan [files...]
│   ├── run <group>
│   ├── sim [app]
│   ├── replay <seed>
│   └── background status
├── generation
│   ├── current
│   ├── history
│   ├── rollback
│   └── compact
└── diagnose
    ├── latest
    └── show <failure-id>
```

The ordinary agent runs `loop ensure` once, edits files, then optionally calls
`loop wait` for the new source epoch. The persistent loop owns classification,
dependency selection, compilation, proofs, publication, rollback, and durable
provenance. `change apply` exists for CI, debugging, and reproducible one-shot
proofs, not as the normal editing ritual.

### Rails-like App layout

```text
apps/<app>/
├── app.def
├── models/
├── controllers/
├── services/
├── events/
├── jobs/
├── projections/
├── views/
└── sim/
```

`app.def` declares resources, routes, capabilities, web/onion/ZNAM bindings,
state schema, migrations, P2P topics, and mandatory simulations. `scaffold`
materializes the conventional C slice. Generated code is ordinary C and may be
edited freely.

## 8. Progressive-disclosure responses

### Branch menu

Schema: `zcl.command_menu.v1`

```json
{
  "schema": "zcl.command_menu.v1",
  "path": "dev.app",
  "summary": "Build capability-scoped C applications",
  "registry_digest": "sha256:...",
  "children": [
    {
      "path": "dev.app.describe",
      "summary": "Describe one App manifest",
      "risk": "read",
      "latency": "<10ms"
    }
  ],
  "next": {
    "command": "dev.app.describe",
    "input": {"app_id": "social"}
  }
}
```

Menus contain immediate children only. They omit argument schemas, aliases,
examples, and transport mappings until a leaf is described.

### Leaf description

Schema: `zcl.command_spec.v1`

Required fields:

- stable path and one-line summary;
- availability: `ready`, `compat`, or `planned`;
- input JSON Schema;
- output schema ID;
- risk, authority, lane scope, mutation, idempotency, and confirmation policy;
- warm latency class;
- one canonical example;
- required Core/App capabilities.

### Execution result

Schema: `zcl.result.v1`

```json
{
  "schema": "zcl.result.v1",
  "command": "dev.app.simulate",
  "ok": true,
  "status": "passed",
  "request_id": "01...",
  "data_schema": "zcl.dev_app_sim.v1",
  "elapsed_us": 9,
  "data": {},
  "next": [{
    "command": "dev.app.publish",
    "input": {"app_id": "social"},
    "reason": "all mandatory scenarios passed"
  }]
}
```

`status` is one of `passed`, `accepted`, `blocked`, or `failed`. `accepted`
returns a job ID. `blocked` names an external or safety precondition. `failed`
means the command attempted work and failed. `ok` reports whether the requested
operation succeeded; it cannot be true merely because valid JSON was produced.

### Error result

Schema: `zcl.result.v1` with `ok=false` and an `error` object

Errors contain:

- stable code;
- short message;
- bounded evidence;
- whether anything mutated;
- retryability, phase, blockers, and a durable failure artifact ID;
- one primary structured, executable next command and at most two alternatives.

No error returns a prose-only recovery essay or a silent nonzero exit.

## 9. Token and payload budgets

Default compact JSON budgets are part of the interface contract:

| Response | Maximum default payload |
|---|---:|
| Root menu | 1,200 bytes |
| Branch menu | 1,600 bytes |
| Leaf specification | 2,400 bytes |
| Status | 2,048 bytes |
| Error or blocker | 2,048 bytes |
| Ordinary result | 4,096 bytes |
| List page | 8,192 bytes or 20 items |
| Search | 5 matches |

Large results require `--view=full`, `--fields=...`, `--max-items`, and a cursor.
Default responses omit nulls, defaults, redundant aliases, repeated
descriptions, and transport metadata.

`loop events` and jobs emit JSON Lines. A heartbeat is small and periodic; unchanged
state is not re-emitted.

## 10. Output modes

- Non-TTY default: compact JSON.
- TTY default: small tree or readable table.
- `--format=json`: compact JSON regardless of TTY.
- `--format=pretty`: indented JSON.
- `--format=tree`: human tree for menus.
- `--format=jsonl`: streams only.
- `--quiet`: data only when a stable leaf payload makes that unambiguous.

Machine status and errors go to stdout as one valid JSON value. Diagnostic
process logs go to bounded artifacts and are referenced, not dumped into the
LLM context.

## 11. Search and context focus

`search` uses registry-owned names, summaries, aliases, tags, synonyms,
capabilities, error codes, and keywords. It is local and deterministic; it does
not call a model or the network. Results are ranked and include the bounded
reason each path matched.

Search results contain only path, one-line match reason, risk, and latency.
An LLM then asks `help <path>` for the selected leaf. This preserves context
focus and makes command discovery reproducible.

Each major branch lives in its own declarative C definition file so an agent
working on Apps does not need to load Core operations:

```text
config/commands/root.def
config/commands/core.def
config/commands/apps.def
config/commands/dev.def
config/commands/ops.def
```

Menus and command specifications carry a registry digest. An LLM may cache a
branch and skip rediscovery while that digest is unchanged.

## 12. Registry as the single source of truth

Every command is declared once. Registry metadata is typed, not free-form:
canonical ID and version, aliases, summary, tags/synonyms, input/output schema
IDs, layer, effect, authority, availability and reason, handler, allowed lanes,
required capabilities, deterministic/reversible/idempotent flags,
confirmation policy, sync/job/stream mode, latency class, cost/rate class,
transport bindings, and deprecation replacement.

Effect and cost are independent: a read-only SQL query or replay can be
expensive, while a local App-state write may be cheap.

A declaration is equivalent to:

```c
ZCL_COMMAND(
    "app.names.resolve",
    names_resolve_handler,
    "Resolve a ZNAM name",
    "zcl.names.resolve.input.v1",
    "zcl.names.resolve.result.v1",
    ZCL_LAYER_APP,
    ZCL_EFFECT_READ,
    ZCL_AUTH_PUBLIC,
    ZCL_MODE_SYNC,
    ZCL_LATENCY_FAST,
    ZCL_CAP_ZNAM,
    "name,znam,resolve,address,onion")
```

The registry generates or validates:

- native CLI dispatch;
- shallow menus and search;
- leaf input schemas;
- human help;
- REST/OpenAPI bindings where permitted;
- optional compact MCP bindings;
- compatibility aliases;
- documentation and contract tests.

Business logic never lives in a CLI, REST, or MCP adapter. All transports call
the same typed C handler.

## 13. Risk and authority

Every leaf declares:

- risk: `read`, `app-write`, `wallet`, `core-recovery`, `destructive`, or
  `dev-mutation`;
- scope: `local`, `node`, `dev-lane`, or `offline-copy`;
- authority: `public`, `operator`, or `owner`;
- allowed lanes;
- whether it is idempotent;
- whether it supports dry-run;
- confirmation policy.

Externally visible mutations require an idempotency key. Reusing the same key
and normalized input returns the original result; reusing it with different
input is a conflict. High-risk commands use a plan/commit handshake: the plan
returns an expiring intent ID and effect digest bound to the exact target,
arguments, generation, lane, and optimistic preconditions such as tip hash or
state version. Commit requires both values. A changed target invalidates the
intent. There is no generic `--force` or English `yes` confirmation.

Successful mutations return an effect ID and, when reversible, a rollback ID.

Local development commands are never registered as node RPC, REST, or MCP
methods. Remote input cannot gain authority over a checkout, compiler, test
runner, generation loader, or service manager.

## 14. Jobs and long-running work

Work expected to exceed roughly 500 ms, or unable to satisfy its declared
foreground latency class, supports asynchronous execution and returns
`accepted` with a job ID. It is managed through:

```text
ops jobs list
ops jobs status <id>
ops jobs wait <id>
ops jobs events <id>
ops jobs log <id>
ops jobs cancel <id>
```

Jobs persist source/build identity, seed, arguments, lane, progress marker,
failure capsule, and next action. Polling a job never restarts it.

Streams are NDJSON only: an initial hello names schema, cursor, and heartbeat;
ordered events carry sequence and kind; a terminal event closes the stream.
`--after=<seq>` resumes. Lost history emits an explicit gap event. Backpressure
is bounded, and compiler/test logs never mix with protocol stdout.

## 15. MCP posture

MCP is an optional compatibility transport, not the ontology or development
engine.

The compact MCP profile exposes only three generic tools:

- `zcl_discover({path|query|digest})`;
- `zcl_exec({command,input,view,budget_bytes})`;
- `zcl_job({id,action,cursor})`.

Their schemas are stable and tiny. They delegate to the same native registry.
The current flat tool catalog remains temporarily available behind an explicit
legacy profile and is not loaded into the default coding-agent context.

REST remains useful for web clients. Native commands remain authoritative for
local agents and operators.

## 16. Compatibility

Existing native commands, RPC methods, and MCP tools become aliases pointing
to registry command IDs. They do not retain duplicate handlers or schemas.

Compatibility responses include a bounded `canonical_path` and deprecation
phase. Aliases remain until callers and docs migrate, then are removed in a
versioned release. Consensus and wallet semantics never change as a side effect
of interface migration.

## 17. Development cycle contract

The native dev state machine is:

```text
debounce -> classify -> prove -> build -> publish/activate -> verify -> record
```

Each phase checks a monotonically increasing source epoch. A newer save
supersedes an older candidate before publication.

### Golden LLM edit loop

```bash
# Idempotent session/bootstrap call.
zclassic23 dev loop ensure \
  --input='{"root":"/home/rhett/github/zclassic23"}'

# The LLM now edits any number of C files directly.

# Optional synchronization when it needs the verdict before continuing.
zclassic23 dev loop wait \
  --input='{"after_epoch":41,"timeout_ms":30000,"view":"summary"}'
```

`ensure` returns watcher ID, registry digest, and baseline source epoch. `wait`
returns exactly one bounded cycle verdict for a newer epoch. On failure, the
agent follows the structured `dev.diagnose.show` command using the returned
failure ID. It never chooses a Make or shell command.

Ordinary App development requires at most one binary call after an edit batch;
when the agent does not need to synchronize immediately, it requires none.

App foreground path:

1. validate public App ABI and capability manifest;
2. run the generic generation/network proof;
3. run App-declared deterministic scenarios;
4. build an immutable content-addressed generation;
5. load, validate, stage, self-test, quiesce, and atomically commit;
6. verify the declared route/protocol probes;
7. asynchronously build the converged immutable binary.

Core foreground path:

1. compile exact affected objects;
2. run mandatory mapped proof, including real-history canaries for consensus;
3. preflight an immutable binary generation;
4. transactionally activate only the isolated dev lane;
5. verify exact executable identity and readiness;
6. restore verified last-good on failure.

Full lint, sanitizers, exhaustive tests, replay, reproducibility, and soak run
as background or pre-push authorities unless an impact rule marks one
foreground-mandatory.

## 18. Social reference acceptance scenario

`dev app simulate social` must prove, with a recorded seed:

- one relay can refuse Alice's valid post without suppressing alternate
  propagation;
- honest peers converge after a partition heals;
- a peer joining after publication catches up through anti-entropy;
- invalid signatures never enter honest projections;
- the same seed produces an identical transcript.

The simulation is RAM-only and belongs in the millisecond foreground path.

## 19. Migration plan

### Phase A — Freeze the contract

- Review this tree, naming rules, envelopes, token budgets, and Core/App law.
- Add golden registry/menu/schema tests.
- Mark every leaf `ready`, `compat`, or `planned`; menus never imply an
  unavailable command works.

Exit: the root and branch trees are stable enough that later work adds leaves
without renaming the grammar.

### Phase B — Native registry and discovery

- Introduce the split `config/commands/*.def` registry.
- Generate native dispatch, menu, help, search, and documentation checks.
- Map every existing native, RPC, MCP, and service operation to a canonical ID;
  fail the build on an unmapped route, duplicate alias, or conflicting effect
  policy.
- Map `status` and all read-only Core/operator commands first.
- Enforce payload budgets in tests.

Exit: an LLM can find and run every read-only operation without MCP discovery.

### Phase C — Native development plane

- Complete `dev loop ensure/wait/events`, native process execution, durable
  verdicts, resumable source epochs, supersession, and generation compaction.
- Port compile/link plans and transactional activation out of shell scripts.
- Make scripts compatibility aliases only, then delete them from the default
  workflow.

Exit: editing code is the only ordinary agent action.

### Phase D — Public App platform

- Finalize the Core host capability table and App manifest ABI.
- Enforce App-only include/link boundaries.
- Add native scaffold, resource, migration, binding, simulation, and publish
  handlers.
- Convert the explorer into the first built-in App without privileged hooks.

Exit: an external developer can build a web/onion/ZNAM App using only the
public SDK.

### Phase E — Social reference App

- Materialize profiles, posts, follows, feeds, signed events, projections, and
  web views.
- Bind the same resources to clearnet, onion, and ZNAM.
- Implement partition, censorship, late-join, invalid-signature, migration,
  and replay scenarios.

Exit: one save can simulate and atomically publish a social App generation.

### Phase F — Runtime Apps and mutations

- Move names, messages, market, swaps, games, blog, and explorer operations
  into manifest-derived `app invoke <id>` subtrees.
- Map wallet and recovery writes with idempotency and confirmation digests.
- Add jobs for long-running work.

Exit: the native tree covers the complete product surface.

### Phase G — Compact transports and cleanup

- Generate REST and compact three-tool MCP adapters from the registry.
- Put the flat MCP catalog behind an explicit legacy profile.
- Remove duplicate adapter logic and stale command documentation.
- Measure command-discovery tokens and save-to-verdict latency.

Exit: native is authoritative, optional transports are mechanically derived,
and default LLM context contains only the selected branch.

## 20. Required proof

- Root/branch menus stay within their byte budgets.
- The root exposes no more than six choices and Apps are dynamic children.
- Search returns no more than five deterministic matches.
- Every ready leaf has one handler, input schema, result schema, risk policy,
  and golden help test.
- Discovery never advertises a ready command that cannot dispatch.
- Every command uses the common result envelope and stable exit-code mapping.
- Every error has a stable code and one executable next action.
- Compatibility aliases call the canonical handler.
- Plan/commit and idempotency conflicts are fail-closed and replay-safe.
- Apps cannot include or link Core-private headers/symbols.
- Release builds contain no dev mutation or loader command path.
- A failed App generation publishes nothing.
- In-flight calls finish on their original generation.
- A failed Core activation restores and verifies last-good.
- Canonical and soak lanes are unreachable from the native dev loop.
- Default MCP discovery contains only the compact gateway tools.
- Ordinary App development needs at most one binary call after an edit batch.
- The social reference simulation proves censorship bypass and deterministic
  replay within the foreground latency budget.

## 21. Current prototype and gaps

The current worktree contains an early native `dev` tree, compact planning,
native inotify watcher, public App ABI skeleton, social manifest, and an
in-process deterministic social scenario. These prove the shape of the
interface; they do not yet satisfy the full plan.

Known gaps before calling the interface production-ready:

- a transport-neutral registry engine now exists at
  `lib/kernel/{include/kernel/command_registry.h,src/command_registry.c}` and
  the first declarative `root`, `apps`, and `dev` definitions exist under
  `config/commands/`, but the composition-root catalog, Core/Ops definitions,
  generated bindings, and native adapter are not wired yet;
- until that wiring lands, the executable command registry remains the older
  dev-specific prototype rather than the split global registry in this spec;
- prototype menu metadata still uses loose strings rather than typed effect,
  authority, availability, capability, schema, and execution-mode fields;
- prototype results and errors do not yet use the common envelope or stable
  exit-code policy;
- search is an unranked substring match without registry tags, synonyms, or
  match reasons;
- the Social menu is currently hardcoded instead of derived from `app.def`;
- `loop ensure/wait/events`, resumable streams, durable heartbeat cursors, and
  native jobs are not yet implemented;
- some advertised prototype leaves are descriptions only and must be marked
  `planned` until their handlers exist;
- some native dev execution still invokes fixed Make compatibility targets;
- transactional activation/status still has shell-backed compatibility paths;
- App generation loading is not yet wired to the public App ABI;
- `ready` versus `planned` metadata is not yet emitted by every menu node;
- flat MCP routes still exist and remain the default MCP catalog;
- the running dev service still needs one safe immutable-generation bootstrap
  before the persistent native watcher can take ownership.

## 22. Migration inventory baseline

As of 2026-07-10, the interface vocabulary is split across 33 native agent
contracts, roughly 201 full-profile RPC methods (plus two exact-dev methods),
139 MCP routes, nine service entries, 24 service operations, and numerous Make
or script development targets. Those counts are an inventory baseline, not the
future public shape.

Canonical migration mapping:

| Current surface | Canonical owner |
|---|---|
| status, health, sync, chain, net, wallet RPC/MCP | `status` or `core` |
| names, messages, files, market, swaps, tokens, games, explorer | dynamic `app` subtree |
| state, logs, timeline, SQL, metrics, lanes, postmortem | `ops` |
| build, impact, tests, watcher, hot-swap, generations, deploy | `dev` |
| API/tool/service/protocol catalogs | `discover` or `app protocols` |
| raw RPC escape hatch | compatibility alias, never primary discovery |

The migration gate fails when an existing callable route has no canonical ID,
two aliases collide, or transport-local safety metadata disagrees with the
canonical effect and authority policy.
