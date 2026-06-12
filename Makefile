# ZClassic C23 Full Node
# Copyright 2026 Rhett Creighton - Apache License 2.0

CC = cc
# <short-hash>[-dirty] — the -dirty suffix means the binary contains
# uncommitted tracked changes, so the hash alone does NOT identify the code
# (a binary built minutes before its fix was committed reports the parent
# commit; that ambiguity cost a live-deploy verification detour 2026-06-12).
BUILD_COMMIT := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)$(shell git diff-index --quiet HEAD -- 2>/dev/null || echo -dirty)
BUILD_DIR = build
BIN_DIR = $(BUILD_DIR)/bin
OBJ_DIR = $(BUILD_DIR)/obj

# ZCL_BUILD_COMMIT is a -D macro, so a new commit never dirties any .o; a TU
# reports whatever HEAD was when IT last recompiled. With the getter inlined
# in every caller, version reporters inside one binary disagreed about which
# commit was running (zcl_status top-level vs health, 2026-06-12). The getter
# now lives only in lib/util/src/clientversion.c, and this stamp file —
# rewritten only when BUILD_COMMIT changes — forces that one object stale.
BUILD_COMMIT_STAMP := $(BUILD_DIR)/build_commit.stamp
$(shell mkdir -p $(BUILD_DIR); \
  [ "`cat $(BUILD_COMMIT_STAMP) 2>/dev/null`" = "$(BUILD_COMMIT)" ] || \
  printf '%s' "$(BUILD_COMMIT)" > $(BUILD_COMMIT_STAMP))

ZCLASSIC23_BIN = $(BIN_DIR)/zclassic23
TEST_ZCL_BIN = $(BIN_DIR)/test_zcl
TEST_PARALLEL_BIN = $(BIN_DIR)/test_parallel
ZCLASSIC_CLI_BIN = $(BIN_DIR)/zclassic-cli
ZCL_RPC_BIN = $(BIN_DIR)/zcl-rpc
ZCL_NODECTL_BIN = $(BIN_DIR)/zcl-nodectl
WAL_CHECKPOINT_BIN = $(BIN_DIR)/wal_checkpoint
SOAK_RUNNER_BIN = $(BIN_DIR)/soak_runner
CRASH_RECOVERY_TEST_BIN = $(BIN_DIR)/crash_recovery_test
P2_INVARIANT_CHECK_BIN = $(BIN_DIR)/p2_invariant_check
ZCLASSIC23_CHAOS_BIN = $(BIN_DIR)/zclassic23-chaos

