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
 *
 * All scratch work happens under ./test-tmp/ (project no-/tmp convention). */

#include "test/test_helpers.h"

#include "codeindex/codeindex.h"
#include "codeindex/codeindex_build.h"

#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

static bool write_fixture(void)
{
    return mk_write(FIX, "lib/net/src/foo.c", FOO_C) &&
           mk_write(FIX, "lib/net/include/net/foo.h", FOO_H);
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

    /* file → group */
    struct ci_file cf;
    codeindex_file(ci, "lib/net/src/foo.c", &cf, &found);
    CI_CHECK("foo.c maps to group lib/net", found &&
             strcmp(cf.group, "lib/net") == 0);

    /* group hierarchy contains lib/net with parent lib */
    struct ci_group groups[256];
    int ng = codeindex_groups(ci, groups, 256);
    bool has_libnet = false, has_appsvc = false;
    for (int i = 0; i < ng; i++) {
        if (strcmp(groups[i].path, "lib/net") == 0 &&
            strcmp(groups[i].parent, "lib") == 0) has_libnet = true;
        if (strcmp(groups[i].path, "app/services") == 0) has_appsvc = true;
    }
    CI_CHECK("group hierarchy has lib/net and app/services",
             has_libnet && has_appsvc);

    /* card render */
    char card[1024];
    int cl = codeindex_render_card(ci, "foo_main", card, sizeof(card));
    CI_CHECK("card renders foo_main", cl > 0 && strstr(card, "foo_main") &&
             strstr(card, "func"));

    /* ── 1: build determinism ── */
    char *dump1 = dump_symbols(ci);
    CI_CHECK("dump #1 non-empty", dump1 && dump1[0]);
    codeindex_rebuild(ci);
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
