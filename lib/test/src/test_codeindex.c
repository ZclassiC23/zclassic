/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_codeindex — the lib/codeindex/ foundation gate.
 *
 * Coverage:
 *   1. build determinism           — same tree ⇒ byte-identical symbol dump.
 *   2. query correctness           — known sym / file / group / refs answers.
 *   3. rebuild-from-scratch identity — delete store, reopen, same dump.
 *   4. staleness ⇒ auto-rebuild    — edit a file, reopen, edit is reflected.
 *   5. verify-on-read              — a corrupted symbol row is rejected.
 *   6. group parity                — scanner module list == Makefile
 *                                    LIB_MODULES, and the eight app/ shapes.
 *   7. file counts + route parity  — recursive vs direct group file counts, and
 *                                    `code tests` route == `dev test plan`
 *                                    proof_group for the same single file.
 *   8. complete source roots       — src/, ports/, and lib/test/ are indexed;
 *                                    license boilerplate is not a purpose.
 *   9. publication safety          — content freshness, old-reader retention,
 *                                    crash boundaries, and 32-way cold open.
 *
 * All scratch work happens under ./test-tmp/ (project no-/tmp convention). */

#include "test/test_helpers.h"

#include "codeindex/codeindex.h"
#include "codeindex/codeindex_build.h"
#include "platform/time_compat.h"

/* For the routing-link parity invariant (case 7): `code tests <path>`'s route
 * (tools/command/native_code_command.c) must equal `dev test plan`'s
 * proof_group (tools/dev/devloop_plan.c) for the same single changed file. */
#include "command/native_command.h"
#include "devloop.h"

#include <sqlite3.h>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CI_CHECK(name, expr) do {                                     \
    if (expr) { printf("  codeindex: %s... OK\n", (name)); }          \
    else { printf("  codeindex: %s... FAIL\n", (name)); failures++; } \
} while (0)

#define FIX "test-tmp/ci_fix"

/* Write content to <dir>/<rel>, creating parent dirs. */
static bool mk_write(const char *dir, const char *rel, const char *content)
{
    char full[4096];
    snprintf(full, sizeof(full), "%s/%s", dir, rel);
    /* create every parent component, including the fixture root itself */
    for (char *p = full + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(full, 0755); *p = '/'; }
    }
    FILE *f = fopen(full, "wb");
    if (!f) return false;
    if (content && content[0]) fwrite(content, 1, strlen(content), f);
    fclose(f);
    return true;
}

static const char *FOO_C =
    "/* net/foo.c — fixture translation unit for the code index test. */\n"
    "#include \"net/foo.h\"\n"
    "\n"
    "#define FOO_MAX 128\n"
    "\n"
    "/* A private accumulator helper. */\n"
    "static int helper_add(int a, int b)\n"
    "{\n"
    "    return a + b;\n"
    "}\n"
    "\n"
    "struct foo_state { int count; };\n"
    "\n"
    "typedef struct { int a; int b; } foo_pair;\n"
    "\n"
    "enum foo_color { FOO_RED, FOO_GREEN };\n"
    "\n"
    "/* Entry point that does a little work. */\n"
    "int foo_main(int x)\n"
    "{\n"
    "    int y = helper_add(x, 1);\n"
    "    int c = foo_checksum(0, 0);\n"
    "    return c + y;\n"
    "}\n"
    "\n"
    "#ifdef FOO_DEBUG\n"
    "void foo_debug(void)\n"
    "{\n"
    "    helper_add(1, 2);\n"
    "}\n"
    "#endif\n";

static const char *FOO_H =
    "/* net/foo.h — fixture header. */\n"
    "#ifndef NET_FOO_H\n"
    "#define NET_FOO_H\n"
    "\n"
    "#include <stddef.h>\n"
    "\n"
    "struct foo_state;\n"
    "\n"
    "/* Checksum over a data frame. */\n"
    "int foo_checksum(const unsigned char *data, size_t len);\n"
    "\n"
    "#endif\n";

static const char *BAR_H =
    "/* net/bar.h — alternate depfile fixture header. */\n"
    "#ifndef NET_BAR_H\n"
    "#define NET_BAR_H\n"
    "int bar_fixture(void);\n"
    "#endif\n";

/* Purpose-derivation fixtures (§1.1): stem-dashed header, explicit override,
 * and a file whose only comment is interior (no file-level purpose). */
static const char *PURPOSE_STEM_C =
    "/* Copyright 2026 Rhett Creighton - Apache License 2.0\n"
    " *\n"
    " * purpose_stem \xe2\x80\x94 derives its purpose from the stem header.\n"
    " */\n"
    "int purpose_stem_fn(void) { return 0; }\n";

static const char *PURPOSE_OVERRIDE_C =
    "/* Copyright 2026 Rhett Creighton - Apache License 2.0\n"
    " *\n"
    " * purpose: explicit override wins.\n"
    " */\n"
    "int purpose_override_fn(void) { return 0; }\n";

static const char *PURPOSE_NONE_C =
    "#include <stddef.h>\n"
    "\n"
    "/* interior helper doc, not a file-level purpose */\n"
    "int purpose_none_fn(void) { return 1; }\n";

