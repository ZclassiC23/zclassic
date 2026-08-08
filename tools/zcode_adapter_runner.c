/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: enter a filesystem-confined environment before a fixed C23 AI adapter. */

#define _GNU_SOURCE

#include "base/cleanse.h"
#include "platform/os_sandbox.h"
#include "util/result.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ADAPTER_PACKET_MAX (512u * 1024u)

volatile sig_atomic_t g_shutdown_requested = 0;

static bool adapter_beneath(const char *parent, const char *child)
{
    size_t len = strlen(parent);
    return strncmp(parent, child, len) == 0 && child[len] == '/';
}

static bool adapter_codex_path(char out[PATH_MAX])
{
    struct passwd pwd, *found = NULL;
    char scratch[16384];
    if (getpwuid_r(getuid(), &pwd, scratch, sizeof(scratch), &found) != 0 ||
        !found || !pwd.pw_dir || !pwd.pw_dir[0])
        return false;
    char link[PATH_MAX];
    int n = snprintf(link, sizeof(link), "%s/.local/bin/codex", pwd.pw_dir);
    if (n <= 0 || (size_t)n >= sizeof(link) || !realpath(link, out))
        return false;
    struct stat st;
    return stat(out, &st) == 0 && S_ISREG(st.st_mode) &&
           st.st_uid == getuid() && (st.st_mode & 0111u) != 0 &&
           (st.st_mode & 0022u) == 0;
}

static bool adapter_mkdir(const char *path)
{
    struct stat st;
    if (mkdir(path, 0700) == 0)
        return true;
    return errno == EEXIST && lstat(path, &st) == 0 && S_ISDIR(st.st_mode) &&
           st.st_uid == getuid() && (st.st_mode & 0077u) == 0;
}

static void adapter_rule_add(struct os_sandbox_path_rule *rules,
                             size_t *count, const char *path, bool write,
                             bool execute, bool create)
{
    if (access(path, F_OK) != 0)
        return;
    rules[*count] = (struct os_sandbox_path_rule){
        .path = path,
        .allow_read = true,
        .allow_write = write,
        .allow_execute = execute,
        .allow_create = create,
    };
    (*count)++;
}

/* RLIMIT_NPROC is charged across the real uid, not just this adapter. Rebase
 * the adapter's 128-task allowance over the already-running development
 * account so an unrelated node or compiler cannot make the fixed adapter
 * fail at startup. As with package_verify, an unreadable procfs fails closed. */
