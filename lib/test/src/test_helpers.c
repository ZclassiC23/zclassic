/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Shared test helper functions. */

#include "test/test_helpers.h"

int check_hex(const unsigned char *data, size_t len, const char *expected)
{
    char buf[256];
    for (size_t i = 0; i < len; i++)
        snprintf(buf + i * 2, 3, "%02x", data[i]);
    if (strcmp(buf, expected) != 0) {
        printf("FAIL\n  got:      %s\n  expected: %s\n", buf, expected);
        return 1;
    }
    printf("OK\n");
    return 0;
}

void test_hex_to_bytes_rev(const char *hex, uint8_t *out, int len)
{
    for (int i = 0; i < len; i++) {
        unsigned int b;
        sscanf(hex + 2*i, "%02x", &b);
        out[len - 1 - i] = (uint8_t)b;
    }
}

void test_hex_to_bytes(const char *hex, uint8_t *out, int len)
{
    for (int i = 0; i < len; i++) {
        unsigned int b;
        sscanf(hex + 2*i, "%02x", &b);
        out[i] = (uint8_t)b;
    }
}

void test_rm_rf(const char *dir)
{
    if (!dir || !dir[0]) return;
    char cmd[4200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    (void)!system(cmd);
}

int test_rm_rf_recursive(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return unlink(path);

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 ||
            strcmp(de->d_name, "..") == 0) {
            continue;
        }
        char child[768];
        int n = snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) continue;
        test_rm_rf_recursive(child);
    }
    closedir(d);
    return rmdir(path);
}

void test_make_tmpdir(char *buf, size_t n, const char *prefix,
                      const char *tag)
{
    test_fmt_tmpdir(buf, n, prefix, tag);
    test_cleanup_tmpdir(buf);
    mkdir("test-tmp", 0755);
    mkdir(buf, 0755);
}

void test_make_easy_consensus_params(struct consensus_params *p)
{
    memset(p, 0, sizeof(*p));
    for (int i = 0; i < 32; i++) p->powLimit.data[i] = 0xff;
}

void test_projection_paths(const char *dir, const char *name,
                           char *elog, size_t elog_n,
                           char *proj, size_t proj_n)
{
    snprintf(elog, elog_n, "%s/event_log.dat", dir);
    snprintf(proj, proj_n, "%s/%s_projection.db", dir, name);
}
