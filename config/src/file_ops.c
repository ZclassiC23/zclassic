/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * File operations for data import/export.
 * Always byte-copy. Never hardlink or symlink. */

#include "config/file_ops.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool file_copy(const char *src, const char *dst)
{
    struct stat st;
    if (stat(src, &st) != 0 || !S_ISREG(st.st_mode))
        return false;

    unlink(dst);
    FILE *fin = fopen(src, "rb");
    if (!fin) return false;
    FILE *fout = fopen(dst, "wb");
    if (!fout) { fclose(fin); return false; }
    char buf[256 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fin)) > 0) {
        if (fwrite(buf, 1, n, fout) != n) {
            fclose(fin); fclose(fout); return false;
        }
    }
    fclose(fin);
    fclose(fout);
    return true;
}

bool dir_copy(const char *src_dir, const char *dst_dir)
{
    dir_remove_shallow(dst_dir);
    if (mkdir(dst_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "Warning: mkdir(%s) failed: %s\n",
                dst_dir, strerror(errno));
        return false;
    }
    DIR *d = opendir(src_dir);
    if (!d) return false;
    struct dirent *ent;
    int copied = 0, failed = 0;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (strcmp(ent->d_name, "LOCK") == 0) continue;
        char s[1024], de[1024];
        snprintf(s, sizeof(s), "%s/%s", src_dir, ent->d_name);
        snprintf(de, sizeof(de), "%s/%s", dst_dir, ent->d_name);
        if (file_copy(s, de))
            copied++;
        else
            failed++;
    }
    closedir(d);
    printf(" %d files", copied);
    if (failed > 0)
        fprintf(stderr, "\nWarning: %d files failed to copy in %s\n",
                failed, dst_dir);
    return failed == 0;
}

int block_files_copy(const char *src_dir, const char *dst_dir)
{
    int count = 0;
    char src[1024], dst[1024];
    for (int i = 0; i < 9999; i++) {
        struct stat st;
        snprintf(src, sizeof(src), "%s/blk%05d.dat", src_dir, i);
        if (stat(src, &st) != 0) break;
        snprintf(dst, sizeof(dst), "%s/blk%05d.dat", dst_dir, i);
        if (!file_copy(src, dst))
            return -1;
        count++;
        snprintf(src, sizeof(src), "%s/rev%05d.dat", src_dir, i);
        if (stat(src, &st) == 0) {
            snprintf(dst, sizeof(dst), "%s/rev%05d.dat", dst_dir, i);
            if (!file_copy(src, dst))
                return -1;
        }
    }
    return count;
}

void block_files_clean(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if ((strncmp(ent->d_name, "blk", 3) == 0 ||
             strncmp(ent->d_name, "rev", 3) == 0) &&
            strstr(ent->d_name, ".dat")) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
            unlink(path);
        }
    }
    closedir(d);
}

void dir_remove_shallow(const char *dir)
{
    struct stat lst;
    if (lstat(dir, &lst) != 0) return;
    if (S_ISLNK(lst.st_mode)) { unlink(dir); return; }
    if (!S_ISDIR(lst.st_mode)) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        unlink(path);
    }
    closedir(d);
    rmdir(dir);
}

void dir_remove_tree(const char *dir)
{
    struct stat lst;
    if (lstat(dir, &lst) != 0)
        return;
    if (S_ISLNK(lst.st_mode) || !S_ISDIR(lst.st_mode)) {
        unlink(dir);
        return;
    }

    DIR *d = opendir(dir);
    if (!d)
        return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        struct stat child_st;
        if (lstat(path, &child_st) != 0)
            continue;
        if (S_ISDIR(child_st.st_mode) && !S_ISLNK(child_st.st_mode))
            dir_remove_tree(path);
        else
            unlink(path);
    }

    closedir(d);
    rmdir(dir);
}