static const char *PURPOSE_AFTER_LICENSE_C =
    "/* Copyright 2026 Rhett Creighton - Apache License 2.0\n"
    " * Distributed under the MIT software license, see the accompanying\n"
    " * file COPYING or http://www.opensource.org/licenses/mit-license.php.\n"
    " *\n"
    " * purpose_after_license — describes behavior after the license.\n"
    " */\n"
    "int purpose_after_license_fn(void) { return 0; }\n";

static const char *PURPOSE_LICENSE_ONLY_C =
    "/* Copyright 2026 Rhett Creighton - Apache License 2.0\n"
    " * Distributed under the MIT software license, see the accompanying\n"
    " * file COPYING or http://www.opensource.org/licenses/mit-license.php.\n"
    " */\n"
    "int purpose_license_only_fn(void) { return 0; }\n";

static const char *ROOT_MAIN_C =
    "/* Copyright 2026 Rhett Creighton - Apache License 2.0\n"
    " *\n"
    " * main — fixture top-level node entry.\n"
    " */\n"
    "int fixture_root_main(void) { return 0; }\n";

static const char *PORT_H =
    "/* SPDX-License-Identifier: Apache-2.0\n"
    " * Copyright 2026 Rhett Creighton\n"
    " *\n"
    " * fixture_port — fixture hexagonal interface.\n"
    " */\n"
    "int fixture_port_probe(void);\n";

static const char *TEST_SOURCE_C =
    "/* Copyright 2026 Rhett Creighton - Apache License 2.0\n"
    " *\n"
    " * test_fixture_indexed — fixture test translation unit.\n"
    " */\n"
    "int test_fixture_indexed(void) { return 0; }\n";

static bool write_fixture(void)
{
    return mk_write(FIX, "lib/net/src/foo.c", FOO_C) &&
           mk_write(FIX, "lib/net/include/net/foo.h", FOO_H) &&
           mk_write(FIX, "lib/net/include/net/bar.h", BAR_H) &&
           mk_write(FIX, "build/obj/foo.d",
                    "build/obj/foo.o: lib/net/src/foo.c "
                    "lib/net/include/net/foo.h\n") &&
           mk_write(FIX, "build/test-obj/foo.d",
                    "build/test-obj/foo.o: lib/net/src/foo.c "
                    "lib/net/include/net/foo.h\n") &&
           mk_write(FIX, "build/obj/epochs/old/foo.d",
                    "build/obj/foo.o: lib/net/src/foo.c "
                    "lib/net/include/net/bar.h\n") &&
           mk_write(FIX, "lib/net/src/purpose_stem.c", PURPOSE_STEM_C) &&
           mk_write(FIX, "lib/net/src/purpose_override.c", PURPOSE_OVERRIDE_C) &&
           mk_write(FIX, "lib/net/src/purpose_none.c", PURPOSE_NONE_C) &&
           mk_write(FIX, "lib/net/src/purpose_after_license.c",
                    PURPOSE_AFTER_LICENSE_C) &&
           mk_write(FIX, "lib/net/src/purpose_license_only.c",
                    PURPOSE_LICENSE_ONLY_C) &&
           mk_write(FIX, "src/main.c", ROOT_MAIN_C) &&
           mk_write(FIX, "ports/include/ports/fixture_port.h", PORT_H) &&
           mk_write(FIX, "lib/test/src/test_fixture_indexed.c", TEST_SOURCE_C) &&
           mk_write(FIX, "lib/test/build/generated_should_not_index.c",
                    "int generated_should_not_index(void) { return 0; }\n");
}

/* Canonical ordered dump of every symbol as one string (for identity tests). */
static char *dump_symbols(struct codeindex *ci)
{
    struct ci_symbol syms[256];
    int n = codeindex_find(ci, "", syms, 256);
    if (n < 0) return NULL;
    size_t cap = 64 * 1024, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    buf[0] = '\0';
    for (int i = 0; i < n; i++) {
        int w = snprintf(buf + len, cap - len,
                         "%s|%c|%s:%d|%s:%d|%s|%s|%d\n",
                         syms[i].name, syms[i].kind,
                         syms[i].def_path, syms[i].def_line,
                         syms[i].decl_path, syms[i].decl_line,
                         syms[i].signature, syms[i].guard,
                         syms[i].partial ? 1 : 0);
        if (w < 0 || (size_t)w >= cap - len) break;
        len += (size_t)w;
    }
    return buf;
}

/* Corrupt one symbol row's signature WITHOUT updating row_sha3, to exercise
 * verify-on-read. Returns true on success. */