# App layer (MVC)
APP_DIRS = models controllers views services supervisors conditions jobs events
APP_INCLUDES = $(foreach d,$(APP_DIRS),-Iapp/$(d)/include)
APP_SRCS = $(foreach d,$(APP_DIRS),$(wildcard app/$(d)/src/*.c))

# Config layer
CONFIG_INCLUDES = -Iconfig/include
CONFIG_SRCS = $(wildcard config/src/*.c)

# Library layer
LIB_MODULES = bloom chain coins consensus core crypto crypto_registry encoding event framework health kernel \
	json keys metrics mining net platform policy primitives rpc script sim storage \
	support sync util validation wallet sapling zslp znam
LIB_INCLUDES = $(foreach m,$(LIB_MODULES),-Ilib/$(m)/include)
LIB_SRCS = $(foreach m,$(LIB_MODULES),$(wildcard lib/$(m)/src/*.c))

# Ports layer (Clean Architecture / Hexagonal interface headers).
# Headers only — adapters that implement these interfaces live elsewhere.
# See ports/include/ports/README.md for the convention.
PORTS_INCLUDES = -Iports/include

# Domain layer (pure, framework-free, no I/O).
# Bounded contexts under domain/<context>/ each expose include/domain/<context>/.
DOMAIN_CONTEXTS = consensus wallet encoding
DOMAIN_INCLUDES = $(foreach c,$(DOMAIN_CONTEXTS),-Idomain/$(c)/include)
DOMAIN_SRCS = $(foreach c,$(DOMAIN_CONTEXTS),$(wildcard domain/$(c)/src/*.c))

# Application layer (use cases / service objects).
# May depend on domain/, ports/, primitives, util — never on adapters or I/O.
APPLICATION_CONTEXTS = consensus
APPLICATION_INCLUDES = $(foreach c,$(APPLICATION_CONTEXTS),-Iapplication/$(c)/include)
APPLICATION_SRCS = $(foreach c,$(APPLICATION_CONTEXTS),$(wildcard application/$(c)/src/*.c))

# Adapters layer (port implementations).
# Outbound adapters implement the port interfaces. Inbound surfaces currently
# live in app/controllers, tools/mcp, and tools/cli until a real adapter shape
# is introduced.
ADAPTERS_INCLUDES = -Iadapters/outbound/persistence/include
ADAPTERS_SRCS = $(wildcard adapters/outbound/persistence/src/*.c)

# MCP router + future controllers (schema-driven tool dispatch)
MCP_INCLUDES = -Itools
MCP_SRCS = $(wildcard tools/mcp/*.c) $(wildcard tools/mcp/controllers/*.c) \
	$(wildcard tools/mcp/views/*.c)

ALL_SRCS = $(APP_SRCS) $(CONFIG_SRCS) $(LIB_SRCS) $(DOMAIN_SRCS) $(APPLICATION_SRCS) $(ADAPTERS_SRCS) $(MCP_SRCS)
ALL_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(ALL_SRCS))

GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0 2>/dev/null)
GTK_DEF    := $(if $(GTK_CFLAGS),-DHAVE_GTK,)
WEBKIT_CFLAGS := $(shell pkg-config --cflags webkit2gtk-4.1 2>/dev/null)
WEBKIT_LIBS   := $(shell pkg-config --libs webkit2gtk-4.1 2>/dev/null)
WEBKIT_DEF    := $(if $(WEBKIT_CFLAGS),-DHAVE_WEBKIT,)

CFLAGS = -std=c23 -O3 -march=native -flto=auto -Wall -Wextra -Werror -pedantic \
	-fstack-protector-strong \
	-Wno-stringop-overflow -Wno-unused-result \
	$(APP_INCLUDES) $(CONFIG_INCLUDES) $(LIB_INCLUDES) $(PORTS_INCLUDES) $(DOMAIN_INCLUDES) $(APPLICATION_INCLUDES) $(ADAPTERS_INCLUDES) $(MCP_INCLUDES) \
	-Ilib/test/include \
	-D_POSIX_C_SOURCE=200809L -DZCL_AR_ENFORCE -DZCL_BUILD_COMMIT=\"$(BUILD_COMMIT)\" -Ivendor/include $(GTK_DEF) $(GTK_CFLAGS) \
	$(WEBKIT_DEF) $(WEBKIT_CFLAGS)
LDFLAGS = -pthread -flto=auto -rdynamic
# Use vendor/tor/libtor.a when Tor is built from source.
# Tor: use full Tor if built, otherwise fall back to stub.
TOR_FULL = $(wildcard vendor/tor/libtor.a \
	vendor/tor/src/ext/ed25519/donna/libed25519_donna.a \
	vendor/tor/src/ext/ed25519/ref10/libed25519_ref10.a \
	vendor/tor/src/ext/keccak-tiny/libkeccak-tiny.a)
TOR_LIBS = $(if $(TOR_FULL),$(TOR_FULL),-Lvendor/lib -ltor_stub)
# All dependencies bundled in vendor/lib as static archives.
# Zero system library requirements beyond libc.
# OpenSSL 3.0 (Apache 2.0), libevent, zlib — all vendored.
LIBS = -Lvendor/lib -lsecp256k1 -lleveldb \
	-lstdc++ -lm -lsqlite3 -ldl -lpthread \
	-levent -levent_openssl -levent_pthreads \
	-lssl -lcrypto -lz

.PHONY: all test test-e2e test-shielded-payment test-store-e2e clean deploy check-restart-follow \
        coverage coverage-clean docs-mcp docs-mcp-check ci audit release \
        bench bench-regress \
        lint check-malloc check-silent-errors check-raw-sqlite check-raw-malloc \
        check-coins-lookup-nullcheck check-observability-pairing \
        check-silent-errors-services check-silent-errors-controllers \
        check-before-save-hooks check-pthread-create check-model-validation \
        check-long-functions check-rpc-registrar check-lag-slo-observable \
        check-file-size-ceiling check-framework-filename-suffix \
        check-operator-needed-sink check-doc-accuracy \
        fuzz-ci-leaks \
        soak-smoke soak-7day soak-ci test-crash-bootstrap \
        test-two-node-peer-tip chaos chaos-clean

CLI_SRCS = lib/rpc/src/client.c lib/json/src/json.c lib/encoding/src/utilstrencodings.c
all: test_zcl zclassic23 zclassic-cli

TEST_SRCS = $(wildcard lib/test/src/*.c)
SPEC_SRCS = $(wildcard lib/test/spec/*.c)

# test.c and test_parallel.c each own their own main() — never both in
# one binary. test_parallel_zcl uses the latter + the same test/spec
# helpers as sequential test_zcl.
TEST_SRCS_NO_MAIN = $(filter-out lib/test/src/test.c lib/test/src/test_parallel.c, $(TEST_SRCS))

# Generate templates from .chtml and .ccss files
TMPL_GEN = app/views/include/views/wallet_templates_gen.h
TMPL_SRC = $(wildcard app/views/templates/*.chtml) $(wildcard app/views/css/*.ccss)
TMPL_TOOL = $(BIN_DIR)/gen_templates

$(TMPL_TOOL): tools/gen_templates.c lib/util/src/safe_alloc.c
	@mkdir -p $(dir $@)
	$(CC) -std=c23 -O2 -Wall -Wextra -Ilib/util/include -o $@ $^

$(BIN_DIR)/inspect_html: tools/inspect_html.c
	@mkdir -p $(dir $@)
	$(CC) -std=c23 -O2 -Wall -Wextra -o $@ $<

$(TMPL_GEN): $(TMPL_SRC) $(TMPL_TOOL)
	$(TMPL_TOOL) app/views/templates $@ app/views/css

.PHONY: templates
templates: $(TMPL_GEN)

.PHONY: tools/gen_templates tools/inspect_html
tools/gen_templates: $(TMPL_TOOL)
tools/inspect_html: $(BIN_DIR)/inspect_html

# Build a tool/test binary that links against the full node library stack
# (Tor, OpenSSL, libevent, GTK, WebKit). Used by 8 binaries to keep the
# recipe in one place — a new tool becomes one $(eval $(call ...)) line and
# cannot drift on flags.
#   $(1) = target name (e.g., wallet_dump)
#   $(2) = entry source(s) — single file or whitespace-separated list
#   $(3) = extra link libs (e.g., -lm); empty by default
#   $(4) = extra CFLAGS (e.g., -DZCL_TESTING); empty by default
define BUILD_NODE_TOOL
.PHONY: $(1)
$(1): $$(BIN_DIR)/$(1)
$$(BIN_DIR)/$(1): $$(TMPL_GEN) $$(BUILD_COMMIT_STAMP) $(2) $$(ALL_SRCS)
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CFLAGS) $(4) -Wno-deprecated-declarations $$(LDFLAGS) -o $$@ $$(filter-out $$(TMPL_GEN) $$(BUILD_COMMIT_STAMP),$$^) $$(TOR_LIBS) $$(LIBS) $$(GTK_LIBS) $$(WEBKIT_LIBS) $(3)
endef

CHAOS_SIM_SRCS = tools/sim/sim_peer.c

$(eval $(call BUILD_NODE_TOOL,test_zcl,$(TEST_SRCS_NO_MAIN) lib/test/src/test.c $(SPEC_SRCS) $(CHAOS_SIM_SRCS),,-DZCL_TESTING))
$(eval $(call BUILD_NODE_TOOL,test_parallel,$(TEST_SRCS_NO_MAIN) lib/test/src/test_parallel.c $(SPEC_SRCS) $(CHAOS_SIM_SRCS),,-DZCL_TESTING))

.PHONY: test-parallel
test-parallel: test_parallel
	ulimit -s unlimited && $(TEST_PARALLEL_BIN)

# ── Fast inner loop ──────────────────────────────────────────────────────
# The edit -> check -> test loop runs dozens of times per session. Use these,
# NOT `make` + `build/bin/test_zcl` (8-15 min) and NOT a bare `build/bin/test_parallel`.
#
# THE REBUILD TRAP: plain `make` does NOT rebuild test_parallel (it is not in
# the default `all`), so running build/bin/test_parallel directly after editing a test
# can false-green an old binary or report "matched no groups" for a new test.
# `make t ONLY=<group>` always rebuilds the harness first, closing that trap.
.PHONY: t syntax-check build-only lint-fast

# Run ONE test group, always rebuilding the harness first:
#   make t ONLY=service_state_driver
t: test_parallel
	@if [ -z "$(ONLY)" ]; then \
	  echo "usage: make t ONLY=<group-substr>   (e.g. make t ONLY=stage_reducer_unwedge)"; \
	  exit 2; fi
	ulimit -s unlimited && $(TEST_PARALLEL_BIN) --only=$(ONLY)

# Incremental compile-check of the whole node (no link). Only changed TUs
# recompile — the fastest "does my change still build" signal. The
# -Wno-deprecated-declarations matches the real node/test build (zclassic23,
# test_parallel) so these targets don't false-fail on pre-existing deprecations.
build-only: CFLAGS += -Wno-deprecated-declarations
build-only: $(TMPL_GEN) $(ALL_OBJS)
	@echo "build-only: all node objects compiled"

# Full no-link syntax check across every TU in one shot (no incremental state).
syntax-check: $(TMPL_GEN)
	@$(CC) $(CFLAGS) -Wno-deprecated-declarations -fsyntax-only $(ALL_SRCS) src/main.c && echo "syntax-check: OK"

# The highest-signal lint gates for the inner loop. Run full `make lint` at
# sub-wave boundaries / before commit.
lint-fast: check-raw-sqlite check-malloc check-silent-errors check-model-validation check-one-write-path
	@echo "lint-fast: OK"

# ── Live-truth diagnosis + safe reproduction ─────────────────────────────
# diagnose-gap: one-shot three-orthogonal-views dump + root-cause verdict over
#   the RUNNING node (live or a repro copy). LIVE TRUTH BEFORE DESIGN.
#     make diagnose-gap SLUG=mystall
#     ZCL_RPCPORT=18299 ZCL_DATADIR=<copy> make diagnose-gap SLUG=onacopy
# repro-on-copy: snapshot the live datadir to a throwaway COPY and run the node
#   against it on an isolated port — validate consensus/recovery fixes BEFORE
#   they can touch the live chain; FAILS LOUD on a tip regression.
#     make repro-on-copy SLUG=import-reset ARGS='-nobgvalidation'
.PHONY: diagnose-gap repro-on-copy
diagnose-gap:
	@tools/diagnose_gap.sh $(SLUG)

repro-on-copy:
	@tools/repro_on_copy.sh $(SLUG) $(if $(ARGS),-- $(ARGS),)

$(eval $(call BUILD_NODE_TOOL,spec_zcl,lib/test/spec_main.c $(SPEC_SRCS) lib/test/src/test_helpers.c))
$(eval $(call BUILD_NODE_TOOL,wallet_dump,tools/wallet_dump.c))

$(BIN_DIR)/session: $(TMPL_GEN) $(BUILD_COMMIT_STAMP) tools/session.c $(ALL_SRCS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-deprecated-declarations $(LDFLAGS) -o $@ $(filter-out $(TMPL_GEN) $(BUILD_COMMIT_STAMP),$^) $(TOR_LIBS) $(LIBS) $(GTK_LIBS) $(WEBKIT_LIBS) -lm

session: $(BIN_DIR)/session
	$(BIN_DIR)/session

$(BIN_DIR)/bot: $(TMPL_GEN) $(BUILD_COMMIT_STAMP) tools/bot.c $(ALL_SRCS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-deprecated-declarations $(LDFLAGS) -o $@ $(filter-out $(TMPL_GEN) $(BUILD_COMMIT_STAMP),$^) $(TOR_LIBS) $(LIBS) $(GTK_LIBS) $(WEBKIT_LIBS) -lm

bot: $(BIN_DIR)/bot
	$(BIN_DIR)/bot

mock_rpc: $(BIN_DIR)/mock_rpc
$(BIN_DIR)/mock_rpc: tools/mock_rpc.c
	@mkdir -p $(dir $@)
	$(CC) -std=c23 -O2 -Wall -Wextra -Werror -pthread -o $@ $<

$(eval $(call BUILD_NODE_TOOL,wallet_sim,tools/wallet_sim.c))
$(eval $(call BUILD_NODE_TOOL,wallet_check,tools/wallet_check.c,-lm))
$(eval $(call BUILD_NODE_TOOL,rebuild_recent,tools/rebuild_recent.c,-lm,-fopenmp))

.PHONY: sim dump check-wallet
sim: wallet_sim
	$(BIN_DIR)/wallet_sim
dump: wallet_dump
	$(BIN_DIR)/wallet_dump

check-wallet: wallet_check
	$(BIN_DIR)/wallet_check

.PHONY: spec
spec: spec_zcl
	ulimit -s unlimited && $(BIN_DIR)/spec_zcl

.PHONY: zclassic23
zclassic23: $(ZCLASSIC23_BIN)
$(ZCLASSIC23_BIN): $(TMPL_GEN) $(BUILD_COMMIT_STAMP) src/main.c tools/mcp_server.c $(ALL_SRCS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-deprecated-declarations $(LDFLAGS) -o $@ $(filter-out $(TMPL_GEN) $(BUILD_COMMIT_STAMP),$^) $(TOR_LIBS) $(LIBS) $(GTK_LIBS) $(WEBKIT_LIBS)
	strip -s $@

.PHONY: zclassic-cli
zclassic-cli: $(ZCLASSIC_CLI_BIN)
$(ZCLASSIC_CLI_BIN): src/cli.c $(CLI_SRCS) lib/util/src/safe_alloc.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lm

# In-tree WAL checkpoint tool used by `deploy`.  Replaces a dependency on
# the sqlite3(1) CLI that isn't installed by default on stock Ubuntu/Debian
# (only libsqlite3-0) — was P12.4 in AGENT.md.  Calls
# sqlite3_wal_checkpoint_v2(TRUNCATE) on the open DB and exits non-zero on
# failure so `make deploy` halts loudly instead of silently skipping the
# checkpoint.
.PHONY: tools/wal_checkpoint
tools/wal_checkpoint: $(WAL_CHECKPOINT_BIN)
$(WAL_CHECKPOINT_BIN): tools/wal_checkpoint.c
	@mkdir -p $(dir $@)
	$(CC) -std=c23 -O2 -Wall -Wextra -Werror -Ivendor/include -o $@ $< \
	    -Lvendor/lib -l:libsqlite3.a -lpthread -ldl -lm

$(eval $(call BUILD_NODE_TOOL,wallet-wireframes,tools/wallet_wireframes.c))
$(eval $(call BUILD_NODE_TOOL,speedrun,tools/speedrun.c))

.PHONY: zcl-rpc
zcl-rpc: $(ZCL_RPC_BIN)
$(ZCL_RPC_BIN): tools/zcl-rpc.c
	@mkdir -p $(dir $@)
	$(CC) -std=c23 -O2 -Wall -o $@ $<

# gen_sha3_windows: one-shot tool that queries a fully-synced reference
# node and overwrites lib/chain/{include/chain,src}/sha3_windows.{h,c}
# with SHA3-256 commitments over 1000-block windows. Standalone build:
# only the libs it directly uses, no DB, no Tor.
.PHONY: tools/gen_sha3_windows
tools/gen_sha3_windows: $(BIN_DIR)/gen_sha3_windows
$(BIN_DIR)/gen_sha3_windows: tools/gen_sha3_windows.c \
		lib/chain/src/sha3_windows.c \
		lib/crypto/src/sha3.c lib/encoding/src/utilstrencodings.c \
		lib/json/src/json.c lib/platform/src/clock.c
	@mkdir -p $(dir $@)
	$(CC) -std=c23 -O3 -march=native -Wall -Wextra -Werror -pedantic \
	    -Wno-stringop-overflow -Wno-unused-result \
	    -Ilib/chain/include -Ilib/crypto/include -Ilib/encoding/include \
	    -Ilib/json/include -Ilib/platform/include -Ilib/util/include \
	    -D_POSIX_C_SOURCE=200809L \
	    -o $@ $^ -pthread

# gen_utxo_snapshot: build-time tool that walks a legacy zclassicd
# chainstate LevelDB and emits a canonical UTXO sidecar file ready
# for runtime mmap+SHA3-verify+bulk-INSERT (Stage J of fast-sync
# plan). Implemented as a `--gen-utxo-snapshot` mode of zclassic23
# itself (avoids duplicating the dep tree); invoke via:
#   zclassic23 --gen-utxo-snapshot <legacy_datadir> <out_path>

.PHONY: zcl-nodectl
zcl-nodectl: $(ZCL_NODECTL_BIN)
$(ZCL_NODECTL_BIN): tools/zcl-nodectl.c lib/util/include/util/rpc_paths.h
	@mkdir -p $(dir $@)
	$(CC) -std=c23 -O2 -Wall -Wextra -Werror -Ilib/util/include -o $@ $<

.PHONY: export_snapshot
export_snapshot: $(BIN_DIR)/export_snapshot
$(BIN_DIR)/export_snapshot: tools/export_snapshot.c
	@mkdir -p $(dir $@)
	$(CC) -std=c23 -O2 -Wall -Wextra -Werror -Ivendor/include -o $@ $< -Lvendor/lib -l:libsqlite3.a -lpthread

.PHONY: zcl-browser
zcl-browser: $(BIN_DIR)/zcl-browser
$(BIN_DIR)/zcl-browser: tools/zcl-browser.c $(ALL_SRCS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-deprecated-declarations $$(pkg-config --cflags webkit2gtk-4.1) -o $@ $^ $(TOR_LIBS) $(LIBS) $$(pkg-config --libs webkit2gtk-4.1)

.PHONY: zcl-blog
zcl-blog: $(BIN_DIR)/zcl-blog
$(BIN_DIR)/zcl-blog: tools/zcl-blog
	@mkdir -p $(dir $@)
	$(CC) -std=c23 -O2 -x c $$(pkg-config --cflags webkit2gtk-4.1) -o $@ $< $$(pkg-config --libs webkit2gtk-4.1)

explorer-css: app/views/src/explorer_css.css
	python3 -c "\
	import re; f=open('app/views/src/explorer_css.css'); css=f.read(); f.close(); \
	css=re.sub(r'/\*.*?\*/', '', css, flags=re.DOTALL); \
	css=re.sub(r'\s+', ' ', css).strip(); css=re.sub(r'\s*([{}:;,])\s*', r'\1', css); \
	css=css.replace('\\\\','\\\\\\\\').replace('\"','\\\\\"'); \
	lines=[]; i=0; \
	exec('while i<len(css): lines.append(chr(32)*4+chr(34)+css[i:min(i+100,len(css))]+chr(34)); i+=100'); \
	o=open('app/views/include/views/explorer_css.h','w'); \
	o.write('/* Auto-generated from app/views/src/explorer_css.css */\n'); \
	o.write('#ifndef EXPLORER_CSS_H\n#define EXPLORER_CSS_H\n\n'); \
	o.write('static const char explorer_css[] =\n'+'\n'.join(lines)+';\n\n#endif\n'); o.close()"

# Default `make test` = the fast fork-based parallel suite (~1min, 282 groups).
# The slow single-process binary is still available as `make test-full`.
# Doctrine: never run test_zcl in the inner loop — use `make t ONLY=<group>`.
test: test_parallel
	ulimit -s unlimited && $(TEST_PARALLEL_BIN)

test-full: test_zcl
	ulimit -s unlimited && $(TEST_ZCL_BIN)

.PHONY: zclassic23-chaos
zclassic23-chaos: $(ZCLASSIC23_CHAOS_BIN)
$(ZCLASSIC23_CHAOS_BIN): tools/sim/chaos.c tools/sim/sim_peer.c \
	lib/util/src/safe_alloc.c \
	lib/util/include/util/safe_alloc.h lib/net/src/net_fault.c \
	lib/net/include/net/net_fault.h lib/platform/src/clock.c \
	lib/platform/include/platform/clock.h lib/platform/include/platform/time_compat.h
	@mkdir -p $(dir $@)
	$(CC) -std=c23 -O2 -Wall -Wextra -Werror -pedantic \
	    -D_POSIX_C_SOURCE=200809L -Ilib/util/include -Ilib/net/include \
	    -Ilib/platform/include -Itools \
	    -o $@ tools/sim/chaos.c tools/sim/sim_peer.c \
	    lib/util/src/safe_alloc.c lib/net/src/net_fault.c \
	    lib/platform/src/clock.c

chaos: zclassic23-chaos
	@set -eu; \
	for s in tools/sim/scenarios/*.scenario; do \
	    echo "==> $$s"; \
	    $(ZCLASSIC23_CHAOS_BIN) --scenario="$$s"; \
	done; \
	echo "==> All chaos scenarios PASSED"

chaos-clean:
	rm -f $(ZCLASSIC23_CHAOS_BIN)
	rm -rf build/chaos-output/ chaos-output/

# Offline P2 self-heal invariant checker: coins_applied_height == utxo_apply
# cursor, read read-only from a progress.kv (works while the node is down —
# the kill-9 window). Self-contained against the vendored sqlite3 header.
.PHONY: p2_invariant_check
p2_invariant_check: $(P2_INVARIANT_CHECK_BIN)
$(P2_INVARIANT_CHECK_BIN): tools/p2_invariant_check.c vendor/include/sqlite3.h vendor/lib/libsqlite3.a
	@mkdir -p $(dir $@)
	$(CC) -std=c23 -O2 -Wall -Wextra -Werror -pedantic \
	    -D_POSIX_C_SOURCE=200809L -Ivendor/include \
	    -o $@ tools/p2_invariant_check.c \
	    -Lvendor/lib -l:libsqlite3.a -lpthread -ldl -lm

# Read-only SQL query CLI over any sqlite db (progress.kv, node.db, fixture
# datadirs). Python is banned and the host has no sqlite3 CLI; this is the
# shell-side diagnostic primitive (zcl_sql covers node.db via MCP only).
SQLQ_BIN = $(BIN_DIR)/sqlq
.PHONY: sqlq
sqlq: $(SQLQ_BIN)
$(SQLQ_BIN): tools/sqlq.c vendor/include/sqlite3.h vendor/lib/libsqlite3.a
	@mkdir -p $(dir $@)
	$(CC) -std=c23 -O2 -Wall -Wextra -Werror -pedantic \
	    -D_POSIX_C_SOURCE=200809L -Ivendor/include \
	    -o $@ tools/sqlq.c \
	    -Lvendor/lib -l:libsqlite3.a -lpthread -ldl -lm

# Crash recovery harness: fork zclassic23, SIGKILL at random points,
# restart, and assert data-integrity invariants. Needs a pre-seeded
# datadir (skips trivially if none exists — see tool header). Build
# depends on the node binary and the CLI RPC helper.
.PHONY: crash_recovery_test
crash_recovery_test: $(CRASH_RECOVERY_TEST_BIN)
$(CRASH_RECOVERY_TEST_BIN): tools/crash_recovery_test.c lib/platform/src/clock.c
	@mkdir -p $(dir $@)
	$(CC) -std=c23 -O2 -Wall -Wextra -Werror -pthread \
	    -Ilib/platform/include -Ilib/util/include -Ivendor/include -o $@ \
	    tools/crash_recovery_test.c lib/platform/src/clock.c \
	    -Lvendor/lib -l:libsqlite3.a -lpthread -ldl -lm

.PHONY: test-crash
# CI entry point for the crash recovery harness.
#
# The harness skips (exit 0) when its datadir does NOT exist, and keeps
# CI green on clean hosts. Don't pre-create the dir — an empty dir
# makes the harness try to start the node on a blank chainstate, which
# then reports "RPC never came up" as a harness error (false negative).
#
# When a fully seeded datadir is available (a CI worker with a pinned
# snapshot), point ZCL_CRASH_DATADIR at it and this target runs the
# full 10-iteration kill/restart cycle against real data. Otherwise
# the target runs through to the SKIP path.
test-crash: crash_recovery_test zclassic23 zcl-rpc
	@set -eu; \
	 dd="$${ZCL_CRASH_DATADIR:-/tmp/zcl-crashtest-ci.absent}"; \
	 if [ -n "$${ZCL_CRASH_DATADIR:-}" ] && [ ! -d "$$dd" ]; then \
	     echo "test-crash: ZCL_CRASH_DATADIR=$$dd does not exist — harness will SKIP"; \
	 fi; \
	 ZCL_CRASH_DATADIR="$$dd" $(CRASH_RECOVERY_TEST_BIN) --iterations=10

# ── Opt-in, node-spawning harnesses (NOT in `make ci`) ─────────────
#
# test-crash-bootstrap and soak-ci both SPAWN a real (isolated regtest)
# node. They are DELIBERATELY excluded from the default `ci:` recipe to
# keep CI hermetic and fast — `make ci` must never start a node. Run
# them explicitly (operator/agent) on a clean host, or in a dedicated
# slow-CI stage. Both source tools/scripts/isolated_node_env.sh, which
# is the single audited chokepoint enforcing /tmp datadir + 39xxx ports
# + refuse-on-live preflight + process-group kill + cleanup trap.
#
# C7 full-binary kill-9: the bootstrap path self-seeds a fresh isolated
# /tmp regtest datadir (mine N blocks), then runs kill/restart cycles
# asserting height-monotone + zero-UTXO-above-tip recovery. The `iso_*`
# helpers mint the datadir/ports and own the cleanup trap; the C harness
# does its own spawn/kill loop against them.
.PHONY: test-crash-bootstrap
# The recipe runs under bash (NOT the default /bin/sh=dash) because
# isolated_node_env.sh relies on `set -o pipefail`. The whole body is one
# bash invocation so the sourced trap stays armed for the harness run.
test-crash-bootstrap: crash_recovery_test zclassic23 zcl-rpc
	@bash -c 'set -euo pipefail; \
	 export ISO_KIND=crash ISO_PORT_BASE=39030; \
	 . tools/scripts/isolated_node_env.sh; \
	 iso_init; \
	 echo "test-crash-bootstrap: kill-9 cycles on $$ISO_DD (rpc=$$ISO_RPCPORT)"; \
	 $(CRASH_RECOVERY_TEST_BIN) \
	     --bootstrap-regtest \
	     --datadir="$$ISO_DD" \
	     --rpc-port="$$ISO_RPCPORT" \
	     --p2p-port="$$ISO_PORT" \
	     --fs-port="$$ISO_FSPORT" \
	     --https-port="$$ISO_HTTPSPORT" \
	     --connect=127.0.0.1:"$$ISO_CONNECT_SINK" \
	     --seed-blocks=30 \
	     --iterations=2 --min-delay-ms=200 --max-delay-ms=800 \
	     --verbose; \
	 echo "test-crash-bootstrap: PASS (recovery invariants held under SIGKILL)"'

# C7 PEER-tip kill-9 (the FULL #7 claim): two isolated regtest nodes on
# disjoint 39xxx quads, B connect-only to A. A mines; assert B syncs to A
# over NATIVE P2P, then kill-9 B mid-life, mine more on A, restart B, and
# assert B re-catches up to A's PEER-tip. This is the complement to
# test-crash-bootstrap (which only proves SINGLE-node boot recovery).
# DELIBERATELY opt-in (NOT in `make ci`) — it spawns two real nodes. The
# harness owns its own /tmp datadirs + 39xxx port refuse/LISTEN preflight
# + process-group SIGKILL + EXIT/INT/TERM cleanup trap (same discipline
# as tools/scripts/isolated_node_env.sh, generalized to two nodes).
.PHONY: test-two-node-peer-tip
# Runs under bash for `set -o pipefail` parity with the other spawn
# harnesses; the script itself sets -euo pipefail.
test-two-node-peer-tip: zclassic23 zcl-rpc
	@bash tools/scripts/two_node_peer_tip.sh

# C6 bounded compressed-soak PROXY: self-spawn an isolated /tmp regtest
# node, drive 180 s of generate-load, and assert the soak runner exits
# SOAK_OK (verdict=OK sentinel grepped so a no-op runner fails loud —
# false-green guard, same discipline as the mvp_gate macro). This is a
# hermetic CI green/red SIGNAL, NOT the real 168 h operational soak.
.PHONY: soak-ci
# Runs under bash for `set -o pipefail` (see test-crash-bootstrap note).
# `set +e` around the runner so a non-zero verdict is reported (not
# swallowed by errexit) before the false-green sentinel check.
soak-ci: soak_runner zclassic23 zcl-rpc
	@bash -c 'set -uo pipefail; \
	 export ISO_KIND=soak ISO_PORT_BASE=39040; \
	 . tools/scripts/isolated_node_env.sh; \
	 iso_init; \
	 log="$$ISO_DD/soak-ci.log"; \
	 echo "soak-ci: 180s compressed soak on $$ISO_DD (rpc=$$ISO_RPCPORT)"; \
	 set +e; \
	 out=$$($(SOAK_RUNNER_BIN) \
	     --ci-proxy \
	     --node-datadir="$$ISO_DD" \
	     --rpcport="$$ISO_RPCPORT" \
	     --p2p-port="$$ISO_PORT" \
	     --fs-port="$$ISO_FSPORT" \
	     --https-port="$$ISO_HTTPSPORT" \
	     --connect=127.0.0.1:"$$ISO_CONNECT_SINK" \
	     --interval-sec=5 \
	     --load=generate:5 \
	     --rpc=$(ZCL_RPC_BIN) \
	     --log="$$log" 2>&1); rc=$$?; set -e; \
	 echo "$$out"; \
	 if [ "$$rc" != "0" ]; then \
	     echo "soak-ci: FAILED (runner exit $$rc != SOAK_OK); log tail:"; \
	     tail -20 "$$log" 2>/dev/null || true; \
	     exit 1; \
	 fi; \
	 if ! echo "$$out" | grep -q "verdict=OK"; then \
	     echo "soak-ci: FALSE-GREEN GUARD — runner exited 0 but never printed verdict=OK (no-op?)"; \
	     exit 1; \
	 fi; \
	 echo "soak-ci: PASS (SOAK_OK — tip advanced under load, RSS plateaued)"'

# Always-fresh end-to-end MCP test.
#
# `test_mcp_e2e` forks the real `build/bin/zclassic23 -mcp` binary and asserts
# wire-level envelope shapes.  If the binary is older than the MCP
# source files the in-suite test SKIPs with a clear message rather
# than failing with a confusing tool-count mismatch — but that means a
# bare `build/bin/test_zcl` after editing MCP code can silently skip the e2e
# coverage.  Use `make test-e2e` to force a rebuild of zclassic23 (and
# test_zcl) before running, so the e2e suite always runs against the
# current source.
test-e2e: zclassic23 test_zcl
	ulimit -s unlimited && $(TEST_ZCL_BIN)

# P11.4 shielded-payment gate.
#
# Runs the real transparent->shielded wallet path inside test_zcl with
# Sapling proving params loaded from ~/.zcash-params. The target skips on
# hosts that do not have the proving/verifying params installed so CI can
# call it unconditionally without creating false negatives on clean workers.
test-shielded-payment: test_zcl
	@set -eu; \
	params_dir="$$HOME/.zcash-params"; \
	for f in sapling-spend.params sapling-output.params sprout-groth16.params sprout-verifying.key; do \
		if [ ! -r "$$params_dir/$$f" ]; then \
			echo "test-shielded-payment: SKIP ($$params_dir/$$f missing)"; \
			exit 0; \
		fi; \
	done; \
	ZCL_STRESS_TESTS=1 ZCL_TEST_ONLY=shielded_payment $(TEST_ZCL_BIN)

# P11.5 store end-to-end gate.
#
# Runs the store order -> payment reconciliation -> token access path inside
# test_zcl. This is deterministic and self-contained, but remains opt-in so
# the default suite does not pay extra setup/runtime cost.
test-store-e2e: test_zcl
	ZCL_STRESS_TESTS=1 ZCL_TEST_ONLY=store_e2e $(TEST_ZCL_BIN)

# ── MVP acceptance gates: hermetic vs fixture-bound ───────────
#
# The MVP acceptance tests (docs/MVP.md #2..#7) all self-skip unless
# ZCL_STRESS_TESTS=1. We split them by what a clean CI container can
# actually provide, so `make ci` only blocks on tests that truly run:
#
#   ci-mvp-gates  HERMETIC — no network, no params, no oracle. Runs in
#                 `make ci`. Each gate is invoked FOCUSED via
#                 ZCL_TEST_ONLY so we do NOT accidentally trip the
#                 non-hermetic onion gate by blanket-setting the env
#                 var on the whole suite.
#                   #3 cold_start             (in-process sync FSM, ~6-10s)
#                   #5 store_e2e              (local SQLite + store ctrl, sub-s)
#                   #7 kill9                  (fork+SIGKILL+SQLite, ~4-8s)
#                      chain_advance_atomicity (fork child procs, supports #7)
#
#   ci-stress     NON-HERMETIC — needs external resources a clean
#                 container lacks. NOT in `make ci`. Run where the
#                 resource exists (params staged / Tor egress allowed):
#                   #2 onion_bootstrap        (real Tor + dir-authority net)
#                   #4 shielded_payment       (~/.zcash-params, params-guarded)
#
# Each focused run is its own process, so a failure in one gate names
# exactly which MVP criterion regressed.
#
# Focused MVP gate (DRY): run ONE ZCL_TEST_ONLY selector hermetically and
# PROVE it actually ran that focused subset. test.c returns early only on a
# selector match, so an unknown/renamed selector silently falls through to the
# FULL suite — which under ZCL_STRESS_TESTS=1 runs the non-hermetic onion test
# and would hang CI while looking green. The sentinel grep converts that silent
# fall-through into a loud failure. Redirect (not pipe) so the test's real exit
# status survives on dash. $(1)=label $(2)=selector $(3)=unique sentinel.
define mvp_gate
@echo "══ $(1) ══"; l=$$(mktemp); if ! ZCL_STRESS_TESTS=1 ZCL_TEST_ONLY=$(2) $(TEST_ZCL_BIN) >"$$l" 2>&1; then cat "$$l"; rm -f "$$l"; echo "MVP GATE FAILED: $(1) (ZCL_TEST_ONLY=$(2) exited non-zero)"; exit 1; fi; cat "$$l"; if ! grep -qF "$(3)" "$$l"; then rm -f "$$l"; echo "MVP GATE FALSE-GREEN GUARD: $(1) — sentinel \"$(3)\" not printed; the ZCL_TEST_ONLY=$(2) selector likely no longer exists in lib/test/src/test.c so the full suite ran. Restore the selector or re-point this gate."; exit 1; fi; rm -f "$$l"
endef

.PHONY: ci-mvp-gates ci-stress
ci-mvp-gates: test_zcl
	$(call mvp_gate,MVP gate 3: cold-start sync FSM (hermetic),cold_start,=== Cold-start subset complete:)
	$(call mvp_gate,MVP gate 5: store end-to-end (hermetic),store_e2e,=== store e2e subset complete:)
	$(call mvp_gate,MVP gate 7: kill -9 recovery (hermetic),kill9,=== kill9 subset complete:)
	$(call mvp_gate,MVP support: chain-advance atomicity (hermetic),chain_advance_atomicity,=== chain_advance_atomicity subset complete:)
	$(call mvp_gate,MVP "it works": mined block -> reducer front door -> tip+1 (hermetic),reducer_ingest,=== reducer-ingest subset complete:)
	$(call mvp_gate,MVP gate 2 (slice): onion bootstrap <60s budget + v3 address (hermetic),onion_slice,=== onion_bootstrap_slice subset complete:)
	$(call mvp_gate,MVP gate 4 (slice): note encrypted to wallet ivk -> wallet decrypts -> z-balance (hermetic),shielded_receive,=== shielded_receive subset complete:)
	$(call mvp_gate,MVP forward-progress: N sequential blocks + heavier-fork reorg (hermetic),reducer_forward,=== reducer-forward subset complete:)
	$(call mvp_gate,MVP gate 8 (slice): consensus-parity mismatch-detection machinery (hermetic fixture),parity_slice,=== parity_slice subset complete:)
	@echo "══ MVP hermetic gates: ALL PASSED ══"

# mvp-it-works: the single "you know your app works" proof — boots a fresh
# in-process regtest reducer, mines one real Equihash (48,5) block, drives it
# through reducer_ingest_block (the same front door live intake uses), and
# asserts the authoritative tip advances by exactly 1 with the block's coinbase
# live in the UTXO set and the commitment moved. Runs isolated (fresh process)
# because it drives reducer process-globals; teeth-verified (fails if the
# reducer cannot finalize forward — the live-wedge failure mode).
.PHONY: mvp-it-works
mvp-it-works: test_zcl
	$(call mvp_gate,MVP "it works": one mined block through the reducer -> tip+1,reducer_ingest,=== reducer-ingest subset complete:)
	@echo "══ MVP it-works gate: PASSED ══"

# mvp-onion-slice (C2 hermetic half): proves the bootstrap readiness/<60s budget
# LOGIC + the v3 .onion address format check in-process. The real <60s gate
# stays in ci-stress (selector "onion"). Runs isolated (onion_service singleton).
.PHONY: mvp-onion-slice
mvp-onion-slice: test_zcl
	$(call mvp_gate,MVP gate 2 (slice): onion bootstrap <60s budget + v3 address,onion_slice,=== onion_bootstrap_slice subset complete:)
	@echo "══ MVP onion-slice gate: PASSED ══"

# mvp-shielded-receive (C4 hermetic half): encrypts a note to the wallet's ivk,
# drives wallet_try_sapling_decrypt, asserts z-balance reflects it (and a note
# to a foreign ivk is NOT credited). Params-free — decryption needs no proving key.
.PHONY: mvp-shielded-receive
mvp-shielded-receive: test_zcl
	$(call mvp_gate,MVP C4 (receive): note encrypted to wallet ivk -> wallet decrypts -> z-balance,shielded_receive,=== shielded_receive subset complete:)
	@echo "══ MVP shielded-receive gate: PASSED ══"

# mvp-forward-progress: the live-wedge repro gate — boots a fresh in-process
# regtest reducer, mines + ingests N=32 sequential blocks through the front
# door asserting MONOTONIC tip advance (no stall/oscillation), then a heavier
# near-tip fork and asserts a clean reorg with a consistent UTXO commitment.
# On a stall it captures the height + all 8 stage cursors. Runs isolated.
.PHONY: mvp-forward-progress
mvp-forward-progress: test_zcl
	$(call mvp_gate,MVP forward-progress: N sequential blocks + heavier-fork reorg through the reducer,reducer_forward,=== reducer-forward subset complete:)
	@echo "══ MVP forward-progress gate: PASSED ══"

# mvp-parity-slice (C8 hermetic slice): regression-protects the UTXO parity
# service's mismatch-detection machinery via the in-process fixture reference —
# a CONSISTENT set reports 0 mismatches, a REAL injected outpoint is DETECTED
# (the negative control). The FULL C8 claim (live zclassicd oracle parity over
# the soak window) still needs the oracle. Runs isolated (parity-service globals).
.PHONY: mvp-parity-slice
mvp-parity-slice: test_zcl
	$(call mvp_gate,MVP C8 (slice): consensus-parity mismatch-detection machinery,parity_slice,=== parity_slice subset complete:)
	@echo "══ MVP parity-slice gate: PASSED ══"

# ci-stress: the fixture/network-bound MVP gates. Run on a worker that
# has the resource (Tor egress for #2, ~/.zcash-params for #4). Reuses
# the params-probe in test-shielded-payment so a host missing the params
# SKIPs cleanly instead of failing — do NOT call this from `make ci`.
ci-stress: test_zcl
	@echo "══ MVP gate #2: onion bootstrap (needs Tor network) ══"
	ZCL_STRESS_TESTS=1 ZCL_TEST_ONLY=onion $(TEST_ZCL_BIN)
	@echo "══ MVP gate #4: shielded payment (needs ~/.zcash-params) ══"
	$(MAKE) test-shielded-payment

# ── MVP gate #1: hermetic single-binary install ───────────────
#
# ci-install: the C1 ("single-binary install on clean Ubuntu/Debian")
# proxy gate. It BUILDS, INSTALLs the node + zcl-rpc to a THROWAWAY /tmp
# prefix (the file-copy a real `make install` performs, to a disposable
# DESTDIR), then SPAWNS one fully isolated regtest node FROM that prefix
# via the single audited isolation chokepoint
# tools/scripts/isolated_node_env.sh (unique /tmp datadir + 39xxx
# non-live ports + -connect=39999 dead sink + process-group cleanup),
# polls RPC readiness with a bounded timeout, asserts the installed
# binary answers + bound ONLY non-live ports, and cleans everything up.
#
# DELIBERATELY opt-in (NOT in `make ci`) — like ci-stress / soak-ci it
# spawns a real process. It NEVER runs systemctl and NEVER touches a
# live port or the live datadir. Runs under bash because the sourced
# isolation harness relies on `set -o pipefail`.
.PHONY: ci-install
ci-install: zclassic23 zcl-rpc
	@bash tools/scripts/ci_install_gate.sh

# ── libFuzzer harnesses ───────────────────────────────────────
#
# Fuzz targets use clang + libFuzzer + ASan + UBSan. They compile
# the same ALL_SRCS as the main build (minus src/main.c), so the same
# code paths the node exercises are the code paths the fuzzer
# exercises. -O1 + -g because aggressive optimisation confuses
# sanitizer reports.
#
# `make fuzz` builds the three binaries. `make fuzz-ci` runs each
# for 60 seconds as a smoke test; CI uses this to detect already-
# latent crashes without chasing exhaustive coverage. If clang is
# unavailable, both targets print a skip message and exit 0 so
# gcc-only hosts never fail the build.
FUZZ_CC ?= clang
FUZZ_CFLAGS = -std=c23 -O1 -g -Wall -Wextra -Wno-unused-result \
	-Wno-deprecated-declarations \
	$(APP_INCLUDES) $(CONFIG_INCLUDES) $(LIB_INCLUDES) $(MCP_INCLUDES) \
	-Ilib/test/include -D_POSIX_C_SOURCE=200809L -Ivendor/include \
	-fsanitize=fuzzer,address,undefined \
	-fno-sanitize=alignment
FUZZ_LIBS = $(TOR_LIBS) $(LIBS)

FUZZ_TARGETS = $(BIN_DIR)/fuzz_block $(BIN_DIR)/fuzz_script $(BIN_DIR)/fuzz_p2p

.PHONY: fuzz fuzz-ci
fuzz: $(FUZZ_TARGETS)

.PHONY: fuzz_block fuzz_script fuzz_p2p
fuzz_block: $(BIN_DIR)/fuzz_block
fuzz_script: $(BIN_DIR)/fuzz_script
fuzz_p2p: $(BIN_DIR)/fuzz_p2p

$(BIN_DIR)/fuzz_block: tools/fuzz/fuzz_block.c $(TMPL_GEN) $(ALL_SRCS)
	@mkdir -p $(dir $@)
	@if ! command -v $(FUZZ_CC) >/dev/null 2>&1; then \
		echo "fuzz_block: $(FUZZ_CC) not found — SKIP (install clang for fuzzing)"; \
		touch $@; \
	else \
		echo "$(FUZZ_CC) ... -o $@"; \
		$(FUZZ_CC) $(FUZZ_CFLAGS) -o $@ tools/fuzz/fuzz_block.c $(ALL_SRCS) $(FUZZ_LIBS); \
	fi

$(BIN_DIR)/fuzz_script: tools/fuzz/fuzz_script.c $(TMPL_GEN) $(ALL_SRCS)
	@mkdir -p $(dir $@)
	@if ! command -v $(FUZZ_CC) >/dev/null 2>&1; then \
		echo "fuzz_script: $(FUZZ_CC) not found — SKIP"; \
		touch $@; \
	else \
		echo "$(FUZZ_CC) ... -o $@"; \
		$(FUZZ_CC) $(FUZZ_CFLAGS) -o $@ tools/fuzz/fuzz_script.c $(ALL_SRCS) $(FUZZ_LIBS); \
	fi

$(BIN_DIR)/fuzz_p2p: tools/fuzz/fuzz_p2p.c $(TMPL_GEN) $(ALL_SRCS)
	@mkdir -p $(dir $@)
	@if ! command -v $(FUZZ_CC) >/dev/null 2>&1; then \
		echo "fuzz_p2p: $(FUZZ_CC) not found — SKIP"; \
		touch $@; \
	else \
		echo "$(FUZZ_CC) ... -o $@"; \
		$(FUZZ_CC) $(FUZZ_CFLAGS) -o $@ tools/fuzz/fuzz_p2p.c $(ALL_SRCS) $(FUZZ_LIBS); \
	fi

fuzz-ci: $(FUZZ_TARGETS)
	@if ! command -v $(FUZZ_CC) >/dev/null 2>&1; then \
		echo "fuzz-ci: $(FUZZ_CC) not found — SKIP"; \
		exit 0; \
	fi; \
	set -e; \
	for t in $(FUZZ_TARGETS); do \
		echo "=== $$t (60s) ==="; \
		base=$$(basename "$$t"); kind="$${base#fuzz_}"; \
		seed_dir="lib/test/fuzz_seeds/$$kind"; \
		work_dir="/tmp/zcl_fuzz_$$kind"; \
		rm -rf "$$work_dir"; mkdir -p "$$work_dir"; \
		ASAN_OPTIONS=detect_leaks=0 $$t -max_total_time=60 \
			-timeout=1 -print_final_stats=1 "$$work_dir" "$$seed_dir"; \
		rm -rf "$$work_dir"; \
	done

# Same binaries with leak detection ON. Separate target so CI stays
# green while known-pre-existing leaks are being triaged; developers
# and Wave 4+ commits that fix leaks opt into this stricter run.
fuzz-ci-leaks: $(FUZZ_TARGETS)
	@if ! command -v $(FUZZ_CC) >/dev/null 2>&1; then \
		echo "fuzz-ci-leaks: $(FUZZ_CC) not found — SKIP"; \
		exit 0; \
	fi; \
	set -e; \
	for t in $(FUZZ_TARGETS); do \
		echo "=== $$t (60s, leak detection ON) ==="; \
		base=$$(basename "$$t"); kind="$${base#fuzz_}"; \
		seed_dir="lib/test/fuzz_seeds/$$kind"; \
		work_dir="/tmp/zcl_fuzz_$${kind}_leaks"; \
		rm -rf "$$work_dir"; mkdir -p "$$work_dir"; \
		$$t -max_total_time=60 -timeout=1 -print_final_stats=1 \
			"$$work_dir" "$$seed_dir"; \
		rm -rf "$$work_dir"; \
	done

# ── P11.6 — 7-day soak runner ─────────────────────────────────
#
# Separate binary that polls a running zclassic23 every 60 s
# against the analyzer in lib/test/src/soak_harness.c. Verdict
# failure (crash / tip-stall / RSS-walk / too-short / no-samples)
# causes exit non-zero, so systemd / CI can gate on a 7-day run
# without the operator having to read the log.
#
# `make soak-7day`   runs the full 604800 s gate against the
#                    installed zclassic23 (MVP criterion #6).
# `make soak-smoke`  runs a 5-minute smoke test of the same
#                    binary so the runner itself doesn't rot
#                    between 7-day gates — safe to hook into
#                    CI on a machine that has the node up.
#
# Neither target is wired into the default `ci` pipeline: 7 days
# is obviously out of band, and the smoke target needs a live
# node on the same host, which most CI workers don't provide.
.PHONY: soak_runner
soak_runner: $(SOAK_RUNNER_BIN)
$(SOAK_RUNNER_BIN): tools/soak/main.c lib/test/src/soak_harness.c \
                        lib/platform/src/clock.c \
                        lib/test/include/test/soak_harness.h
	@mkdir -p $(dir $@)
	$(CC) -std=c23 -O2 -Wall -Wextra -Werror -pedantic \
	    -D_POSIX_C_SOURCE=200809L \
	    -Ilib/test/include -Ilib/platform/include -Ilib/util/include -o $@ \
	    tools/soak/main.c lib/test/src/soak_harness.c lib/platform/src/clock.c

soak-7day: soak_runner zcl-rpc
	$(SOAK_RUNNER_BIN) \
	    --duration-sec=604800 \
	    --interval-sec=60 \
	    --service=zclassic23 \
	    --rpc=$(ZCL_RPC_BIN)

soak-smoke: soak_runner zcl-rpc
	$(SOAK_RUNNER_BIN) \
	    --duration-sec=300 \
	    --interval-sec=30 \
	    --service=zclassic23 \
	    --rpc=$(ZCL_RPC_BIN) \
	    --stall-sec=600 \
	    --warmup-sec=60

.PHONY: bench-sync
bench-sync: zclassic23 bench_fresh_sync
	$(BIN_DIR)/bench_fresh_sync

.PHONY: bench_fresh_sync
bench_fresh_sync: $(BIN_DIR)/bench_fresh_sync
$(BIN_DIR)/bench_fresh_sync: tools/bench_fresh_sync.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -o $@ $<

bench: zclassic23
	@$(ZCLASSIC23_BIN) -bench

bench-regress: zclassic23
	@$(ZCLASSIC23_BIN) -bench-regress

# CI guard: fresh datadir, must reach tip-10 in <600s against a local
# peer. Fails the build if sync regresses to the 9-hour stall the
# baked checkpoints + watchdog thread + peer-floor invariant are
# meant to prevent. Skipped automatically if no local peer is up.
# CI guard: fresh datadir + downloaded consensus_snapshot.db only,
# must reach tip > 1M with utxos > 1M in <90s. Asserts Wave 11A
# snapshot-first boot ordering didn't regress — without that fix the
# import path is silently dead. Skipped if no source snapshot is
# available locally (~/.zclassic-c23{,-test}/consensus_snapshot.db).
.PHONY: ci-coldstart
ci-coldstart: zclassic23
	@bash tools/scripts/cold_start_test.sh

.PHONY: ci-sync-smoke
ci-sync-smoke: zclassic23
	@if ! ss -tln 2>/dev/null | grep -q ':8033 '; then \
	    echo "[ci-sync-smoke] no local peer on :8033 — skipping"; \
	    exit 0; \
	fi
	@echo "[ci-sync-smoke] recording C benchmark placeholders..."
	@$(ZCLASSIC23_BIN) -bench-coldstart
	@$(ZCLASSIC23_BIN) -bench-mtbf
	@echo "[ci-sync-smoke] OK"

$(OBJ_DIR)/%.o: %.c $(TMPL_GEN)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# The one TU that bakes in ZCL_BUILD_COMMIT — see the stamp comment up top.
$(OBJ_DIR)/lib/util/src/clientversion.o: $(BUILD_COMMIT_STAMP)

# Deploy: lint → WAL checkpoint → install service → restart → RPC verify.
#
# `make deploy` used to print "Deployed." whenever systemd held the unit
# active for >2s — false-positive friendly. The new target fails loudly on
# three distinct paths:
#   1. `lint` — untouched, but now actually FAILs on raw sqlite3_step.
#   2. `wal_checkpoint` — truncate WAL before SIGTERM so SQLite doesn't
#      recover a half-checkpointed journal on boot.
#   3. `tools/deploy_verify.sh` — poll `zclassic-cli getblockcount` until the
#      node answers and diagnostics are ready, with a startup-sized deadline.
#
# The wal_checkpoint step calls the in-tree tools/wal_checkpoint binary
# (P12.4 — was an inline `sqlite3(1)` CLI invocation before, which failed
# on stock Ubuntu/Debian hosts where the CLI isn't installed).  The tool
# issues `sqlite3_wal_checkpoint_v2(TRUNCATE)` via the library only — no
# DELETE, no unguarded statements, and safe to re-run.
# ── install (MVP criterion #1) ──────────────────────────────────────────────
# Literal install for a fresh operator: copy the two binaries onto PATH and
# install the systemd --user unit pointed at the installed binary, so a clean
# Ubuntu/Debian box can do `make install && systemctl --user start zclassic23`.
#   PREFIX   binary install prefix (default /usr/local; use ~/.local for rootless)
#   DESTDIR  staging root for packaging — when set, the live --user unit + daemon
#            reload are skipped (binaries are only staged under DESTDIR).
PREFIX  ?= /usr/local
DESTDIR ?=
.PHONY: install
install: zclassic23 zcl-rpc
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 755 $(ZCLASSIC23_BIN) "$(DESTDIR)$(PREFIX)/bin/zclassic23"
	install -m 755 $(ZCL_RPC_BIN)    "$(DESTDIR)$(PREFIX)/bin/zcl-rpc"
	@if [ -z "$(DESTDIR)" ]; then \
	    install -d "$(HOME)/.config/systemd/user"; \
	    sed 's|%h/zclassic23/build/bin/zclassic23|$(PREFIX)/bin/zclassic23|' \
	        deploy/zclassic23.service \
	        > "$(HOME)/.config/systemd/user/zclassic23.service"; \
	    (systemctl --user daemon-reload 2>/dev/null || true); \
	    echo "installed systemd --user unit; start: systemctl --user start zclassic23"; \
	fi
	@echo "make install: zclassic23 + zcl-rpc -> $(DESTDIR)$(PREFIX)/bin"

deploy: lint zclassic23 zclassic-cli tools/wal_checkpoint
	@if [ -f $(HOME)/.zclassic-c23/node.db ]; then \
	    $(WAL_CHECKPOINT_BIN) $(HOME)/.zclassic-c23/node.db \
	        || { echo "WAL checkpoint failed"; exit 1; }; \
	fi
	@install -m 644 deploy/zclassic23.service $(HOME)/.config/systemd/user/zclassic23.service
	@systemctl --user daemon-reload
	systemctl --user restart zclassic23
	@./tools/deploy_verify.sh

release:
	@./tools/release.sh

clean:
	rm -rf $(BUILD_DIR)
	rm -f test_zcl test_parallel zclassic23 zclassic-cli zcl-rpc zcl-nodectl \
	    zclassic23-chaos p2_invariant_check crash_recovery_test rebuild_recent \
	    shadow_replay_proof wallet_check spec_zcl session bot wallet_dump \
	    wallet_sim wallet-wireframes mock_rpc export_snapshot bench_fresh_sync \
	    fuzz_block fuzz_script fuzz_p2p test_zcl_cov
	rm -f tools/gen_templates tools/inspect_html tools/wal_checkpoint \
	    tools/check_observability_pairing tools/gen_sha3_windows \
	    tools/soak/soak_runner

# ── Coverage (wave 5 #8) ──────────────────────────────────────
#
# Establishes a measurement path.  This is NOT targeted at a specific
# percentage yet — it exists so future commits can track whether they
# move the needle up or down.
#
# Builds a separate `test_zcl_cov` binary with gcov instrumentation
# instead of clobbering the main `test_zcl` (which uses -flto and -O3,
# both of which fight with coverage instrumentation).  Running the
# coverage binary emits .gcda files next to each translation unit;
# we then render them with either lcov+genhtml or gcovr — whichever
# is installed — and leave the tooling path permissive so a developer
# without coverage utilities still gets a useful message.
#
# Normal `make` / `make test` paths are untouched.
#
# NB: `-Werror` gets stripped alongside `-flto -O3` because -O0/-O1 +
# gcov produces a different set of lints (unused-static,
# format-truncation at different inlining thresholds) that fire
# cleanly in the main build but trip -Werror here.  Coverage is an
# observability tool, not a production build — warnings still print,
# they just don't block the binary.
#
# Two things have to be right before the numbers are meaningful:
#
# 1. Optimisation level.  -O0 + gcov drives one of the recursive
#    JSON tests into stack overflow in ~11 minutes wall-clock; -O1
#    keeps the instrumentation accurate (lcov/gcov handle it fine)
#    while cutting runtime roughly in half and eliminating the
#    regression.
#
# 2. Object-file layout.  The main `test_zcl` target compiles all
#    sources in ONE `cc` invocation — that's fast for LTO but ruinous
#    for gcov, because files like `lib/net/src/protocol.c` and
#    `lib/rpc/src/protocol.c` share a basename and collide at .gcda
#    write time ("overwriting an existing profile data with a
#    different checksum").  For the coverage build we therefore
#    compile each source into its own `build/cov/<same/path>/file.o`
#    FIRST, then link — this way each .gcda lives next to its .o and
#    the directory structure guarantees uniqueness.  Slower than the
#    single-command build, but sound.
COV_BUILD_DIR = $(BUILD_DIR)/cov
COV_CFLAGS = $(filter-out -flto -O3 -march=native -Werror,$(CFLAGS)) \
             --coverage -O1 -g -DCOVERAGE_BUILD -DZCL_TESTING
COV_LDFLAGS = $(filter-out -flto,$(LDFLAGS)) --coverage
COV_TEST_BIN = $(BIN_DIR)/test_zcl_cov
COV_INFO = $(BUILD_DIR)/coverage.info
COV_HTML = $(BUILD_DIR)/coverage_html

COV_TEST_SRCS := $(filter-out lib/test/src/test_parallel.c, $(TEST_SRCS))
COV_OBJS := $(patsubst %.c,$(COV_BUILD_DIR)/%.o,$(COV_TEST_SRCS) $(SPEC_SRCS) $(ALL_SRCS))

$(COV_BUILD_DIR)/%.o: %.c $(TMPL_GEN)
	@mkdir -p $(dir $@)
	$(CC) $(COV_CFLAGS) -Wno-deprecated-declarations -c -o $@ $<

.PHONY: test_zcl_cov
test_zcl_cov: $(COV_TEST_BIN)
$(COV_TEST_BIN): $(COV_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(COV_CFLAGS) $(COV_LDFLAGS) -o $@ $(COV_OBJS) $(TOR_LIBS) $(LIBS) $(GTK_LIBS) $(WEBKIT_LIBS)

coverage: coverage-clean test_zcl_cov
	@echo "== Resetting gcov counters =="
	@find $(COV_BUILD_DIR) -name '*.gcda' -delete 2>/dev/null || true
	@echo "== Running test_zcl_cov =="
	@# Match the `test` target — some json/recursion tests need
	@# unlimited stack.  If the binary still crashes we continue
	@# anyway so the partial coverage data is still flushed for any
	@# test that ran to completion before the crash (cov_flush.c
	@# installs a SIGSEGV handler that calls __gcov_dump).
	@ulimit -s unlimited && $(COV_TEST_BIN) || true
	@if command -v lcov >/dev/null 2>&1; then \
		echo "== Rendering coverage via lcov =="; \
		lcov --capture --directory $(COV_BUILD_DIR) --output-file $(COV_INFO) \
		     --rc geninfo_unexecuted_blocks=1 --quiet || true; \
		lcov --remove $(COV_INFO) \
		     '*/lib/test/*' '*/vendor/*' '*/tools/fuzz/*' '/usr/*' \
		     --output-file $(COV_INFO) --quiet || true; \
		lcov --summary $(COV_INFO); \
		if command -v genhtml >/dev/null 2>&1; then \
			genhtml --quiet $(COV_INFO) --output-directory $(COV_HTML); \
			echo "== HTML report: $(COV_HTML)/index.html =="; \
		else \
			echo "(genhtml not installed — summary only)"; \
		fi; \
	elif command -v gcovr >/dev/null 2>&1; then \
		echo "== Rendering coverage via gcovr =="; \
		gcovr --root . --filter 'app/' --filter 'lib/' --filter 'tools/' \
		      --exclude 'lib/test/.*' --exclude 'vendor/.*' \
		      --exclude 'tools/fuzz/.*' --print-summary; \
	elif command -v gcov >/dev/null 2>&1; then \
		echo "== Rendering coverage via plain gcov (install lcov or gcovr for full report) =="; \
		gcov_sum=$$(mktemp); \
		find $(COV_BUILD_DIR) -name '*.gcda' \
		    -not -path '*/lib/test/*' -not -path '*/tools/fuzz/*' \
		    -print0 2>/dev/null \
		    | xargs -0 -r gcov -n 2>/dev/null \
		    > $$gcov_sum || true; \
		awk ' \
		    /^File / { cur=$$2; gsub(/'\''/, "", cur); \
		               sysheader = (index(cur, "/usr/") == 1 || index(cur, "vendor/") > 0 || index(cur, "lib/test/") > 0); \
		               next } \
		    /^Lines executed:/ { \
		        if (sysheader) next; \
		        split($$2, p, ":"); pct = p[2]; gsub(/%.*$$/, "", pct); \
		        total = $$4 + 0; \
		        exec = total * (pct+0) / 100.0; \
		        sum_exec += exec; sum_total += total; n++ \
		    } \
		    END { \
		        if (n > 0 && sum_total > 0) { \
		            printf "coverage: %d translation units, %d / %d lines executed (%.1f%%)\n", \
		                n, sum_exec, sum_total, 100.0 * sum_exec / sum_total; \
		            printf "(install lcov or gcovr for per-file breakdown + HTML report)\n" \
		        } else { \
		            printf "coverage: no .gcda data — did the test binary fail to run?\n" \
		        } \
		    }' $$gcov_sum; \
		rm -f $$gcov_sum *.gcov 2>/dev/null || true; \
	else \
		echo "WARN: install lcov, gcovr, or gcc's gcov to render the report."; \
		echo "Raw .gcda files are left in place for manual inspection."; \
	fi

coverage-clean:
	@rm -rf $(COV_BUILD_DIR) $(COV_INFO) $(COV_HTML) $(COV_TEST_BIN)
	@find . \( -name '*.gcda' -o -name '*.gcno' \) -delete 2>/dev/null || true
	@echo "Coverage artifacts removed."

# ── docs-mcp ───────────────────────────────────────────────────
# Regenerate MCP_REFERENCE.md by running the MCP server in stdio
# mode and piping the tools/list JSON response through a small
# Python formatter.  The formatter uses stdlib only — no pip install
# required.  Use `make docs-mcp-check` in CI to fail when the
# checked-in reference drifts from what the live router emits.
docs-mcp: zclassic23
	@echo "== Generating MCP_REFERENCE.md =="
	@echo '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' \
		| $(ZCLASSIC23_BIN) -mcp 2>/dev/null \
		| python3 tools/gen_mcp_reference.py > MCP_REFERENCE.md
	@wc -l MCP_REFERENCE.md

docs-mcp-check: zclassic23
	@echo "== Verifying MCP_REFERENCE.md is up to date =="
	@tmp=$$(mktemp); \
	 echo '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' \
		| $(ZCLASSIC23_BIN) -mcp 2>/dev/null \
		| python3 tools/gen_mcp_reference.py > "$$tmp"; \
	 if ! diff -q MCP_REFERENCE.md "$$tmp" >/dev/null; then \
		echo "MCP_REFERENCE.md is STALE. Run: make docs-mcp"; \
		diff -u MCP_REFERENCE.md "$$tmp" | head -40; \
		rm -f "$$tmp"; \
		exit 1; \
	 fi; \
	 rm -f "$$tmp"; \
	 echo "MCP_REFERENCE.md is up to date."

# ── ci ─────────────────────────────────────────────────────────
# Single command for full verification: build, test, fuzz (short),
# and coverage.  Fail-fast — stops at the first broken stage so
# you don't waste minutes on coverage when tests don't pass.
#
# Usage:
#   make ci                 # full pipeline
#   make ci SKIP_FUZZ=1     # skip the fuzz stage (faster)
#   make ci SKIP_COV=1      # skip coverage (faster)
check-malloc:
	@echo "══ LINT: bare malloc/calloc/realloc in app/tools code ══"
	@HITS=$$(grep -rn '[^_]malloc\s*(' app/ tools/ --include='*.c' --include='*.h' \
	    | grep -v 'zcl_malloc\|zcl_calloc\|zcl_realloc\|raw-alloc-ok\|safe_alloc\|".*malloc\|LOG_\|fprintf'); \
	if [ -n "$$HITS" ]; then \
	    echo "$$HITS"; \
	    echo "FAIL: bare malloc in app/tools code (use zcl_malloc or mark // raw-alloc-ok)"; \
	    exit 1; \
	fi
	@HITS=$$(grep -rn '[^_]calloc\s*(' app/ tools/ --include='*.c' --include='*.h' \
	    | grep -v 'zcl_calloc\|raw-alloc-ok\|safe_alloc\|".*calloc\|LOG_\|fprintf'); \
	if [ -n "$$HITS" ]; then \
	    echo "$$HITS"; \
	    echo "FAIL: bare calloc in app/tools code (use zcl_calloc or mark // raw-alloc-ok)"; \
	    exit 1; \
	fi
	@HITS=$$(grep -rn '[^_]realloc\s*(' app/ tools/ --include='*.c' --include='*.h' \
	    | grep -v 'zcl_realloc\|raw-alloc-ok\|safe_alloc\|".*realloc\|LOG_\|fprintf'); \
	if [ -n "$$HITS" ]; then \
	    echo "$$HITS"; \
	    echo "FAIL: bare realloc in app/tools code (use zcl_realloc or mark // raw-alloc-ok)"; \
	    exit 1; \
	fi
	@echo "  OK: no raw allocations"

check-silent-errors:
	@echo "══ LINT: bare return -1 in MCP handlers ══"
	@HITS=$$(grep -rn 'return -1;' tools/mcp/controllers/ --include='*.c' \
	    | grep -v 'LOG_ERR\|log_json\|fprintf\|// silent-ok' \
	    | grep -vE '// raw-return-ok:[A-Za-z][A-Za-z0-9_-]+'); \
	if [ -n "$$HITS" ]; then \
	    echo "$$HITS"; \
	    echo "FAIL: bare return -1 in MCP handlers (use LOG_ERR or mark // raw-return-ok:<tag>)"; \
	    exit 1; \
	fi
	@echo "  OK: all MCP error returns logged"

check-raw-sqlite:
	@echo "══ LINT: raw sqlite3_step in app code ══"
	@tools/scripts/check_raw_sqlite.sh

check-raw-malloc:
	@echo "══ LINT: raw malloc/calloc/realloc in production code ══"
	@tools/scripts/check_raw_malloc.sh

check-coins-lookup-nullcheck:
	@echo "══ LINT: guarded controller coin lookups ══"
	@tools/scripts/check_coins_lookup_nullcheck.sh

.PHONY: tools/check_observability_pairing
tools/check_observability_pairing: $(BIN_DIR)/check_observability_pairing
$(BIN_DIR)/check_observability_pairing: tools/check_observability_pairing.c
	@mkdir -p $(dir $@)
	$(CC) -std=c23 -O2 -Wall -Wextra -Werror -o $@ $<

check-observability-pairing: tools/check_observability_pairing
	@echo "══ LINT: observable stderr diagnostics ══"
	@$(BIN_DIR)/check_observability_pairing

check-silent-errors-services:
	@echo "══ LINT: silent error returns in services ══"
	@HITS=$$(grep -rn -B1 'return -1;' app/services/src/ --include='*.c' \
	    | grep 'return -1;' \
	    | grep -v 'LOG_ERR\|LOG_FAIL\|LOG_RETURN\|log_json' \
	    | grep -vE '(//|/\*) raw-return-ok:[A-Za-z][A-Za-z0-9_-]+' \
	    | while read -r line; do \
	        file=$$(echo "$$line" | cut -d: -f1); \
	        lnum=$$(echo "$$line" | cut -d: -f2); \
	        prev=$$((lnum - 1)); \
	        prev_line=$$(sed -n "$${prev}p" "$$file"); \
	        echo "$$prev_line" | grep -qE 'LOG_ERR|LOG_FAIL|LOG_RETURN|log_json.*error' || echo "$$line"; \
	    done); \
	if [ -n "$$HITS" ]; then \
	    echo "$$HITS"; \
	    echo "FAIL: silent error returns found in services (use LOG_ERR/LOG_FAIL/LOG_RETURN, prev-line error log, or mark // raw-return-ok:<reason>)"; \
	    exit 1; \
	fi
	@echo "  OK: all service error returns logged"

check-silent-errors-controllers:
	@echo "══ LINT: silent error returns in controllers ══"
	@HITS=$$(grep -rn -B1 'return -1;' app/controllers/src/ --include='*.c' \
	    | grep 'return -1;' \
	    | grep -v 'LOG_ERR\|LOG_FAIL\|LOG_RETURN\|log_json' \
	    | grep -vE '(//|/\*) raw-return-ok:[A-Za-z][A-Za-z0-9_-]+' \
	    | while read -r line; do \
	        file=$$(echo "$$line" | cut -d: -f1); \
	        lnum=$$(echo "$$line" | cut -d: -f2); \
	        prev=$$((lnum - 1)); \
	        prev_line=$$(sed -n "$${prev}p" "$$file"); \
	        echo "$$prev_line" | grep -qE 'LOG_ERR|LOG_FAIL|LOG_RETURN|log_json.*error' || echo "$$line"; \
	    done); \
	if [ -n "$$HITS" ]; then \
	    echo "$$HITS"; \
	    echo "FAIL: silent error returns found in controllers (use LOG_ERR/LOG_RETURN, prev-line fprintf, or mark // raw-return-ok:<reason>)"; \
	    exit 1; \
	fi
	@echo "  OK: all controller error returns logged"

check-silent-errors-jobs:
	@echo "══ LINT: silent error returns in jobs ══"
	@HITS=$$(grep -rn -B1 'return -1;' app/jobs/src/ --include='*.c' \
	    | grep 'return -1;' \
	    | grep -v 'LOG_ERR\|LOG_FAIL\|LOG_RETURN\|log_json' \
	    | grep -vE '(//|/\*) raw-return-ok:[A-Za-z][A-Za-z0-9_-]+' \
	    | while read -r line; do \
	        file=$$(echo "$$line" | cut -d: -f1); \
	        lnum=$$(echo "$$line" | cut -d: -f2); \
	        prev=$$((lnum - 1)); \
	        prev_line=$$(sed -n "$${prev}p" "$$file"); \
	        echo "$$prev_line" | grep -qE 'LOG_ERR|LOG_FAIL|LOG_RETURN|log_json.*error' || echo "$$line"; \
	    done); \
	if [ -n "$$HITS" ]; then \
	    echo "$$HITS"; \
	    echo "FAIL: silent error returns found in jobs (use LOG_ERR/LOG_FAIL/LOG_RETURN, prev-line error log, or mark // raw-return-ok:<reason>)"; \
	    exit 1; \
	fi
	@echo "  OK: all job error returns logged"

check-silent-errors-conditions:
	@echo "══ LINT: silent error returns in conditions ══"
	@HITS=$$(grep -rn -B1 'return -1;' app/conditions/src/ --include='*.c' \
	    | grep 'return -1;' \
	    | grep -v 'LOG_ERR\|LOG_FAIL\|LOG_RETURN\|log_json' \
	    | grep -vE '(//|/\*) raw-return-ok:[A-Za-z][A-Za-z0-9_-]+' \
	    | while read -r line; do \
	        file=$$(echo "$$line" | cut -d: -f1); \
	        lnum=$$(echo "$$line" | cut -d: -f2); \
	        prev=$$((lnum - 1)); \
	        prev_line=$$(sed -n "$${prev}p" "$$file"); \
	        echo "$$prev_line" | grep -qE 'LOG_ERR|LOG_FAIL|LOG_RETURN|log_json.*error' || echo "$$line"; \
	    done); \
	if [ -n "$$HITS" ]; then \
	    echo "$$HITS"; \
	    echo "FAIL: silent error returns found in conditions (use LOG_ERR/LOG_FAIL/LOG_RETURN, prev-line error log, or mark // raw-return-ok:<reason>)"; \
	    exit 1; \
	fi
	@echo "  OK: all condition error returns logged"

check-before-save-hooks:
	@echo "══ LINT: critical models wire before_save hooks ══"
	@for model in utxo block wallet_key wallet_tx; do \
	    grep -q 'before_save' app/models/src/$$model.c \
	    || (echo "FAIL: app/models/src/$$model.c missing before_save hook" && exit 1); \
	done
	@echo "  OK: critical models have before_save hooks"

# Move 4: every long-running thread goes through thread_registry_spawn{,_ex}.
# Short-burst workers joined within the same function, and pthread_attr-using
# detached-helper wrappers, are explicitly opted out with a `raw-pthread-ok`
# marker on the call line or the line immediately above. The registry's own
# implementation in lib/util/src/thread_registry.c is implicitly skipped.
check-pthread-create:
	@echo "══ LINT: raw pthread_create outside thread_registry ══"
	@HITS=$$(grep -rn 'pthread_create\s*(' lib/ app/ tools/ config/ --include='*.c' \
	    | grep -v 'lib/test/' \
	    | grep -v 'lib/util/src/thread_registry.c' \
	    | grep -v 'thread_registry_spawn\|thread_registry_trampoline' \
	    | grep -v 'raw-pthread-ok' \
	    | while read -r line; do \
	        f=$$(echo "$$line" | cut -d: -f1); \
	        n=$$(echo "$$line" | cut -d: -f2); \
	        prev=$$((n - 1)); \
	        if [ "$$prev" -gt 0 ] && \
	           sed -n "$${prev}p" "$$f" | grep -q 'raw-pthread-ok'; then \
	            continue; \
	        fi; \
	        echo "$$line"; \
	    done); \
	if [ -n "$$HITS" ]; then \
	    echo "$$HITS"; \
	    echo "FAIL: raw pthread_create in production code (use thread_registry_spawn{,_ex} or mark // raw-pthread-ok: <reason>)"; \
	    exit 1; \
	fi
	@echo "  OK: all pthread_create call sites accounted for"

# Move 11: every app/models/src/*.c either invokes validates_* macros
# from app/models/include/models/activerecord.h, or carries an
# ar-validate-skip:<tag> marker explaining why the AR validation
# lifecycle does not apply (infrastructure wrapper, registry, etc.).
check-model-validation:
	@echo "══ LINT: model validation coverage ══"
	@./tools/scripts/check_model_validation.sh

# Keep top-level functions in app/controllers + app/services under 500
# lines. Single state-machines that truly belong as one function can carry
# a `// long-function-ok:<tag>` override marker explaining WHY.
check-long-functions:
	@echo "══ LINT: long function cap (500 lines) ══"
	@./tools/scripts/check_long_functions.sh

# Wave 9a: every register_*_rpc_commands callsite uses rpc_table_must_append.
# rpc_table_append returns false silently on registration failure (duplicate
# name / MAX_RPC_COMMANDS cap / table running) — that silent failure mode
# left the control-group RPCs unreachable for a release cycle. The
# must_append variant aborts at boot with a precise reason.
check-rpc-registrar:
	@echo "══ LINT: rpc_table_must_append in registrars ══"
	@./tools/scripts/check_rpc_registrar.sh

# Lag-SLO observability: the legacy_mirror_sync_service must emit
# EV_LAG_SLO_BREACH and EV_MIRROR_CONCURRENT_CATCHUP, and the
# chain_advance_coordinator must honor mirror_lag_sla_breach_blocks.
# Prevents the "silent lag" regression we shipped this gate to lock down.
check-lag-slo-observable:
	@echo "══ LINT: lag SLO observability ══"
	@./tools/scripts/check_lag_slo_observable.sh

# lib/ layer purity: no lib/ file should #include from app/ unless the
# include is in the grandfathered baseline or has a documented per-line
# override marker. Catches regressions; lets us pay down the existing
# debt incrementally.
check-lib-layering:
	@echo "══ LINT: lib/ layer purity ══"
	@./tools/scripts/check_lib_layering.sh

# Supervisor registration: every long-running service in
# app/services/src/*_service.c must register a liveness contract with
# the supervisor (Round 5) OR appear in supervisor_baseline.txt OR
# carry a per-file `// supervisor-ok:<tag>` override marker. Drives
# opt-in adoption of the supervisor primitive over Rounds 6-8.
check-supervisor-registration:
	@echo "══ LINT: supervisor registration ══"
	@./tools/scripts/check_supervisor_registration.sh

# Lint gate #16 — typed blocker primitive adoption (Round 6 C6).
# Ratchets raw `char *_blocker[]` string fields / `lms_set_blocker(`
# legacy setters / `last_blocker_code` mutations to the typed
# `blocker_set()` primitive (lib/util/blocker.h). Baseline file
# enumerates the grandfathered sites; must shrink over Rounds 7-9.
check-typed-blocker:
	@echo "══ LINT: typed blocker adoption ══"
	@./tools/scripts/check_typed_blocker.sh

# Gate #18 graduated WARN → RATCHET (E10): fails on any new off-shape
# app/ .c file (the allowlist is the baseline and is currently empty).
check-framework-shape:
	@echo "→ Gate #18: framework_shape_check"
	@ZCL_LINT_MODE=RATCHET ./tools/lint/framework_shape_check.sh

# Gate #22 — framework filename suffix (HARD). The recurrence guard for the
# S1 service renames: no app/ file may carry a foreign shape's name suffix
# (e.g. *_controller.c in services/). Override: // suffix-ok:<tag>.
check-framework-filename-suffix:
	@echo "→ Gate #22: framework_filename_suffix"
	@./tools/lint/check_framework_filename_suffix.sh

check-no-raw-clock-outside-platform:
	@echo "→ Gate #19: no_raw_clock_outside_platform"
	@./tools/lint/check_no_raw_clock_outside_platform.sh

# Gate #20 graduated WARN → RATCHET (E10): fails on any new controller
# file that uses raw sqlite. Baseline of grandfathered files lives in
# tools/lint/no_raw_sqlite_in_controllers_baseline.txt (may only shrink).
check-no-raw-sqlite-in-controllers:
	@echo "→ Gate #20: no_raw_sqlite_in_controllers"
	@ZCL_LINT_MODE=RATCHET ./tools/lint/check_no_raw_sqlite_in_controllers.sh

check-supervisor-domain:
	@echo "→ Gate #21: supervisor_domain"
	@./tools/lint/check_supervisor_domain.sh

# Gate E13 — consensus-parity guard (HARD). Bans any non-zclassicd consensus
# mechanism (miner-signaled versionbits / dynamic Equihash override — the
# PR #6 "sidegrade" class) from the consensus path, so zclassic23 stays
# bit-for-bit consensus-compatible with zclassicd. Doctrine:
# docs/CONSENSUS_PARITY_DOCTRINE.md; the golden VALUES are pinned by the
# test_consensus_parity test group.
check-consensus-parity:
	@echo "══ LINT: consensus parity with zclassicd (E13) ══"
	@./tools/scripts/check_consensus_parity.sh

# Gate E1 — file-size ceiling for app/ .c files (RATCHET). Mega-modules
# cannot hide behind <500-LOC functions; baseline at
# tools/scripts/file_size_ceiling_baseline.txt may only shrink.
check-file-size-ceiling:
	@echo "══ LINT: app/ file-size ceiling (E1) ══"
	@./tools/scripts/check_file_size_ceiling.sh

# Gate E9 — EV_OPERATOR_NEEDED emit must reach a registered sink (HARD).
# The silent-halt fix: the loud "human needed" signal can never be emitted
# without a subscriber in lib/util/src/alerts.c.
check-operator-needed-sink:
	@echo "══ LINT: operator-needed sink (E9) ══"
	@./tools/scripts/check_operator_needed_sink.sh

# Gate E11 — doc accuracy: the gate list in DEFENSIVE_CODING.md must match
# the actual check-* dependencies of the lint: target (count + names).
check-doc-accuracy:
	@echo "══ LINT: doc accuracy (E11) ══"
	@./tools/scripts/check_doc_accuracy.sh

# Gate E2 — new service functions return struct zcl_result, not bare
# bool/int (RATCHET at file granularity; baseline at
# tools/scripts/one_result_type_baseline.txt may only shrink).
check-one-result-type:
	@echo "══ LINT: one result type (E2) ══"
	@./tools/scripts/check_one_result_type.sh

# ── Shape-skeleton generator (Workstream A5, FRAMEWORK.md Law 3) ──────
# Emit a correct, compiling, readable skeleton for one of the four shapes
# into the right shape folder. Plain committed source (no metaprogramming),
# matching the exemplars so it passes the framework lint gates the day it
# lands. The generator never edits a registry — it prints the wiring step.
#
#   make new-condition  NAME=foo_bar   -> app/conditions/src/foo_bar.c
#   make new-model      NAME=foo        -> app/models/src/foo.c
#   make new-job        NAME=foo_stage  -> app/jobs/src/foo_stage.c
#   make new-controller NAME=foo        -> app/controllers/src/foo_controller.c
.PHONY: new-condition new-model new-job new-controller
new-condition:
	@test -n "$(NAME)" || { echo "usage: make new-condition NAME=foo_bar"; exit 1; }
	@./tools/new_shape.sh condition "$(NAME)"
new-model:
	@test -n "$(NAME)" || { echo "usage: make new-model NAME=foo"; exit 1; }
	@./tools/new_shape.sh model "$(NAME)"
new-job:
	@test -n "$(NAME)" || { echo "usage: make new-job NAME=foo_stage"; exit 1; }
	@./tools/new_shape.sh job "$(NAME)"
new-controller:
	@test -n "$(NAME)" || { echo "usage: make new-controller NAME=foo"; exit 1; }
	@./tools/new_shape.sh controller "$(NAME)"

# Gate E3 — shape source files include their shape contract header
# (conditions -> framework/condition.h, models -> models/ header,
# supervisors -> supervisor header). HARD: the tree already complies.
check-shape-includes-header:
	@echo "══ LINT: shape includes header (E3) ══"
	@./tools/scripts/check_shape_includes_header.sh

# Gate E4 — projections are pure folds: no app-layer (services/controllers)
# includes and no AR model saves. HARD: the projection set already complies.
check-projections-pure:
	@echo "══ LINT: projections pure (E4) ══"
	@./tools/scripts/check_projections_pure.sh

# Gate E6 — one chain-state write path (RATCHET). Legacy writer surfaces
# are grandfathered in tools/scripts/one_write_path_baseline.txt and shrink
# as B8 deletes them; new write surfaces fail.
check-one-write-path:
	@echo "══ LINT: one write path (E6) ══"
	@./tools/scripts/check_one_write_path.sh

# Gate E7 — no authoritative RAM state (RATCHET). Direct active_chain
# internals/global active_chain state are forbidden outside the baseline.
check-no-authoritative-ram-state:
	@echo "══ LINT: no authoritative RAM state (E7) ══"
	@./tools/scripts/check_no_authoritative_ram_state.sh

# Gate E5 — Job stages advance OR block (HARD). Every app/jobs/src/*_stage.c
# step must surface JOB_BLOCKED/JOB_IDLE on non-progress AND reference a cursor
# (cursor_out / c->cursor_in / stage_cursor) — no silent forward spin. The 8
# stages already comply, so the gate runs HARD.
check-stage-advances-or-blocks:
	@echo "══ LINT: stage advances-or-blocks (E5) ══"
	@./tools/scripts/check_stage_advances_or_blocks.sh

check-no-silent-ready:
	@echo "══ LINT: no-silent-ready (E8) ══"
	@./tools/scripts/check_no_silent_ready.sh

# Gate E12 — honest witness (Law 7). A Condition's witness must observe the
# symptom MOVE (tip/cursor/block_map/SELECT/progress counter), never just a
# constant, the pure inverse of detect, or an FSM/poison-flag the remedy
# itself set (which lets a no-op remedy self-certify "cleared"). FAIL mode:
# the tree is clean (every witness reads real progress or carries a reviewed
# // honest-witness-ok:<reason> hatch); the baseline at
# tools/lint/honest_witness_baseline.txt is empty and may only shrink.
check-honest-witness:
	@echo "══ LINT: honest witness (E12) ══"
	@ZCL_LINT_MODE=FAIL ./tools/lint/check_honest_witness.sh

lint: check-malloc check-silent-errors check-raw-sqlite check-raw-malloc check-coins-lookup-nullcheck check-observability-pairing check-silent-errors-services check-silent-errors-controllers check-silent-errors-jobs check-silent-errors-conditions check-before-save-hooks check-pthread-create check-model-validation check-long-functions check-rpc-registrar check-lag-slo-observable check-lib-layering check-supervisor-registration check-typed-blocker check-framework-shape check-framework-filename-suffix check-no-raw-clock-outside-platform check-no-raw-sqlite-in-controllers check-supervisor-domain check-file-size-ceiling check-operator-needed-sink check-doc-accuracy check-one-result-type check-shape-includes-header check-projections-pure check-one-write-path check-no-authoritative-ram-state check-stage-advances-or-blocks check-no-silent-ready check-honest-witness check-consensus-parity
	@echo "══ LINT: all checks passed ══"

ci: lint bench-regress zclassic23 test_zcl
	@echo "══ CI: test ══"
	ulimit -s unlimited && $(TEST_ZCL_BIN)
	@echo ""
	@echo "══ CI: mvp-gates (hermetic MVP acceptance #3/#5/#7) ══"
	$(MAKE) ci-mvp-gates
	@echo ""
	@echo "══ CI: test-crash ══"
	$(MAKE) test-crash
	@echo ""
	@if [ "$(SKIP_FUZZ)" != "1" ]; then \
		echo "══ CI: fuzz-ci ══"; \
		$(MAKE) fuzz-ci || exit 1; \
		echo ""; \
	else \
		echo "══ CI: fuzz-ci (SKIPPED — SKIP_FUZZ=1) ══"; \
	fi
	@if [ "$(SKIP_COV)" != "1" ]; then \
		echo "══ CI: coverage ══"; \
		$(MAKE) coverage || exit 1; \
	else \
		echo "══ CI: coverage (SKIPPED — SKIP_COV=1) ══"; \
	fi
	@echo ""
	@echo "══ CI: ALL STAGES PASSED ══"

audit:
	@tools/dep_audit.sh

check-restart-follow:
	$(ZCL_NODECTL_BIN) verify-follow --restart
