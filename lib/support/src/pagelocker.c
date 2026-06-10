/* Copyright (c) 2009-2013 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "support/pagelocker.h"
#include <assert.h>
#include <string.h>

#include <sys/mman.h>
#include <unistd.h>

size_t get_system_page_size(void)
{
    return (size_t)sysconf(_SC_PAGESIZE);
}

bool memory_page_lock(const void *addr, size_t len)
{
    return mlock(addr, len) == 0;
}

bool memory_page_unlock(const void *addr, size_t len)
{
    return munlock(addr, len) == 0;
}

void locked_page_manager_init(struct locked_page_manager *m)
{
    zcl_mutex_init(&m->mutex);
    m->num_entries = 0;
    m->page_size = get_system_page_size();
    assert((m->page_size & (m->page_size - 1)) == 0);
    m->page_mask = ~(m->page_size - 1);
}

void locked_page_manager_destroy(struct locked_page_manager *m)
{
    zcl_mutex_destroy(&m->mutex);
}

static int find_page(struct locked_page_manager *m, size_t page)
{
    for (size_t i = 0; i < m->num_entries; i++) {
        if (m->entries[i].page == page)
            return (int)i;
    }
    return -1;
}

void locked_page_manager_lock_range(struct locked_page_manager *m,
                                    void *p, size_t size)
{
    zcl_mutex_lock(&m->mutex);
    if (size == 0) {
        zcl_mutex_unlock(&m->mutex);
        return;
    }
    size_t base = (size_t)p;
    size_t start_page = base & m->page_mask;
    size_t end_page = (base + size - 1) & m->page_mask;
    for (size_t page = start_page; page <= end_page; page += m->page_size) {
        int idx = find_page(m, page);
        if (idx < 0) {
            memory_page_lock((void *)page, m->page_size);
            assert(m->num_entries < MAX_LOCKED_PAGES);
            m->entries[m->num_entries].page = page;
            m->entries[m->num_entries].count = 1;
            m->num_entries++;
        } else {
            m->entries[idx].count++;
        }
    }
    zcl_mutex_unlock(&m->mutex);
}

void locked_page_manager_unlock_range(struct locked_page_manager *m,
                                      void *p, size_t size)
{
    zcl_mutex_lock(&m->mutex);
    if (size == 0) {
        zcl_mutex_unlock(&m->mutex);
        return;
    }
    size_t base = (size_t)p;
    size_t start_page = base & m->page_mask;
    size_t end_page = (base + size - 1) & m->page_mask;
    for (size_t page = start_page; page <= end_page; page += m->page_size) {
        int idx = find_page(m, page);
        assert(idx >= 0);
        m->entries[idx].count--;
        if (m->entries[idx].count == 0) {
            memory_page_unlock((void *)page, m->page_size);
            m->entries[idx] = m->entries[m->num_entries - 1];
            m->num_entries--;
        }
    }
    zcl_mutex_unlock(&m->mutex);
}

int locked_page_manager_get_count(struct locked_page_manager *m)
{
    zcl_mutex_lock(&m->mutex);
    int count = (int)m->num_entries;
    zcl_mutex_unlock(&m->mutex);
    return count;
}

static struct locked_page_manager g_locked_page_manager;
static bool g_locked_page_manager_initialized = false;

struct locked_page_manager *locked_page_manager_instance(void)
{
    if (!g_locked_page_manager_initialized) {
        locked_page_manager_init(&g_locked_page_manager);
        g_locked_page_manager_initialized = true;
    }
    return &g_locked_page_manager;
}

void lock_object(void *obj, size_t size)
{
    locked_page_manager_lock_range(locked_page_manager_instance(), obj, size);
}

void unlock_object(void *obj, size_t size)
{
    memory_cleanse(obj, size);
    locked_page_manager_unlock_range(locked_page_manager_instance(), obj, size);
}