static bool corrupt_symbol(const char *name)
{
    sqlite3 *db = NULL;
    if (sqlite3_open(FIX "/.codeindex/index.kv", &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    char sql[512];
    snprintf(sql, sizeof(sql),
             "UPDATE symbols SET signature='CORRUPTED' WHERE name='%s'", name);
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_close(db);
    return rc == SQLITE_OK;
}

/* Inspect the published generation directly, without triggering a lazy
 * rebuild. Used to distinguish the old/new winner at crash boundaries. */
static bool published_index_has_symbol(const char *name)
{
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(FIX "/.codeindex/index.kv", &db,
                        SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    sqlite3_stmt *st = NULL;
    bool found = false;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM symbols WHERE name=? LIMIT 1", -1, &st, NULL) ==
        SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
        found = sqlite3_step(st) == SQLITE_ROW;
    }
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    return found;
}

static bool no_staging_files(void)
{
    DIR *d = opendir(FIX "/.codeindex");
    if (!d) return false;
    bool clean = true;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "index.kv.tmp.", 13) == 0) {
            clean = false;
            break;
        }
    }
    closedir(d);
    return clean;
}

static bool file_equals(const char *path, const char *expected)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char buf[128];
    size_t n = fread(buf, 1, sizeof(buf), f);
    bool eof = feof(f) != 0;
    fclose(f);
    size_t expected_len = strlen(expected);
    return eof && n == expected_len &&
           memcmp(buf, expected, expected_len) == 0;
}

static uint64_t monotonic_us(void)
{
    int64_t now = platform_time_monotonic_us();
    return now > 0 ? (uint64_t)now : 0;
}

static bool crash_rebuild_at(enum codeindex_test_crash_point point)
{
    codeindex_test_set_crash_point(point);
    pid_t pid = fork();
    if (pid == 0) {
        struct codeindex *child = codeindex_open(FIX);
        if (child) codeindex_close(child);
        _exit(3);  /* reaching here means the required boundary did not fire */
    }
    codeindex_test_set_crash_point(CODEINDEX_TEST_CRASH_NONE);
    if (pid < 0) return false;
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return false;
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
}

static bool concurrent_open_32(const char *required_symbol)
{
    enum { CHILDREN = 32 };
    int gate[2];
    if (pipe(gate) != 0) return false;
    pid_t pids[CHILDREN];
    int started = 0;
    for (int i = 0; i < CHILDREN; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            close(gate[1]);
            char token = 0;
            if (read(gate[0], &token, 1) != 1) _exit(10);
            close(gate[0]);
            struct codeindex *child = codeindex_open(FIX);
            struct ci_symbol sym;
            bool found = false;
            bool ok = child &&
                      codeindex_symbol(child, required_symbol, &sym, &found) &&
                      found;
            if (child) codeindex_close(child);
            _exit(ok ? 0 : 11);
        }
        if (pid < 0) break;
        pids[started++] = pid;
    }
    close(gate[0]);
    bool ok = started == CHILDREN;
    for (int i = 0; i < started; i++) {
        char token = 'x';
        if (write(gate[1], &token, 1) != 1) ok = false;
    }
    close(gate[1]);
    for (int i = 0; i < started; i++) {
        int status = 0;
        if (waitpid(pids[i], &status, 0) != pids[i] ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0)
            ok = false;
    }
    return ok;
}

/* Parse the Makefile's LIB_MODULES (possibly line-continued) into a set. */
static int makefile_lib_modules(char out[64][64])
{
    FILE *f = fopen("Makefile", "rb");
    if (!f) return -1;
    char line[4096];
    int count = 0;
    bool in_block = false;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!in_block) {
            if (strncmp(p, "LIB_MODULES", 11) != 0) continue;
            in_block = true;
            /* skip "LIB_MODULES" and "=" */
            p += 11;
            char *eq = strchr(p, '=');
            if (eq) p = eq + 1;
        }
        bool cont = strchr(p, '\\') != NULL;
        /* tokenize p on whitespace, dropping a trailing backslash token */
        char *tok = strtok(p, " \t\r\n");
        while (tok) {
            if (strcmp(tok, "\\") != 0 && count < 64) {
                snprintf(out[count], 64, "%s", tok);
                count++;
            }
            tok = strtok(NULL, " \t\r\n");
        }
        if (!cont) break;
    }
    fclose(f);
    return count;
}

static bool set_contains(char set[64][64], int n, const char *s)
{
    for (int i = 0; i < n; i++) if (strcmp(set[i], s) == 0) return true;
    return false;
}