static bool adapter_nproc_limit(uint64_t *out)
{
    DIR *proc = opendir("/proc");
    if (!proc) return false;
    const uid_t me = getuid();
    uint64_t total = 0;
    struct dirent *entry;
    while ((entry = readdir(proc)) != NULL) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        char path[64];
        int n = snprintf(path, sizeof(path), "/proc/%s/task", entry->d_name);
        if (n <= 0 || (size_t)n >= sizeof(path)) continue;
        struct stat st;
        if (stat(path, &st) != 0 || st.st_uid != me) continue;
        DIR *tasks = opendir(path);
        if (!tasks) continue;
        struct dirent *task;
        while ((task = readdir(tasks)) != NULL)
            if (task->d_name[0] >= '0' && task->d_name[0] <= '9') total++;
        closedir(tasks);
    }
    closedir(proc);
    if (total > UINT64_MAX - 128u) return false;
    *out = total + 128u;
    return true;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "adapter_runner: candidate and packet required\n");
        return 64;
    }
    char candidate[PATH_MAX], packet[PATH_MAX], codex[PATH_MAX];
    if (!realpath(argv[1], candidate) || !realpath(argv[2], packet) ||
        !adapter_beneath(candidate, packet) || !adapter_codex_path(codex)) {
        fprintf(stderr, "adapter_runner: fixed adapter or paths unavailable\n");
        return 69;
    }
    int packet_fd = open(packet, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat packet_st;
    if (packet_fd < 0 || fstat(packet_fd, &packet_st) != 0 ||
        !S_ISREG(packet_st.st_mode) || packet_st.st_size <= 0 ||
        (uint64_t)packet_st.st_size > ADAPTER_PACKET_MAX) {
        if (packet_fd >= 0) close(packet_fd);
        fprintf(stderr, "adapter_runner: bounded packet refused\n");
        return 65;
    }
    const char *credential_name = "CODEX_API_KEY";
    const char *inherited_key = getenv(credential_name);
    const char *access_token = getenv("CODEX_ACCESS_TOKEN");
    if ((!inherited_key || !inherited_key[0]) && access_token &&
        access_token[0]) {
        credential_name = "CODEX_ACCESS_TOKEN";
        inherited_key = access_token;
    }
    if (!inherited_key || !inherited_key[0] || strlen(inherited_key) > 16384u ||
        (getenv("CODEX_API_KEY") && getenv("CODEX_API_KEY")[0] &&
         access_token && access_token[0])) {
        close(packet_fd);
        fprintf(stderr, "adapter_runner: one supported Codex credential required\n");
        return 69;
    }
    char *api_key = strdup(inherited_key);
    char adapter_home[PATH_MAX], adapter_tmp[PATH_MAX];
    int hn = snprintf(adapter_home, sizeof(adapter_home),
                      "%s/.zcode-adapter-home", candidate);
    int tn = snprintf(adapter_tmp, sizeof(adapter_tmp),
                      "%s/.zcode-adapter-tmp", candidate);
    if (!api_key || hn <= 0 || (size_t)hn >= sizeof(adapter_home) ||
        tn <= 0 || (size_t)tn >= sizeof(adapter_tmp) ||
        !adapter_mkdir(adapter_home) || !adapter_mkdir(adapter_tmp)) {
        free(api_key); close(packet_fd);
        fprintf(stderr, "adapter_runner: private adapter directories unavailable\n");
        return 73;
    }
    if (clearenv() != 0 || setenv(credential_name, api_key, 1) != 0 ||
        setenv("HOME", adapter_home, 1) != 0 ||
        setenv("CODEX_HOME", adapter_home, 1) != 0 ||
        setenv("TMPDIR", adapter_tmp, 1) != 0 ||
        setenv("PATH", "/usr/bin:/bin", 1) != 0 ||
        setenv("SSL_CERT_FILE", "/etc/ssl/certs/ca-certificates.crt", 1) != 0) {
        memory_cleanse(api_key, strlen(api_key)); free(api_key); close(packet_fd);
        fprintf(stderr, "adapter_runner: environment scrub failed\n");
        return 70;
    }
    memory_cleanse(api_key, strlen(api_key));
    free(api_key);

    uint64_t nproc_limit = 0;
    if (!adapter_nproc_limit(&nproc_limit)) {
        close(packet_fd);
        fprintf(stderr, "adapter_runner: process-limit baseline unavailable\n");
        return 70;
    }
    struct os_sandbox_rlimits limits = {
        .as_bytes = UINT64_C(4) * 1024u * 1024u * 1024u,
        .cpu_seconds = 300,
        .nproc = nproc_limit,
        .fsize_bytes = 16u * 1024u * 1024u,
        .nofile = 256,
        .core_bytes = 0,
    };
    struct zcl_result limited = os_sandbox_set_rlimits(&limits);
    if (!limited.ok || !os_sandbox_no_new_privs()) {
        close(packet_fd);
        fprintf(stderr, "adapter_runner: resource confinement unavailable\n");
        return 70;
    }
    struct os_sandbox_path_rule rules[20];
    size_t count = 0;
    adapter_rule_add(rules, &count, candidate, true, false, true);
    adapter_rule_add(rules, &count, codex, false, true, false);
    adapter_rule_add(rules, &count, "/usr", false, true, false);
    adapter_rule_add(rules, &count, "/bin", false, true, false);
    adapter_rule_add(rules, &count, "/lib", false, true, false);
    adapter_rule_add(rules, &count, "/lib64", false, true, false);
    adapter_rule_add(rules, &count, "/etc/ssl", false, false, false);
    adapter_rule_add(rules, &count, "/etc/resolv.conf", false, false, false);
    adapter_rule_add(rules, &count, "/etc/hosts", false, false, false);
    adapter_rule_add(rules, &count, "/etc/nsswitch.conf", false, false, false);
    adapter_rule_add(rules, &count, "/etc/passwd", false, false, false);
    adapter_rule_add(rules, &count, "/etc/group", false, false, false);
    adapter_rule_add(rules, &count, "/dev/null", true, false, false);
    adapter_rule_add(rules, &count, "/dev/urandom", false, false, false);
    adapter_rule_add(rules, &count, OS_SANDBOX_PROC_SELF_PATH,
                     false, false, false);
    struct zcl_result confined = os_sandbox_landlock_restrict(rules, count);
    if (!confined.ok) {
        close(packet_fd);
        fprintf(stderr, "adapter_runner: Landlock confinement unavailable\n");
        return 70;
    }
    if (chdir(candidate) != 0 || dup2(packet_fd, STDIN_FILENO) < 0 ||
        dup2(STDOUT_FILENO, STDERR_FILENO) < 0) {
        close(packet_fd);
        fprintf(stderr, "adapter_runner: confined I/O setup failed\n");
        return 70;
    }
    if (packet_fd > STDERR_FILENO)
        close(packet_fd);
    const char *const codex_argv[] = {
        codex, "exec", "--sandbox", "workspace-write", "-C", candidate,
        "--skip-git-repo-check", "--ephemeral", "--ignore-user-config",
        "--ignore-rules", "--color", "never",
        "-c", "shell_environment_policy.inherit=none",
        "-c", "shell_environment_policy.set.PATH=\"/usr/bin:/bin\"",
        "-c", "shell_environment_policy.set.HOME=\".\"",
        "-c", "shell_environment_policy.set.TMPDIR=\".zcode-adapter-tmp\"",
        "-", NULL,
    };
    execv(codex, (char *const *)codex_argv);
    fprintf(stderr, "adapter_runner: fixed codex exec failed\n");
    return 127;
}
