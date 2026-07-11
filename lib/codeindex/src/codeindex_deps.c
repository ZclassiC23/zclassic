/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * codeindex_deps — turn the compiler's own dependency files (the depfiles
 * under build/, extension .d) into include edges. Each depfile records
 * "<obj>: <src.c> <hdr.h> ..."; we
 * emit (source, header) pairs for in-tree prerequisites, which is the exact
 * include graph the build already computed — no re-parsing of #include lines,
 * no guessing search paths. Best-effort: if build/ is absent (a fresh tree or
 * the test fixture), no edges are produced. */

#include "codeindex_priv.h"

#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Rewrite an absolute or ./-relative depfile token to repo-relative, or return
 * false if the token is outside the tree (a system header). */
static bool to_relpath(const char *root, const char *tok, char out[CI_PATH_MAX])
{
    size_t rl = strlen(root);
    if (strncmp(tok, root, rl) == 0 && tok[rl] == '/') {
        snprintf(out, CI_PATH_MAX, "%s", tok + rl + 1);
        return true;
    }
    if (tok[0] == '/')
        return false;  /* absolute, outside root */
    /* already relative (build usually emits repo-relative prereqs) */
    if (strncmp(tok, "./", 2) == 0) tok += 2;
    if (tok[0] == '/') return false;
    /* reject paths that escape upward or reference vendored system trees */
    if (strncmp(tok, "../", 3) == 0) return false;
    snprintf(out, CI_PATH_MAX, "%s", tok);
    return true;
}

static bool has_ext(const char *s, const char *ext)
{
    size_t a = strlen(s), b = strlen(ext);
    return a >= b && strcmp(s + a - b, ext) == 0;
}

/* Parse one depfile's text; emit (src, dep) edges. */
static void parse_depfile(const char *root, char *text, size_t len,
                          ci_dep_cb cb, void *user)
{
    /* fold line continuations: "\\\n" → "  " */
    for (size_t i = 0; i + 1 < len; i++) {
        if (text[i] == '\\' && text[i + 1] == '\n') {
            text[i] = ' ';
            text[i + 1] = ' ';
        }
    }
    /* process one logical rule per physical line */
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        char *rhs = colon + 1;
        /* tokenize prerequisites */
        char src_rel[CI_PATH_MAX];
        bool have_src = false;
        char *tsave = NULL;
        for (char *tok = strtok_r(rhs, " \t", &tsave); tok;
             tok = strtok_r(NULL, " \t", &tsave)) {
            char rel[CI_PATH_MAX];
            if (!to_relpath(root, tok, rel)) continue;
            if (!have_src && (has_ext(rel, ".c") || has_ext(rel, ".cc") ||
                              has_ext(rel, ".c23"))) {
                snprintf(src_rel, sizeof(src_rel), "%s", rel);
                have_src = true;
                continue;
            }
            if (have_src && (has_ext(rel, ".h") || has_ext(rel, ".hpp") ||
                             has_ext(rel, ".hh")))
                cb(src_rel, rel, user);
        }
    }
}

/* Recurse build/ collecting *.d files. */
static bool walk_d(const char *root, const char *reldir, ci_dep_cb cb,
                   void *user)
{
    char full[CI_PATH_MAX];
    snprintf(full, sizeof(full), "%s/%s", root, reldir);
    DIR *d = opendir(full);
    if (!d) return true;  /* absent is fine */
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char child[CI_PATH_MAX];
        int n = snprintf(child, sizeof(child), "%s/%s", reldir, e->d_name);
        if (n <= 0 || (size_t)n >= sizeof(child)) continue;
        char cfull[CI_PATH_MAX];
        snprintf(cfull, sizeof(cfull), "%s/%s", root, child);
        struct stat st;
        if (stat(cfull, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            walk_d(root, child, cb, user);
        } else if (has_ext(e->d_name, ".d")) {
            int fd = open(cfull, O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            size_t cap = (size_t)st.st_size + 1;
            char *buf = zcl_malloc(cap ? cap : 1, "ci_depbuf");
            if (!buf) { close(fd); continue; }
            ssize_t r = read(fd, buf, cap - 1);
            close(fd);
            if (r > 0) {
                buf[r] = '\0';
                parse_depfile(root, buf, (size_t)r, cb, user);
            }
            free(buf);
        }
    }
    closedir(d);
    return true;
}

bool ci_deps_scan(const char *root, ci_dep_cb cb, void *user)
{
    if (!root || !cb) LOG_FAIL("codeindex", "null arg to deps_scan");
    return walk_d(root, "build", cb, user);
}
