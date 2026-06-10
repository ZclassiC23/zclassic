/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "util/util.h"
#include "chain/chainparamsbase.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

#ifdef __linux__
#include <sched.h>
#endif

#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

struct arg_entry g_args[MAX_ARGS];
int g_nargs = 0;

static char cachedDataDir[4096] = "";
static char cachedDataDirNet[4096] = "";

void ClearDataDirCache(void)
{
    cachedDataDir[0] = '\0';
    cachedDataDirNet[0] = '\0';
}

static int find_arg(const char *key)
{
    for (int i = 0; i < g_nargs; i++) {
        if (strcmp(g_args[i].key, key) == 0)
            return i;
    }
    return -1;
}

void ParseParameters(int argc, const char *const argv[])
{
    ClearDataDirCache();
    g_nargs = 0;
    for (int i = 1; i < argc && g_nargs < MAX_ARGS; i++) {
        const char *arg = argv[i];
        const char *eq = strchr(arg, '=');
        char key[MAX_ARG_LEN];
        char value[MAX_ARG_LEN];

        if (eq) {
            size_t klen = (size_t)(eq - arg);
            if (klen >= MAX_ARG_LEN) klen = MAX_ARG_LEN - 1;
            memcpy(key, arg, klen);
            key[klen] = '\0';
            snprintf(value, MAX_ARG_LEN, "%s", eq + 1);
        } else {
            snprintf(key, MAX_ARG_LEN, "%s", arg);
            value[0] = '\0';
        }

        if (key[0] != '-') break;

        /* --foo → -foo */
        const char *k = key;
        if (k[0] == '-' && k[1] == '-')
            k++;

        int idx = find_arg(k);
        if (idx >= 0) {
            snprintf(g_args[idx].value, MAX_ARG_LEN, "%s", value);
        } else {
            snprintf(g_args[g_nargs].key, MAX_ARG_LEN, "%s", k);
            snprintf(g_args[g_nargs].value, MAX_ARG_LEN, "%s", value);
            g_nargs++;
        }
    }

    /* Interpret -nofoo as -foo=0 */
    for (int i = 0; i < g_nargs; i++) {
        if (strncmp(g_args[i].key, "-no", 3) == 0) {
            char positive[MAX_ARG_LEN];
            snprintf(positive, sizeof(positive), "-%s", g_args[i].key + 3);
            if (find_arg(positive) < 0 && g_nargs < MAX_ARGS) {
                bool val = !GetBoolArg(g_args[i].key, false);
                snprintf(g_args[g_nargs].key, MAX_ARG_LEN, "%s", positive);
                snprintf(g_args[g_nargs].value, MAX_ARG_LEN, "%d", val ? 1 : 0);
                g_nargs++;
            }
        }
    }
}

const char *GetArg(const char *arg, const char *default_val)
{
    int idx = find_arg(arg);
    if (idx >= 0)
        return g_args[idx].value;
    return default_val;
}

int64_t GetArgInt(const char *arg, int64_t default_val)
{
    int idx = find_arg(arg);
    if (idx >= 0)
        return strtoll(g_args[idx].value, NULL, 10);
    return default_val;
}

bool GetBoolArg(const char *arg, bool default_val)
{
    int idx = find_arg(arg);
    if (idx >= 0) {
        if (g_args[idx].value[0] == '\0')
            return true;
        return atoi(g_args[idx].value) != 0;
    }
    return default_val;
}



bool LogAcceptCategory(const char *category)
{
    if (category != NULL) {
        /* Category logging emits only when -debug (optionally
         * -debug=<category>) was passed on the command line. */
        for (int i = 0; i < g_nargs; i++) {
            if (strcmp(g_args[i].key, "-debug") == 0) {
                if (g_args[i].value[0] == '\0' ||
                    strcmp(g_args[i].value, "1") == 0 ||
                    strcmp(g_args[i].value, category) == 0)
                    return true;
            }
        }
        return false;
    }
    return true;
}

int LogPrintStr(const char *str)
{
    fputs(str, stderr);
    return (int)strlen(str);
}

void GetDefaultDataDir(char *out, size_t out_size)
{
#ifdef _WIN32
    char path[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, path) == S_OK)
        snprintf(out, out_size, "%s\\ZClassic", path);
    else
        snprintf(out, out_size, "ZClassic");
#elif defined(__APPLE__)
    const char *home = getenv("HOME");
    if (home && home[0])
        snprintf(out, out_size, "%s/Library/Application Support/ZClassic", home);
    else
        snprintf(out, out_size, "/ZClassic");
#else
    const char *home = getenv("HOME");
    if (home && home[0])
        snprintf(out, out_size, "%s/.zclassic-c23", home);
    else
        snprintf(out, out_size, "/.zclassic-c23");
#endif
}

static void AppendNetworkDataDir(char *path, size_t path_size)
{
    const struct base_chain_params *bp = BaseParams();
    if (!bp || !bp->strDataDir[0])
        return;

    size_t len = strlen(path);
    if (len + 1 >= path_size)
        return;
#ifdef _WIN32
    snprintf(path + len, path_size - len, "\\%s", bp->strDataDir);
#else
    snprintf(path + len, path_size - len, "/%s", bp->strDataDir);
#endif
}

void SetDataDir(const char *datadir)
{
    ClearDataDirCache();
    if (!datadir || !datadir[0])
        return;

    snprintf(cachedDataDir, sizeof(cachedDataDir), "%s", datadir);
    snprintf(cachedDataDirNet, sizeof(cachedDataDirNet), "%s", datadir);
    AppendNetworkDataDir(cachedDataDirNet, sizeof(cachedDataDirNet));

#ifdef _WIN32
    CreateDirectoryA(cachedDataDir, NULL);
    CreateDirectoryA(cachedDataDirNet, NULL);
#else
    mkdir(cachedDataDir, 0700);
    mkdir(cachedDataDirNet, 0700);
#endif
}

void GetDataDir(bool fNetSpecific, char *out, size_t out_size)
{
    char *cached = fNetSpecific ? cachedDataDirNet : cachedDataDir;
    if (cached[0]) {
        snprintf(out, out_size, "%s", cached);
        return;
    }

    int idx = find_arg("-datadir");
    if (idx >= 0 && g_args[idx].value[0]) {
        snprintf(out, out_size, "%s", g_args[idx].value);
    } else {
        GetDefaultDataDir(out, out_size);
    }

    if (fNetSpecific) {
        AppendNetworkDataDir(out, out_size);
    }

#ifdef _WIN32
    CreateDirectoryA(out, NULL);
#else
    mkdir(out, 0700);
#endif

    snprintf(cached, 4096, "%s", out);
}






int GetNumCores(void)
{
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#elif defined(_SC_NPROCESSORS_ONLN)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#else
    return 1;
#endif
}
