# OS-B2 — fold the first_call latency contract into the kernel result envelope

Plan of record: `~/.claude/plans/think-more-about-our-keen-crown.md` §2 LLM-FRIENDLY "OS-B2"
(line 194) + §3 step 1 (OS-B, "B1 ratchet → B2 latency envelope") + §3b "WF-envelope" (line 296,
"OS-B2 (opus; depends on B1 merged)"). All anchors below were re-read on `main @ 6405cf48d` this
session (2026-07-15); re-verify line numbers before editing — B1 shifts most of them (see
"Merge order" below, which pins exactly what shifts and what doesn't).

## The one-sentence problem

`agent_first_call.h:11-14` (`ZCL_AGENT_FIRST_CALL_BUDGET_AGENT_MS 250` /
`_HEALTHCHECK_MS 500` / `_LIVENESS_MS 750` / `_DIAGNOSE_MS 900`) is consumed only by five legacy
MCP agent controllers (`agent_controller.c`, `agent_diagnose_controller.c`,
`agent_liveness_controller.c`, `event_agent_summary.c`, `event_healthcheck_controller.c` —
verified via `grep -rln agent_first_call app/ lib/ tools/`) and dies the moment zero-MCP W3
deletes `tools/mcp/**` and these MCP-only controllers with it. The native `zcl.result.v1`
envelope (`serialize_reply`, `command_registry.c:1342`) has NO latency budget/measurement field
at all today — only `elapsed_us` (unbudgeted). Every native leaf already declares
`enum zcl_command_latency latency` (`command_registry.h:251`, 5 values: INSTANT/FAST/FOREGROUND/
BACKGROUND/PERSISTENT) but nothing reads it except the string name in `discover describe`
(`describe_json:1030-1031`). B2 makes `latency` load-bearing: map it to a budget, measure against
it on every dispatch, and expose the measured p99 for the leaf.

## Design decision: derive, don't store — zero new `zcl_command_spec` fields

B1 (in flight, worktree `.claude/worktrees/agent-aba54b0f6f26b21bc`, branch
`worktree-agent-aba54b0f6f26b21bc`) already adds two fields to `struct zcl_command_spec`
(`semantics`, `budget_bytes` — see "Merge order" below). **B2 adds none.** The ms budget is a
*pure function* of the already-existing `spec->latency` enum — no `.def` file, no
`command_catalog.c` macro, and no fixture struct-literal in any test needs to change. This is
the single biggest lever for keeping B2 collision-free with B1: B1's diff touches
`config/commands/*.def` (7 files, ~500 LOC) and `config/src/command_catalog.c`; B2 touches
neither.

Similarly the p99 ring is a **side-table keyed by array offset**, not a struct field — see
below.

## 1. The budget mapping (new, pure, total)

`lib/kernel/include/kernel/command_registry.h`, insert immediately after the `latency` enum
(today ends at line 90 with `};`, before `enum zcl_command_cost` at line 92 — this region is
untouched by B1's diff, so the anchor is stable pre- and post-B1):

```c
/* Per-latency-bucket dispatch budget in milliseconds. Rehomes the legacy
 * agent_first_call.h budgets (250/500/750/900, MCP-only, deleted in zero-MCP
 * W3) as the kernel's own contract so every native leaf carries it, not just
 * the five MCP agent controllers that used to. */
#define ZCL_COMMAND_LATENCY_BUDGET_INSTANT_MS    50
#define ZCL_COMMAND_LATENCY_BUDGET_FAST_MS       250
#define ZCL_COMMAND_LATENCY_BUDGET_FOREGROUND_MS 750
#define ZCL_COMMAND_LATENCY_BUDGET_BACKGROUND_MS 900
#define ZCL_COMMAND_LATENCY_BUDGET_PERSISTENT_MS 900

/* >= the compiled catalog's leaf count; sized with headroom for the per-leaf
 * latency-sample ring (§2). config/src/command_catalog.c asserts against this
 * at compile time (§2, "size guard"). */
#define ZCL_COMMAND_LATENCY_TABLE_MAX 512U

/* Maps a leaf's declared `latency` enum to its dispatch budget in ms. Pure,
 * total: an out-of-range value falls back to the PERSISTENT/900ms ceiling,
 * never 0 or undefined behavior. */
int64_t zcl_command_latency_budget_ms(enum zcl_command_latency latency);
```

`lib/kernel/src/command_registry.c` — add the definition next to the other `NAME_FN`/table
lookups (near `g_latency_names`, line 331-333, untouched by B1):

```c
static const int64_t g_latency_budget_ms[] = {
    [ZCL_COMMAND_LATENCY_INSTANT]    = ZCL_COMMAND_LATENCY_BUDGET_INSTANT_MS,
    [ZCL_COMMAND_LATENCY_FAST]       = ZCL_COMMAND_LATENCY_BUDGET_FAST_MS,
    [ZCL_COMMAND_LATENCY_FOREGROUND] = ZCL_COMMAND_LATENCY_BUDGET_FOREGROUND_MS,
    [ZCL_COMMAND_LATENCY_BACKGROUND] = ZCL_COMMAND_LATENCY_BUDGET_BACKGROUND_MS,
    [ZCL_COMMAND_LATENCY_PERSISTENT] = ZCL_COMMAND_LATENCY_BUDGET_PERSISTENT_MS,
};

int64_t zcl_command_latency_budget_ms(enum zcl_command_latency latency)
{
    size_t idx = (size_t)latency;
    if (idx >= sizeof(g_latency_budget_ms) / sizeof(g_latency_budget_ms[0]))
        return ZCL_COMMAND_LATENCY_BUDGET_PERSISTENT_MS;
    return g_latency_budget_ms[idx];
}
```

Design note on the specific numbers: INSTANT/FAST/FOREGROUND reuse the legacy AGENT(250) and
LIVENESS(750) anchors exactly (INSTANT gets its own tighter 50ms — nothing in the legacy scheme
had a value that small, and INSTANT leaves are in-binary index reads, e.g. `code.*`).
BACKGROUND/PERSISTENT both get the legacy DIAGNOSE ceiling (900ms) — for `MODE_JOB`/`MODE_STREAM`
leaves this budgets the **dispatch/kickoff** call (accept-and-return-a-handle), never the job's
own completion time, which is out of scope for this envelope (jobs are polled, not blocked on).
State this explicitly in the doc comment above the table so nobody later "fixes" BACKGROUND to
mean job-completion latency.

## 2. The per-leaf latency ring (new, side-table, no struct field)

`lib/kernel/src/command_registry.c`, insert right after `static _Atomic uint64_t
g_request_sequence = 1;` (line 18, untouched by B1):

```c
/* ── Per-leaf latency ring (OS-B2) ───────────────────────────────────────
 * A small in-process ring of the last ZCL_COMMAND_LATENCY_RING_CAP dispatch
 * durations per catalog leaf, indexed by the leaf's offset into whichever
 * `registry->commands` array it was dispatched through. Feeds the
 * `observed_p99_us`/`observed_samples` fields in
 * zcl_command_registry_describe_json.
 *
 * PROCESS-LIFETIME CAVEAT (read before treating p99 as durable): the plain
 * CLI path (`zclassic23 <command>`, tools/command/native_command.c:2388,
 * dispatched from src/main.c ~2021/2030) is a FRESH OS PROCESS PER
 * INVOCATION — main() calls zcl_native_command_main() and returns. A ring
 * that lives in static process memory therefore starts EMPTY on every plain
 * CLI call; `discover describe` run immediately after one CLI command will
 * usually show observed_samples=1 (that command's own dispatch), not a
 * historical p99. The ring accumulates real history only within a
 * long-lived process: `-mcp-inprocess`, the eventual REST server once
 * OS-B3b wires it through this same execute path, or a test/fixture process
 * that dispatches the same leaf repeatedly. This is deliberate phase-1
 * scope (the acceptance bar below is an in-process fixture test) — a
 * cross-process persistence layer (mmap'd or on-disk ring, keyed like
 * progress.kv) is explicitly follow-on work, not part of B2. */
#define ZCL_COMMAND_LATENCY_RING_CAP 64U

struct zcl_command_latency_ring {
    _Atomic int64_t samples_us[ZCL_COMMAND_LATENCY_RING_CAP];
    _Atomic uint32_t next;
    _Atomic uint32_t filled;
};

static struct zcl_command_latency_ring
    g_latency_rings[ZCL_COMMAND_LATENCY_TABLE_MAX];

/* Bound-checked against BOTH `registry->count` (the caller's own array) and
 * ZCL_COMMAND_LATENCY_TABLE_MAX (the side-table's fixed size) before
 * indexing — an ad hoc test registry larger than the compiled catalog, or a
 * `spec` pointer that isn't actually inside `registry->commands`, silently
 * no-ops rather than indexing out of bounds. KNOWN LIMITATION: two DIFFERENT
 * registries dispatched in the SAME process share slot space by raw offset
 * (e.g. index 3 in the real catalog and index 3 in a small ad hoc test
 * registry write the same ring). The only production caller
 * (native_command.c:2388) always passes zcl_command_catalog(), so this only
 * matters inside test binaries that build small ad hoc registries in the
 * SAME test process as catalog-based tests — acceptable for phase 1;
 * document it, don't engineer around it. */
static void latency_ring_record(const struct zcl_command_registry *registry,
                                const struct zcl_command_spec *spec,
                                int64_t elapsed_us)
{
    if (!registry || !spec || !registry->commands || elapsed_us < 0)
        return;
    if (spec < registry->commands ||
        spec >= registry->commands + registry->count)
        return;
    size_t idx = (size_t)(spec - registry->commands);
    if (idx >= ZCL_COMMAND_LATENCY_TABLE_MAX)
        return;
    struct zcl_command_latency_ring *ring = &g_latency_rings[idx];
    uint32_t slot = atomic_fetch_add_explicit(&ring->next, 1,
                                              memory_order_relaxed) %
                    ZCL_COMMAND_LATENCY_RING_CAP;
    atomic_store_explicit(&ring->samples_us[slot], elapsed_us,
                          memory_order_relaxed);
    uint32_t filled = atomic_load_explicit(&ring->filled,
                                           memory_order_relaxed);
    if (filled < ZCL_COMMAND_LATENCY_RING_CAP)
        atomic_fetch_add_explicit(&ring->filled, 1, memory_order_relaxed);
}

static int latency_cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

/* p99 + sample count for the ring at `spec`'s offset in `registry`. Returns
 * false with *count=0 when no samples exist yet (fresh process — see the
 * PROCESS-LIFETIME CAVEAT above); *p99_us is 0 in that case, never garbage. */
static bool latency_ring_p99(const struct zcl_command_registry *registry,
                             const struct zcl_command_spec *spec,
                             int64_t *p99_us, uint32_t *count)
{
    *p99_us = 0;
    *count = 0;
    if (!registry || !spec || spec < registry->commands ||
        spec >= registry->commands + registry->count)
        return false;
    size_t idx = (size_t)(spec - registry->commands);
    if (idx >= ZCL_COMMAND_LATENCY_TABLE_MAX)
        return false;
    struct zcl_command_latency_ring *ring = &g_latency_rings[idx];
    uint32_t filled = atomic_load_explicit(&ring->filled,
                                           memory_order_relaxed);
    if (filled == 0)
        return false;
    int64_t tmp[ZCL_COMMAND_LATENCY_RING_CAP];
    for (uint32_t i = 0; i < filled; i++)
        tmp[i] = atomic_load_explicit(&ring->samples_us[i],
                                      memory_order_relaxed);
    qsort(tmp, filled, sizeof(int64_t), latency_cmp_i64);
    *p99_us = tmp[(size_t)((filled - 1) * 99 / 100)];
    *count = filled;
    return true;
}
```

`<stdatomic.h>` and `<stdlib.h>` (for `qsort`) are already `#include`d at the top of
`command_registry.c` (lines 8, 12 today) — no new includes.

**Size guard (compile-time, layering-clean):** `lib/kernel` may not include `config/*`
(`check-lib-layering`), so the "table is big enough for the real catalog" check must live on the
`config/src` side, which already includes `kernel/command_registry.h`. Add, right after the
existing `static const struct zcl_command_registry g_catalog_registry = { ... };` in
`config/src/command_catalog.c` (today line 242-245, untouched by B1 — B1's `command_catalog.c`
diff only touches the macro bodies above line 218, not this tail):

```c
_Static_assert(sizeof(g_catalog_commands) / sizeof(g_catalog_commands[0]) <=
                   ZCL_COMMAND_LATENCY_TABLE_MAX,
               "catalog leaf count exceeds the kernel latency-ring table — "
               "raise ZCL_COMMAND_LATENCY_TABLE_MAX in command_registry.h");
```

## 3. Wire it into `zcl_command_registry_execute_json` (measurement)

`lib/kernel/src/command_registry.c:1388-1527` (today; untouched by B1's diff in its entirety —
B1's only hunk in this file lands inside `zcl_command_registry_validate` (~561),
`zcl_command_registry_describe_json` (~1005-1060), and the tail of `serialize_reply` (~1372-1392,
the `contract` budget-bytes clamp) — `execute_json`'s body is verbatim untouched by B1, so these
line numbers are stable post-merge too).

Two edits:

**(a)** Line 1398 today reads `(void)registry;` — delete it. `registry` becomes genuinely used
below, so the discard cast must go (leaving it would be dead/wrong once the param is read).

**(b)** Right before the existing `serialize_reply(...)` call (today lines 1499-1504):

```c
    int64_t elapsed_us = platform_time_monotonic_us() - started_us;
    uint64_t sequence = atomic_fetch_add_explicit(&g_request_sequence, 1,
                                                   memory_order_relaxed);
+   latency_ring_record(registry, spec, elapsed_us);
    size_t result = serialize_reply(spec, &reply, invoked_by_alias, sequence,
                                    elapsed_us, budget_bytes,
                                    out, out_size);
```

Recording is unconditional (every dispatch outcome — PLANNED/COMPAT/LANE_DENIED/AUTHORITY_DENIED/
CAPABILITY_DENIED fast-fail paths included, since `elapsed_us` is already computed once for all
of them at this single call site). This is deliberate: a leaf whose authorization check alone
blows its latency budget is exactly the kind of regression the ring should catch.

## 4. Wire it into `serialize_reply` (the envelope fields)

`lib/kernel/src/command_registry.c:1342-1386` today. B1's only change in this function is at the
very end (`contract` computation, ~1377-1380) — the insertion point below is at the *top* of the
function, immediately after the existing `elapsed_us` push (today lines 1358-1365), so there is
no line-range overlap with B1's edit inside the same function — a 3-way merge trivially unions
both hunks.

```c
    bool ok = json_push_kv_str(&root, "schema", "zcl.result.v1") &&
              json_push_kv_str(&root, "command", spec->path) &&
              json_push_kv_bool(&root, "ok", successful) &&
              json_push_kv_str(&root, "status",
                               zcl_command_status_name(reply->status)) &&
              json_push_kv_str(&root, "request_id", request_id) &&
              json_push_kv_int(&root, "elapsed_us",
                               elapsed_us < 0 ? 0 : elapsed_us);
+   int64_t budget_ms = zcl_command_latency_budget_ms(spec->latency);
+   int64_t elapsed_ms = elapsed_us < 0 ? 0 : elapsed_us / 1000;
+   bool budget_exceeded = elapsed_us > budget_ms * 1000;
+   ok = ok && json_push_kv_int(&root, "budget_ms", budget_ms) &&
+        json_push_kv_int(&root, "elapsed_ms", elapsed_ms) &&
+        json_push_kv_bool(&root, "budget_exceeded", budget_exceeded);
    if (invoked_by_alias)
        ok = ok && json_push_kv_str(&root, "canonical_path", spec->path);
```

`elapsed_us` stays exactly as-is (microsecond precision, unbudgeted) — nothing removes it, no
test asserting on it breaks. `elapsed_ms`/`budget_ms`/`budget_exceeded` are new siblings.

Byte-budget headroom check (measure, don't guess): three new keys add roughly
`"budget_ms":900,"elapsed_ms":12,"budget_exceeded":false,` ≈ 55 bytes to every envelope.
`ZCL_COMMAND_RESULT_BUDGET` (4096) and `ZCL_COMMAND_ERROR_BUDGET` (2048, unchanged by B1) have
ample headroom on inspection, but run the existing byte-budget tests
(`test_command_registry_catalog.c` — search for `_BUDGET` assertions, e.g. the
`test_status_brief_flat_lean_envelope` truncation check at line 371) after the change; bump
either constant only if a real test regresses.

## 5. Wire it into `zcl_command_registry_describe_json` (measured p99)

`lib/kernel/src/command_registry.c:980-1056` today. **This is the one seam that must be
re-derived from the POST-B1 file, not today's**, because B1 inserts two things directly in this
function's `policy`-building chain (verified in the B1 worktree diff, `.claude/worktrees/agent-
aba54b0f6f26b21bc`):

```c
     json_push_kv_int(&policy, "allowed_lanes", spec->allowed_lanes) &&
     json_push_kv_int(&policy, "required_capabilities",
                      (int64_t)spec->required_capabilities) &&
+    json_push_kv_int(&policy, "budget_bytes",             /* ← B1 added this */
+                     spec->budget_bytes > 0
+                         ? spec->budget_bytes
+                         : (int64_t)ZCL_COMMAND_RESULT_BUDGET) &&
     json_push_kv(&root, "policy", &policy) &&
     json_push_kv_str(&root, "example", spec->example);
```

B2 inserts its two fields **after** B1's `budget_bytes` push and **before** the
`json_push_kv(&root, "policy", &policy)` line — i.e. author this against the merged main, find
the `"budget_bytes"` push B1 landed, and extend the chain immediately after it:

```c
     json_push_kv_int(&policy, "budget_bytes",
                      spec->budget_bytes > 0
                          ? spec->budget_bytes
                          : (int64_t)ZCL_COMMAND_RESULT_BUDGET) &&
+    json_push_kv_int(&policy, "budget_ms",
+                     zcl_command_latency_budget_ms(spec->latency)) &&
+    json_push_kv_int(&policy, "observed_p99_us", observed_p99_us) &&
+    json_push_kv_int(&policy, "observed_samples",
+                     (int64_t)observed_samples) &&
     json_push_kv(&root, "policy", &policy) &&
     json_push_kv_str(&root, "example", spec->example);
```

...with the two locals computed once, before the big `ok = ok && ...` chain starts (right after
`json_set_object(&policy);`, today line 1001):

```c
    json_set_object(&policy);
+   int64_t observed_p99_us = 0;
+   uint32_t observed_samples = 0;
+   (void)latency_ring_p99(registry, spec, &observed_p99_us, &observed_samples);
    bool ok = json_push_kv_str(&root, "schema", "zcl.command_spec.v1") &&
```

`describe_json` already receives `registry` as its first parameter (line 980-982, unchanged) —
no signature change needed anywhere in this section.

`ZCL_COMMAND_SPEC_BUDGET` headroom: B1 already raised this 2400→2816 to fit `semantics` +
`budget_bytes`. B2 adds ~3 more small int fields (`"budget_ms":900,"observed_p99_us":0,
"observed_samples":0,` ≈ 55 bytes). Re-measure after landing (call `describe` on the
longest-`semantics`/longest-`summary` leaf — `discover schema` catalog scan or just grep the
longest `.semantics`/`.summary` string literal in `config/commands/*.def`); bump
`ZCL_COMMAND_SPEC_BUDGET` again (e.g. to ~2944, next round-ish number with headroom) only if a
real describe call truncates.

## 6. Merge order with OS-B1 (the dependency, stated precisely)

**Do not start B2 implementation until B1 is merged to `main`.** B1's worktree is
`.claude/worktrees/agent-aba54b0f6f26b21bc` (branch `worktree-agent-aba54b0f6f26b21bc`, currently
uncommitted local changes on top of `main @ 6405cf48d`, files: `Makefile`,
`config/commands/{accounts,apps,code,core,dev,ops,root}.def`, `config/src/command_catalog.c`,
`lib/kernel/include/kernel/command_registry.h`, `lib/kernel/src/command_registry.c`,
`lib/test/src/test_command_registry_catalog.c`, new `tools/lint/check_command_contract.sh`).

Exact overlap inventory (only 2 of 6 touched files overlap; both are cleanly separable):

| File | B1 touches | B2 touches | Overlap? |
|---|---|---|---|
| `command_registry.h` | struct fields `semantics`/`budget_bytes` (after `summary`/`compat_target`, struct lines ~235/243) + `ZCL_COMMAND_SPEC_BUDGET` bump | new `latency` enum region (~line 90), untouched by B1 | none — different regions |
| `command_registry.c` | `validate()` (~561), `describe_json()` policy chain (~1005-1060), `serialize_reply()` tail `contract` calc (~1372-1392) | new ring block (after line 18), `execute_json()` body (~1398/1499), `serialize_reply()` **top** (~1358-1365), `describe_json()` **same policy chain B1 touched** — see §5, this is the ONE real sequencing point | `describe_json` only — resolved by authoring B2's insertion relative to B1's already-landed `budget_bytes` push (§5), not to today's line numbers |
| `command_catalog.c` | 7 macro bodies (add `semantics_`/`budget_` params) | tail-only: one `_Static_assert` after `g_catalog_registry` (line ~245) | none |
| `Makefile` | new gate `check-command-contract` | none (B2 adds no lint gate — the ms mapping is total/pure, nothing to ratchet at the `.def` level) | none |
| `*.def` (7 files) | `semantics`/`budget_` args on every leaf macro call | none | none |
| `test_command_registry_catalog.c` | 3 new test fns (`test_semantics_contract_negative`, `test_leaf_semantics_and_budget`, `test_describe_emits_semantics`), appended before `test_domain_leaf_counts` | 3 new test fns appended after B1's (§7) | none — pure append, different functions |

**The single real risk is `describe_json`**: both lanes edit the same `&&`-chained
`json_push_kv_int(&policy, ...)` sequence. It is NOT a textual merge conflict in the git sense
(B1 will already be on `main` when B2 branches — B2 is authored sequentially, not in parallel
worktree isolation against a stale base) — it is a **design sequencing requirement**: B2's
recipe above is written as "insert after B1's `budget_bytes` push," not as an absolute line
number, specifically so the implementer re-reads the merged `describe_json` and finds that exact
anchor string (`"budget_bytes"`) rather than trusting a line number that has already moved once.

Consensus-parity note: none of this touches `domain/` or any consensus-adjacent code — pure
kernel/registry metadata and diagnostics, no relation to Equihash/Sapling/UTXO/tx validity.

## 7. Test — the acceptance bar

Add to `lib/test/src/test_command_registry_catalog.c`, appended to the group aggregator
`test_command_registry_catalog()` (today ends with `failures += test_root_menu_budget();` and
more — add after B1's three new calls, keeping the append-only pattern B1 itself used).

**7a. Pure mapping test** (no dispatch, no registry):

```c
static int test_latency_budget_mapping(void)
{
    int failures = 0;
    TEST("latency enum maps to the documented ms budget, total over the enum") {
        ASSERT_EQ(zcl_command_latency_budget_ms(ZCL_COMMAND_LATENCY_INSTANT), 50);
        ASSERT_EQ(zcl_command_latency_budget_ms(ZCL_COMMAND_LATENCY_FAST), 250);
        ASSERT_EQ(zcl_command_latency_budget_ms(ZCL_COMMAND_LATENCY_FOREGROUND), 750);
        ASSERT_EQ(zcl_command_latency_budget_ms(ZCL_COMMAND_LATENCY_BACKGROUND), 900);
        ASSERT_EQ(zcl_command_latency_budget_ms(ZCL_COMMAND_LATENCY_PERSISTENT), 900);
        ASSERT_EQ(zcl_command_latency_budget_ms((enum zcl_command_latency)999), 900);
        PASS();
    } _test_next:;
    return failures;
}
```

**7b. Envelope carries the three new fields, on a leaf that needs no live node**
(`discover.help` — zero I/O, `ZCL_COMMAND_LATENCY_INSTANT`, mirrors the existing `exec_leaf`
helper at line 28-47, no RPC mock needed):

```c
static int test_envelope_carries_latency_contract(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    char out[ZCL_COMMAND_RESULT_BUDGET + 1];
    TEST("zcl.result.v1 carries budget_ms/elapsed_ms/budget_exceeded") {
        const struct zcl_command_spec *s = find_spec(reg, "discover.help");
        ASSERT(s != NULL);
        enum zcl_command_exit code = ZCL_COMMAND_EXIT_INTERNAL;
        ASSERT(exec_leaf(reg, s, out, sizeof(out), &code));
        ASSERT(strstr(out, "\"budget_ms\"") != NULL);
        ASSERT(strstr(out, "\"elapsed_ms\"") != NULL);
        ASSERT(strstr(out, "\"budget_exceeded\":false") != NULL);
        PASS();
    } _test_next:;
    return failures;
}
```

**7c. The acceptance bar itself — "every READY read leaf meets its bucket."** Scoped
deliberately to the `discover.*` and `code.*` domains: both are local, deterministic,
`ZCL_COMMAND_TRAIT_DETERMINISTIC`-tagged, RPC-free leaves (verified — `code.def` and the
`discover.*` entries in `root.def` never call the legacy RPC bridge; contrast with most
`core.*` leaves, which bridge to a live node's RPC port via `zcl_native_bridge_command` and
would need the same per-leaf mock hook `test_status_brief_flat_lean_envelope` already uses for
`core.status.brief` — extending this sweep to `core.*` under mocks is named, explicit follow-on
work below, not silently dropped):

```c
static int test_ready_read_leaves_meet_latency_bucket(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    TEST("every READY discover.*/code.* leaf's dispatch meets its latency bucket") {
        for (size_t i = 0; i < reg->count; i++) {
            const struct zcl_command_spec *s = &reg->commands[i];
            if (s->availability != ZCL_COMMAND_READY ||
                s->effect != ZCL_COMMAND_EFFECT_READ ||
                s->mode != ZCL_COMMAND_MODE_SYNC)
                continue;
            bool in_scope = strncmp(s->path, "discover.", 9) == 0 ||
                            strncmp(s->path, "code.", 5) == 0;
            if (!in_scope)
                continue;
            char out[ZCL_COMMAND_RESULT_BUDGET + 1];
            enum zcl_command_exit code = ZCL_COMMAND_EXIT_INTERNAL;
            /* Dispatch with an empty object; leaves needing a required
             * positional (e.g. code.tests' `path`) fail input validation
             * FAST (before any I/O) — still a valid latency measurement,
             * ok=false is expected and NOT asserted here. */
            ASSERT(exec_leaf(reg, s, out, sizeof(out), &code));
            ASSERT(strstr(out, "\"budget_exceeded\":false") != NULL);
        }
        PASS();
    } _test_next:;
    return failures;
}
```

**7d. p99 surfaces in `discover describe`** (in-process accumulation, per §2's caveat —
dispatch the same leaf several times in this ONE test process, then describe it):

```c
static int test_describe_emits_observed_p99(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    TEST("describe surfaces observed_p99_us/observed_samples after repeated dispatch") {
        const struct zcl_command_spec *s = find_spec(reg, "discover.help");
        ASSERT(s != NULL);
        char out[ZCL_COMMAND_RESULT_BUDGET + 1];
        enum zcl_command_exit code = ZCL_COMMAND_EXIT_INTERNAL;
        for (int i = 0; i < 10; i++)
            ASSERT(exec_leaf(reg, s, out, sizeof(out), &code));
        char describe_out[ZCL_COMMAND_SPEC_BUDGET + 1];
        size_t n = zcl_command_registry_describe_json(reg, "discover.help",
                                                       describe_out,
                                                       sizeof(describe_out));
        ASSERT(n > 0);
        ASSERT(strstr(describe_out, "\"observed_p99_us\"") != NULL);
        ASSERT(strstr(describe_out, "\"observed_samples\":10") != NULL ||
               strstr(describe_out, "\"observed_samples\":11") != NULL);
        PASS();
    } _test_next:;
    return failures;
}
```

(`observed_samples` may be 10 or 11 depending on whether an earlier test in the same binary
already dispatched `discover.help` and left ring state — tests in one binary share the static
`g_latency_rings`; assert `>= 10`, not `== 10`, if test execution order isn't otherwise pinned —
check `ASSERT_GE` availability in `test/test_helpers.h` or use a plain `>=` comparison via
`json_get_int`.)

**Gate:** these five new functions run under the existing `test_command_registry_catalog` group
— no new `make lint` gate, no new test-group registration (`check-test-registration`). Run via
`make test-parallel` and confirm the group's `N passed, M failed` line. Also re-run the full
`make lint` once (nothing here should trip `check-command-contract`, `check-doc-accuracy`, or
`check-lib-layering`, but B1's fresh gate is new enough this session to be worth a direct
confirmation).

## 8. Docs to touch (accuracy, not scope)

`docs/NATIVE_COMMAND_INTERFACE.md` §8 "Execution result" (line 365-385) shows the
`zcl.result.v1` example JSON with `elapsed_us` only — add `budget_ms`/`elapsed_ms`/
`budget_exceeded` to the example so `check-doc-accuracy` (E11, `tools/scripts/
check_doc_accuracy.sh`) doesn't drift from reality; check what that script actually validates
before assuming it enforces byte-for-byte JSON — if it doesn't, update anyway for legibility
(`docs/AGENT_TRAPS.md` doctrine: docs rot, but this one is cheap to keep honest).

## 9. Explicit follow-on (not in B2's scope — name it so it isn't silently dropped)

- **Cross-process p99 persistence.** Today's ring is process-local (§2 caveat). A durable
  version (mmap'd fixed-size file under the datadir, or folded into `progress.kv`-style
  append-only state) would make `discover describe`'s p99 meaningful for the plain per-invocation
  CLI, not just long-lived processes. Not blocking B2's acceptance bar (an in-process fixture
  test), but the honest next step once B3b wires REST through this same execute path (a
  persistent server process would make the in-process ring immediately far more useful even
  without persistence).
- **`core.*` coverage for the acceptance sweep (§7c).** Extending the "every READY read leaf"
  test to the RPC-bridged `core.*` domain requires either mocking every bridged leaf's RPC call
  (per-leaf `mcp_rpc_client_set_test_hook`, following `test_status_brief_flat_lean_envelope`'s
  pattern) or running against a live fixture node. Scope, don't silently narrow: file as a
  follow-on once B2 lands, not folded in here.
- **Legacy `agent_first_call.h` deletion.** Not touched by B2 (still consumed by 5 MCP
  controllers pending zero-MCP W2/W3). Once those controllers are deleted in W3, delete
  `agent_first_call.h`/`agent_first_call.c` too — the kernel envelope now carries the same
  contract natively. Do not delete early; W3 is a separate, later, named step
  (`docs/work/MCP-REMOVAL-WORKLIST.md`).
