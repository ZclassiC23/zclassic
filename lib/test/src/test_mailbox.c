/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Unit tests for the mailbox primitive (lib/util/src/mailbox.c).
 *
 * Coverage:
 *   - create/destroy: input validation, capacity & msg_size accessors
 *   - SPSC: 1000 sends → 1000 recvs, deterministic order preserved
 *   - overflow: try_send returns false at capacity; drop_count
 *     increments
 *   - close: try_send returns false post-close; drain still works
 *   - MPSC: 4 producer threads × 256 messages, single consumer drains
 *     all 1024 with no loss */

#include "test/test_helpers.h"
#include "util/mailbox.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MBX_CHECK(name, expr) do { \
    printf("mailbox: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

struct producer_args {
    mailbox_t *m;
    int        tid;
    int        count;
};

static void *producer_thread(void *arg)
{
    struct producer_args *a = arg;
    /* Encode (tid, seq) in a uint64 so the consumer can verify no loss. */
    for (int i = 0; i < a->count; i++) {
        uint64_t msg = ((uint64_t)a->tid << 32) | (uint64_t)i;
        /* Spin-retry on overflow so the test never silently drops. */
        while (!mailbox_try_send(a->m, &msg)) {
            sched_yield();
        }
    }
    return NULL;
}

int test_mailbox(void)
{
    printf("\n=== mailbox tests ===\n");
    int failures = 0;

    /* ── create / destroy / validation ──────────────────────────── */
    {
        mailbox_t *m = mailbox_create(16, sizeof(int));
        MBX_CHECK("create OK", m != NULL);
        MBX_CHECK("capacity matches", mailbox_capacity(m) == 16);
        MBX_CHECK("depth starts 0", mailbox_depth(m) == 0);
        MBX_CHECK("not closed at create", !mailbox_is_closed(m));
        mailbox_destroy(m);

        MBX_CHECK("zero capacity → NULL",
                  mailbox_create(0, sizeof(int)) == NULL);
        MBX_CHECK("zero msg_size → NULL",
                  mailbox_create(16, 0) == NULL);
        mailbox_destroy(NULL);  /* must not crash */
    }

    /* ── SPSC: 1000 messages preserve FIFO order ────────────────── */
    {
        mailbox_t *m = mailbox_create(1000, sizeof(int));
        for (int i = 0; i < 1000; i++) {
            int v = i;
            if (!mailbox_try_send(m, &v)) {
                printf("send %d failed\n", i);
                failures++;
                break;
            }
        }
        MBX_CHECK("depth=1000 after 1000 sends",
                  mailbox_depth(m) == 1000);
        MBX_CHECK("sent_count=1000",
                  mailbox_sent_count(m) == 1000);

        int got;
        bool fifo_ok = true;
        for (int i = 0; i < 1000; i++) {
            if (!mailbox_try_recv(m, &got) || got != i) {
                fifo_ok = false;
                break;
            }
        }
        MBX_CHECK("FIFO order preserved", fifo_ok);
        MBX_CHECK("depth=0 after drain", mailbox_depth(m) == 0);
        MBX_CHECK("recv on empty returns false",
                  !mailbox_try_recv(m, &got));
        MBX_CHECK("recv_count=1000",
                  mailbox_recv_count(m) == 1000);
        mailbox_destroy(m);
    }

    /* ── overflow: ring full ─────────────────────────────────── */
    {
        mailbox_t *m = mailbox_create(4, sizeof(int));
        int v = 0;
        for (int i = 0; i < 4; i++) {
            v = i;
            MBX_CHECK("fill OK", mailbox_try_send(m, &v));
        }
        v = 99;
        MBX_CHECK("5th send returns false",
                  !mailbox_try_send(m, &v));
        MBX_CHECK("drop_count=1", mailbox_drop_count(m) == 1);
        MBX_CHECK("depth still capacity",
                  mailbox_depth(m) == mailbox_capacity(m));

        /* Drain one, send one — should succeed. */
        int got;
        mailbox_try_recv(m, &got);
        v = 100;
        MBX_CHECK("send after drain OK",
                  mailbox_try_send(m, &v));
        mailbox_destroy(m);
    }

    /* ── close: send fails, drain still works ────────────────── */
    {
        mailbox_t *m = mailbox_create(8, sizeof(int));
        int v = 42;
        mailbox_try_send(m, &v);
        mailbox_try_send(m, &v);
        MBX_CHECK("close transitions", mailbox_close(m));
        MBX_CHECK("close idempotent (returns false 2nd)",
                  !mailbox_close(m));
        MBX_CHECK("is_closed reflects state",
                  mailbox_is_closed(m));
        MBX_CHECK("send-after-close returns false",
                  !mailbox_try_send(m, &v));
        int got = 0;
        MBX_CHECK("recv-after-close drains 1",
                  mailbox_try_recv(m, &got) && got == 42);
        MBX_CHECK("recv-after-close drains 2",
                  mailbox_try_recv(m, &got) && got == 42);
        MBX_CHECK("recv after drained empty",
                  !mailbox_try_recv(m, &got));
        mailbox_destroy(m);
    }

    /* ── MPSC: 4 producers × 256 = 1024 messages, no loss ────── */
    {
        const int N_PROD = 4;
        const int PER    = 256;
        mailbox_t *m = mailbox_create(64, sizeof(uint64_t));

        pthread_t threads[4];
        struct producer_args args[4];
        for (int i = 0; i < N_PROD; i++) {
            args[i].m = m;
            args[i].tid = i;
            args[i].count = PER;
            pthread_create(&threads[i], NULL, producer_thread, &args[i]);
        }

        /* Consumer drains 1024 messages, tracking what's seen. */
        int seen[4][256] = {{0}};
        int drained = 0;
        const int target = N_PROD * PER;

        /* Use a generous bound to give producers time. */
        for (int spin = 0; spin < 1000000 && drained < target; spin++) {
            uint64_t msg;
            if (mailbox_try_recv(m, &msg)) {
                int tid = (int)(msg >> 32);
                int seq = (int)(msg & 0xffffffffu);
                if (tid >= 0 && tid < N_PROD && seq >= 0 && seq < PER) {
                    seen[tid][seq]++;
                }
                drained++;
            } else {
                sched_yield();
            }
        }

        for (int i = 0; i < N_PROD; i++) pthread_join(threads[i], NULL);

        /* Drain any stragglers post-join. */
        uint64_t leftover;
        while (mailbox_try_recv(m, &leftover)) {
            int tid = (int)(leftover >> 32);
            int seq = (int)(leftover & 0xffffffffu);
            if (tid >= 0 && tid < N_PROD && seq >= 0 && seq < PER) {
                seen[tid][seq]++;
            }
            drained++;
        }

        MBX_CHECK("MPSC drained 1024", drained == target);
        bool no_dup_no_miss = true;
        for (int t = 0; t < N_PROD; t++) {
            for (int s = 0; s < PER; s++) {
                if (seen[t][s] != 1) { no_dup_no_miss = false; break; }
            }
            if (!no_dup_no_miss) break;
        }
        MBX_CHECK("MPSC no duplicates / no misses", no_dup_no_miss);
        MBX_CHECK("MPSC sent_count=target",
                  mailbox_sent_count(m) == (size_t)target);
        MBX_CHECK("MPSC recv_count=target",
                  mailbox_recv_count(m) == (size_t)target);
        mailbox_destroy(m);
    }

    if (failures == 0) {
        printf("=== mailbox tests: ALL PASS ===\n\n");
    } else {
        printf("=== mailbox tests: %d FAILURE(S) ===\n\n", failures);
    }
    return failures;
}