int test_codeindex(void)
{
    int failures = 0;

    system("rm -rf " FIX);
    if (!write_fixture()) {
        printf("  codeindex: write_fixture... FAIL\n");
        return failures + 1;
    }

    /* ── open (triggers first build) ── */
    struct codeindex *ci = codeindex_open(FIX);
    CI_CHECK("open builds index", ci != NULL);
    if (!ci) return failures + 1;

    /* ── 2: query correctness ── */
    struct ci_symbol s;
    bool found = false;

    codeindex_symbol(ci, "helper_add", &s, &found);
    CI_CHECK("helper_add is a static func", found && s.kind == 't' &&
             strstr(s.def_path, "lib/net/src/foo.c") != NULL);

    codeindex_symbol(ci, "foo_main", &s, &found);
    CI_CHECK("foo_main is a func", found && s.kind == 'T');

    codeindex_symbol(ci, "foo_state", &s, &found);
    CI_CHECK("foo_state is a struct (def wins)", found && s.kind == 'S' &&
             strstr(s.def_path, "src/foo.c") != NULL);

    codeindex_symbol(ci, "foo_pair", &s, &found);
    CI_CHECK("foo_pair is a typedef", found && s.kind == 'Y');

    codeindex_symbol(ci, "foo_color", &s, &found);
    CI_CHECK("foo_color is an enum", found && s.kind == 'E');

    codeindex_symbol(ci, "FOO_MAX", &s, &found);
    CI_CHECK("FOO_MAX is a macro", found && s.kind == 'M');

    codeindex_symbol(ci, "foo_debug", &s, &found);
    CI_CHECK("foo_debug carries #ifdef guard", found &&
             strcmp(s.guard, "FOO_DEBUG") == 0);

    codeindex_symbol(ci, "foo_checksum", &s, &found);
    CI_CHECK("foo_checksum is a header declaration", found &&
             s.def_path[0] == '\0' &&
             strstr(s.decl_path, "include/net/foo.h") != NULL);

    /* refs */
    struct ci_ref refs[32];
    int nref = codeindex_refs(ci, "helper_add", refs, 32);
    CI_CHECK("helper_add has call-site refs", nref >= 1);
    nref = codeindex_refs(ci, "foo_checksum", refs, 32);
    CI_CHECK("foo_checksum has one call-site ref", nref == 1);

    char includes[8][256];
    int nincludes = codeindex_includes_of_file(
        ci, "lib/net/src/foo.c", includes, 8);
    CI_CHECK("compiler depfile include edge is indexed",
             nincludes == 1 &&
             strcmp(includes[0], "lib/net/include/net/foo.h") == 0);

    /* Warm opens validate metadata cache keys only. Exact source/dep bytes are
     * sealed in the generation and reread only when inode/size/mtime/ctime
     * changes. Historical compile epochs are outside the active dep graph. */
    codeindex_close(ci);
    ci = NULL;
    CI_CHECK("historical epoch mutation fixture writes",
             mk_write(FIX, "build/obj/epochs/old/foo.d",
                      "build/obj/foo.o: lib/net/src/foo.c "
                      "lib/net/include/net/foo.h\n"));
    codeindex_test_reset_exact_bytes_read();
    uint64_t warm_start_us = monotonic_us();
    ci = codeindex_open(FIX);
    uint64_t warm_elapsed_us = monotonic_us() - warm_start_us;
    uint64_t warm_exact_bytes = codeindex_test_exact_bytes_read();
    printf("  codeindex: warm-open elapsed_us=%llu exact_bytes=%llu\n",
           (unsigned long long)warm_elapsed_us,
           (unsigned long long)warm_exact_bytes);
    CI_CHECK("warm open rereads zero exact-content bytes",
             ci && warm_exact_bytes == 0);
    CI_CHECK("fixture warm open stays below 250 ms",
             ci && warm_elapsed_us > 0 && warm_elapsed_us <= UINT64_C(250000));
    memset(includes, 0, sizeof(includes));
    nincludes = ci ? codeindex_includes_of_file(
        ci, "lib/net/src/foo.c", includes, 8) : -1;
    CI_CHECK("historical epoch depfiles cannot change active include edges",
             ci && nincludes == 1 &&
             strcmp(includes[0], "lib/net/include/net/foo.h") == 0);

    /* Warm acceptance uses an owner-controlled directory capability. Mode
     * drift fails closed before SQLite can consume the canonical pathname. */
    if (ci) { codeindex_close(ci); ci = NULL; }
    CI_CHECK("make codeindex directory insecure for boundary test",
             chmod(FIX "/.codeindex", 0777) == 0);
    struct codeindex *insecure = codeindex_open(FIX);
    CI_CHECK("warm open rejects group/world-writable codeindex directory",
             insecure == NULL);
    if (insecure) codeindex_close(insecure);
    CI_CHECK("restore owner-controlled codeindex directory",
             chmod(FIX "/.codeindex", 0755) == 0);
    ci = codeindex_open(FIX);
    CI_CHECK("owner-controlled warm index reopens", ci != NULL);

    /* file → group */
    struct ci_file cf;
    codeindex_file(ci, "lib/net/src/foo.c", &cf, &found);
    CI_CHECK("foo.c maps to group lib/net", found &&
             strcmp(cf.group, "lib/net") == 0);

    /* file → purpose (§1.1): stem-dashed header, explicit override, none */
    codeindex_file(ci, "lib/net/src/purpose_stem.c", &cf, &found);
    CI_CHECK("stem-dashed header yields the bare description", found &&
             strcmp(cf.purpose, "derives its purpose from the stem header.") == 0);

    codeindex_file(ci, "lib/net/src/purpose_override.c", &cf, &found);
    CI_CHECK("explicit /* purpose: X */ override wins", found &&
             strcmp(cf.purpose, "explicit override wins.") == 0);

    codeindex_file(ci, "lib/net/src/purpose_none.c", &cf, &found);
    CI_CHECK("interior-only comment yields empty purpose", found &&
             cf.purpose[0] == '\0');

    codeindex_file(ci, "lib/net/src/purpose_after_license.c", &cf, &found);
    CI_CHECK("license lines are skipped before a real purpose", found &&
             strcmp(cf.purpose,
                    "describes behavior after the license.") == 0);

    codeindex_file(ci, "lib/net/src/purpose_license_only.c", &cf, &found);
    CI_CHECK("license-only header yields empty purpose", found &&
             cf.purpose[0] == '\0');

    /* ── 8: every developer-facing source root is indexed ── */
    codeindex_file(ci, "src/main.c", &cf, &found);
    CI_CHECK("src/main.c is indexed in root", found &&
             strcmp(cf.group, "root") == 0 &&
             strcmp(cf.purpose, "fixture top-level node entry.") == 0);

    codeindex_file(ci, "ports/include/ports/fixture_port.h", &cf, &found);
    CI_CHECK("ports header is indexed in ports", found &&
             strcmp(cf.group, "ports") == 0 &&
             strcmp(cf.purpose, "fixture hexagonal interface.") == 0);

    codeindex_file(ci, "lib/test/src/test_fixture_indexed.c", &cf, &found);
    CI_CHECK("lib/test source is indexed in lib/test", found &&
             strcmp(cf.group, "lib/test") == 0 &&
             strcmp(cf.purpose, "fixture test translation unit.") == 0);

    codeindex_symbol(ci, "fixture_root_main", &s, &found);
    CI_CHECK("src symbol is searchable", found && s.kind == 'T');
    codeindex_symbol(ci, "fixture_port_probe", &s, &found);
    CI_CHECK("ports declaration is searchable", found && s.kind == 'T');
    codeindex_symbol(ci, "test_fixture_indexed", &s, &found);
    CI_CHECK("lib/test symbol is searchable", found && s.kind == 'T');

    codeindex_file(ci, "lib/test/build/generated_should_not_index.c",
                   &cf, &found);
    CI_CHECK("generated test build directory stays pruned", !found);

    /* group hierarchy contains lib/net with parent lib */
    struct ci_group groups[256];
    int ng = codeindex_groups(ci, groups, 256);
    bool has_libnet = false, has_appsvc = false, has_libtest = false;
    for (int i = 0; i < ng; i++) {
        if (strcmp(groups[i].path, "lib/net") == 0 &&
            strcmp(groups[i].parent, "lib") == 0) has_libnet = true;
        if (strcmp(groups[i].path, "app/services") == 0) has_appsvc = true;
        if (strcmp(groups[i].path, "lib/test") == 0 &&
            strcmp(groups[i].parent, "lib") == 0) has_libtest = true;
    }
    CI_CHECK("group hierarchy has lib/net, lib/test, and app/services",
             has_libnet && has_libtest && has_appsvc);

    /* card render */
    char card[1024];
    int cl = codeindex_render_card(ci, "foo_main", card, sizeof(card));
    CI_CHECK("card renders foo_main", cl > 0 && strstr(card, "foo_main") &&
             strstr(card, "func"));

    /* ── 7a: file counts (ci_store_count_files_in_group via the public
     * wrapper). The fixture has a production lib/net module plus lib/test. */
    {
        struct ci_file fbuf[16];
        int listed = codeindex_files_in_group(ci, "lib/net", fbuf, 16);
        int direct = codeindex_count_files_in_group(ci, "lib/net", false);
        int direct_test = codeindex_count_files_in_group(ci, "lib/test", false);
        int recur_lib = codeindex_count_files_in_group(ci, "lib", true);
        int recur_self = codeindex_count_files_in_group(ci, "lib/net", true);
        int missing = codeindex_count_files_in_group(ci, "lib/nope", true);
        CI_CHECK("direct count equals the listed file count",
                 direct >= 2 && direct == listed);
        CI_CHECK("recursive count on parent 'lib' aggregates lib/net children",
                 direct_test == 1 && recur_lib == direct + direct_test);
        CI_CHECK("recursive count on a leaf group equals its direct count",
                 recur_self == direct);
        CI_CHECK("unknown group counts zero (not an error)", missing == 0);
    }

    /* ── 7b: routing-link parity — `code tests <path>`'s route MUST equal
     * `dev test plan`'s proof_group for the same single changed file. Pure
     * path→route on both sides (no index/fixture dependency); the files need
     * not exist. Every case is a non-docs, non-hotswap single file, so devloop
     * takes the RELOAD path where proof_group is defined. This tripwire fails
     * the instant native_code_command.c's consensus/route logic drifts from
     * devloop_plan.c. */
    {
        static const char *const parity_paths[] = {
            "lib/net/src/download.c",             /* -> "download" */
            "core/consensus/src/pow.c",           /* -> "consensus_parity" */
            "core/math/src/arith_uint256.c",      /* sealed core -> consensus_parity */
            "app/services/src/node_health_service.c", /* -> "node_health_service" */
            "lib/net/src/msg_blocks.c",           /* -> "msg_handlers" */
            "lib/bloom/src/zzz_unmapped_xyz.c",   /* no rule -> "make_lint_gates" */
        };
        bool all_agree = true;
        for (size_t i = 0; i < sizeof(parity_paths) / sizeof(parity_paths[0]); i++) {
            const char *files[1] = { parity_paths[i] };
            struct zcl_devloop_plan plan;
            bool ok = zcl_devloop_plan_files(files, 1, &plan);
            const char *my_route =
                zcl_native_code_route_for_path(parity_paths[i], NULL, NULL);
            if (!ok || plan.action != ZCL_DEVLOOP_RELOAD || !my_route ||
                !plan.proof_group || strcmp(my_route, plan.proof_group) != 0) {
                printf("    parity MISMATCH for %s: code=%s devloop=%s\n",
                       parity_paths[i], my_route ? my_route : "(null)",
                       ok ? plan.proof_group : "(plan failed)");
                all_agree = false;
            }
        }
        CI_CHECK("code tests route == dev test plan proof_group (all cases)",
                 all_agree);
    }

    /* ── 1: build determinism ── */
    char *dump1 = dump_symbols(ci);
    CI_CHECK("dump #1 non-empty", dump1 && dump1[0]);
    CI_CHECK("legacy WAL sidecar fixture writes",
             mk_write(FIX, ".codeindex/index.kv-wal", "legacy-wal-bytes") &&
             mk_write(FIX, ".codeindex/index.kv-shm", "legacy-shm-bytes"));
    CI_CHECK("forced rebuild publishes after legacy sidecars",
             codeindex_rebuild(ci));
    CI_CHECK("publication removes legacy WAL/SHM names",
             access(FIX "/.codeindex/index.kv-wal", F_OK) != 0 &&
             access(FIX "/.codeindex/index.kv-shm", F_OK) != 0);
    char *dump2 = dump_symbols(ci);
    CI_CHECK("rebuild is deterministic (dump identical)",
             dump1 && dump2 && strcmp(dump1, dump2) == 0);

    /* ── 3: rebuild-from-scratch identity ── */
    codeindex_close(ci);
    system("rm -rf " FIX "/.codeindex");
    ci = codeindex_open(FIX);
    char *dump3 = ci ? dump_symbols(ci) : NULL;
    CI_CHECK("from-scratch rebuild matches",
             dump1 && dump3 && strcmp(dump1, dump3) == 0);

    /* ── 4: staleness ⇒ auto-rebuild ── */
    if (ci) codeindex_close(ci);
    {
        char appended[4096];
        snprintf(appended, sizeof(appended), "%s\nint foo_added(void){return 7;}\n",
                 FOO_C);
        mk_write(FIX, "lib/net/src/foo.c", appended);
    }
    ci = codeindex_open(FIX);
    CI_CHECK("reopen after edit", ci != NULL);
    found = false;
    if (ci) codeindex_symbol(ci, "foo_added", &s, &found);
    CI_CHECK("staleness auto-rebuild reflects the edit",
             found && s.kind == 'T');

    /* Depfiles are consumed index inputs too. A same-size edit with the exact
     * previous mtime must invalidate include edges even when every C/H byte is
     * unchanged. */
    static const char dep_b[] =
        "build/obj/foo.o: lib/net/src/foo.c lib/net/include/net/bar.h\n";
    struct stat dep_st;
    bool dep_stat = stat(FIX "/build/obj/foo.d", &dep_st) == 0;
    bool dep_write = mk_write(FIX, "build/obj/foo.d", dep_b);
    struct timespec dep_times[2];
    if (dep_stat) {
        dep_times[0] = dep_st.st_atim;
        dep_times[1] = dep_st.st_mtim;
    }
    bool dep_mtime = dep_stat && dep_write &&
        utimensat(AT_FDCWD, FIX "/build/obj/foo.d", dep_times, 0) == 0;
    CI_CHECK("same-size depfile edit restores exact previous mtime",
             dep_mtime);
    if (ci) { codeindex_close(ci); ci = NULL; }
    ci = codeindex_open(FIX);
    memset(includes, 0, sizeof(includes));
    nincludes = ci ? codeindex_includes_of_file(
        ci, "lib/net/src/foo.c", includes, 8) : -1;
    /* Both release and test object-root aliases are active. Changing one
     * profile must rebuild the union while preserving the other profile's
     * still-current edge. */
    CI_CHECK("depfile digest rebuilds the include graph",
             ci && nincludes == 2 &&
             strcmp(includes[0], "lib/net/include/net/bar.h") == 0 &&
             strcmp(includes[1], "lib/net/include/net/foo.h") == 0);

    /* ── 9a: the freshness root is bytes, not (mtime,size). Preserve the
     * exact original mtime while changing a same-length symbol name. */
    if (ci) { codeindex_close(ci); ci = NULL; }
    char source_current[16384];
    snprintf(source_current, sizeof(source_current),
             "%s\nint ci_same_aaaa(void){return 1;}\n", FOO_C);
    CI_CHECK("content-freshness fixture A writes",
             mk_write(FIX, "lib/net/src/foo.c", source_current));
    ci = codeindex_open(FIX);
    found = false;
    if (ci) codeindex_symbol(ci, "ci_same_aaaa", &s, &found);
    CI_CHECK("content-freshness fixture A is indexed", ci && found);
    if (ci) { codeindex_close(ci); ci = NULL; }

    struct stat same_st;
    bool same_stat = stat(FIX "/lib/net/src/foo.c", &same_st) == 0;
    char *same_name = strstr(source_current, "ci_same_aaaa");
    if (same_name) memcpy(same_name, "ci_same_bbbb", strlen("ci_same_bbbb"));
    bool same_write = same_name &&
        mk_write(FIX, "lib/net/src/foo.c", source_current);
    struct timespec same_times[2];
    if (same_stat) {
        same_times[0] = same_st.st_atim;
        same_times[1] = same_st.st_mtim;
    }
    bool same_mtime = same_stat && same_write &&
        utimensat(AT_FDCWD, FIX "/lib/net/src/foo.c", same_times, 0) == 0;
    CI_CHECK("same-size edit restores the exact previous mtime", same_mtime);
    ci = codeindex_open(FIX);
    bool found_new = false, found_old = true;
    if (ci) {
        codeindex_symbol(ci, "ci_same_bbbb", &s, &found_new);
        codeindex_symbol(ci, "ci_same_aaaa", &s, &found_old);
    }
    CI_CHECK("content digest rejects same-size/same-mtime stale index",
             ci && found_new && !found_old);

    /* ── 9b: publication is rename-over, not unlink-then-rename. A reader
     * already bound to generation A remains valid while B becomes canonical. */
    struct codeindex *old_reader = ci;
    size_t used = strlen(source_current);
    snprintf(source_current + used, sizeof(source_current) - used,
             "int ci_retained_new(void){return 2;}\n");
    CI_CHECK("old-reader replacement fixture writes",
             mk_write(FIX, "lib/net/src/foo.c", source_current));
    struct codeindex *new_reader = codeindex_open(FIX);
    bool old_known = false, old_new = true, new_new = false;
    if (old_reader) {
        codeindex_symbol(old_reader, "ci_same_bbbb", &s, &old_known);
        codeindex_symbol(old_reader, "ci_retained_new", &s, &old_new);
    }
    if (new_reader)
        codeindex_symbol(new_reader, "ci_retained_new", &s, &new_new);
    CI_CHECK("old reader retains complete prior generation",
             old_reader && old_known && !old_new);
    CI_CHECK("new reader sees atomically published generation",
             new_reader && new_new);
    if (old_reader) codeindex_close(old_reader);
    ci = new_reader;

    /* ── 9c: a substituted stage name never receives SQLite writes. The
     * retained O_EXCL descriptor becomes unlinked, identity verification
     * refuses publication, and both victim + prior canonical stay intact. */
    static const char victim_path[] = FIX "/stage-victim.bin";
    static const char victim_bytes[] = "stage-victim-must-not-change";
    CI_CHECK("stage-substitution victim fixture writes",
             mk_write(FIX, "stage-victim.bin", victim_bytes));
    used = strlen(source_current);
    snprintf(source_current + used, sizeof(source_current) - used,
             "int ci_stage_symlink(void){return 6;}\n");
    CI_CHECK("symlink-substitution source fixture writes",
             mk_write(FIX, "lib/net/src/foo.c", source_current));
    codeindex_test_set_stage_tamper(CODEINDEX_TEST_STAGE_TAMPER_SYMLINK,
                                    victim_path);
    CI_CHECK("symlink-substituted stage is rejected",
             ci && !codeindex_rebuild(ci));
    codeindex_test_set_stage_tamper(CODEINDEX_TEST_STAGE_TAMPER_NONE, NULL);
    CI_CHECK("symlink victim bytes remain unchanged",
             file_equals(victim_path, victim_bytes));
    CI_CHECK("symlink substitution preserves prior canonical generation",
             !published_index_has_symbol("ci_stage_symlink"));
    CI_CHECK("symlink substitution leaves no staging name",
             no_staging_files());

    used = strlen(source_current);
    snprintf(source_current + used, sizeof(source_current) - used,
             "int ci_stage_hardlink(void){return 7;}\n");
    CI_CHECK("hardlink-substitution source fixture writes",
             mk_write(FIX, "lib/net/src/foo.c", source_current));
    codeindex_test_set_stage_tamper(CODEINDEX_TEST_STAGE_TAMPER_HARDLINK,
                                    victim_path);
    CI_CHECK("hardlink-substituted stage is rejected",
             ci && !codeindex_rebuild(ci));
    codeindex_test_set_stage_tamper(CODEINDEX_TEST_STAGE_TAMPER_NONE, NULL);
    CI_CHECK("hardlink victim bytes remain unchanged",
             file_equals(victim_path, victim_bytes));
    CI_CHECK("hardlink substitution preserves prior canonical generation",
             !published_index_has_symbol("ci_stage_hardlink"));
    CI_CHECK("hardlink substitution leaves no staging name",
             no_staging_files());
    CI_CHECK("clean rebuild succeeds after stage substitutions",
             ci && codeindex_rebuild(ci));

    /* ── 9d: process death before rename leaves the old generation; a later
     * open removes the abandoned unique stage and publishes a complete new
     * generation. */
    if (ci) { codeindex_close(ci); ci = NULL; }
    used = strlen(source_current);
    snprintf(source_current + used, sizeof(source_current) - used,
             "int ci_crash_before(void){return 3;}\n");
    CI_CHECK("pre-rename crash fixture writes",
             mk_write(FIX, "lib/net/src/foo.c", source_current));
    CI_CHECK("SIGKILL fires immediately before publication rename",
             crash_rebuild_at(CODEINDEX_TEST_CRASH_BEFORE_RENAME));
    CI_CHECK("pre-rename crash preserves the prior canonical generation",
             !published_index_has_symbol("ci_crash_before"));
    ci = codeindex_open(FIX);
    found = false;
    if (ci) codeindex_symbol(ci, "ci_crash_before", &s, &found);
    CI_CHECK("next open rebuilds after pre-rename crash", ci && found);
    CI_CHECK("next opener removes abandoned staging files", no_staging_files());

    /* A kill after rename has exactly the other legal winner: the fully
     * committed new file. Directory fsync is a power-loss boundary; SIGKILL
     * cannot manufacture a hybrid SQLite generation. */
    if (ci) { codeindex_close(ci); ci = NULL; }
    used = strlen(source_current);
    snprintf(source_current + used, sizeof(source_current) - used,
             "int ci_crash_after(void){return 4;}\n");
    CI_CHECK("post-rename crash fixture writes",
             mk_write(FIX, "lib/net/src/foo.c", source_current));
    CI_CHECK("SIGKILL fires immediately after publication rename",
             crash_rebuild_at(CODEINDEX_TEST_CRASH_AFTER_RENAME));
    CI_CHECK("post-rename crash exposes one complete new generation",
             published_index_has_symbol("ci_crash_after"));
    ci = codeindex_open(FIX);
    found = false;
    if (ci) codeindex_symbol(ci, "ci_crash_after", &s, &found);
    CI_CHECK("post-rename generation reopens and verifies", ci && found);
    CI_CHECK("post-rename crash leaves no staging file", no_staging_files());

    /* ── 9e: 32 processes start with no index. Exactly one rebuilds; losers
     * wait, recheck the winner's content root, and adopt it. */
    if (ci) { codeindex_close(ci); ci = NULL; }
    used = strlen(source_current);
    snprintf(source_current + used, sizeof(source_current) - used,
             "int ci_concurrent_32(void){return 5;}\n");
    CI_CHECK("32-way cold-open fixture writes",
             mk_write(FIX, "lib/net/src/foo.c", source_current));
    system("rm -rf " FIX "/.codeindex");
    CI_CHECK("32 concurrent cold opens all adopt one complete generation",
             concurrent_open_32("ci_concurrent_32"));
    CI_CHECK("32-way publication leaves no staging files", no_staging_files());
    ci = codeindex_open(FIX);
    found = false;
    if (ci) codeindex_symbol(ci, "ci_concurrent_32", &s, &found);
    CI_CHECK("32-way winner is the durable canonical index", ci && found);
    CI_CHECK("32 concurrent warm opens retain the durable generation",
             concurrent_open_32("ci_concurrent_32"));

    /* ── 5: verify-on-read rejects a corrupted row ── */
    if (ci) codeindex_close(ci);
    CI_CHECK("corrupt a symbol row", corrupt_symbol("foo_main"));
    ci = codeindex_open(FIX);
    found = true;
    if (ci) codeindex_symbol(ci, "foo_main", &s, &found);
    CI_CHECK("corrupted row is rejected on read", ci && found == false);
    if (ci) codeindex_close(ci);

    free(dump1); free(dump2); free(dump3);

    /* ── 6: group parity vs Makefile + shapes ── */
    {
        char mk[64][64];
        int mn = makefile_lib_modules(mk);
        size_t cn = 0;
        const char *const *code = ci_lib_modules(&cn);
        /* set equality (both directions) */
        bool all_mk_in_code = true, all_code_in_mk = true;
        for (int i = 0; i < mn; i++) {
            bool hit = false;
            for (size_t j = 0; j < cn; j++)
                if (strcmp(mk[i], code[j]) == 0) { hit = true; break; }
            if (!hit) all_mk_in_code = false;
        }
        for (size_t j = 0; j < cn; j++)
            if (!set_contains(mk, mn, code[j])) all_code_in_mk = false;
        CI_CHECK("lib module list matches Makefile LIB_MODULES",
                 mn > 0 && (size_t)mn == cn && all_mk_in_code && all_code_in_mk);

        size_t sn = 0;
        const char *const *shapes = ci_app_shapes(&sn);
        const char *expect[] = { "conditions", "controllers", "events", "jobs",
                                 "models", "services", "supervisors", "views" };
        bool shapes_ok = (sn == 8);
        for (size_t i = 0; shapes_ok && i < 8; i++) {
            bool hit = false;
            for (size_t j = 0; j < sn; j++)
                if (strcmp(expect[i], shapes[j]) == 0) { hit = true; break; }
            if (!hit) shapes_ok = false;
        }
        CI_CHECK("app shape list is the eight canonical shapes", shapes_ok);
    }

    return failures;
}
