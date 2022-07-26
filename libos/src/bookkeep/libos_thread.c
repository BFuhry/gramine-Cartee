/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University
 * Copyright (C) 2020 Intel Corporation
 *                    Borys Popławski <borysp@invisiblethingslab.com>
 */

/*
 * This file contains code for maintaining bookkeeping of threads in library OS.
 */

#include "api.h"
#include "asan.h"
#include "assert.h"
#include "cpu.h"
#include "libos_checkpoint.h"
#include "libos_defs.h"
#include "libos_flags_conv.h"
#include "libos_handle.h"
#include "libos_internal.h"
#include "libos_lock.h"
#include "libos_process.h"
#include "libos_signal.h"
#include "libos_thread.h"
#include "libos_vma.h"
#include "list.h"
#include "pal.h"
#include "toml_utils.h"

/* TODO: consider changing this list to a tree. */
static LISTP_TYPE(libos_thread) g_thread_list = LISTP_INIT;
struct libos_lock g_thread_list_lock;

static struct libos_signal_dispositions* alloc_default_signal_dispositions(void) {
    struct libos_signal_dispositions* dispositions = malloc(sizeof(*dispositions));
    if (!dispositions) {
        return NULL;
    }

    if (!create_lock(&dispositions->lock)) {
        free(dispositions);
        return NULL;
    }
    refcount_set(&dispositions->ref_count, 1);
    for (size_t i = 0; i < ARRAY_SIZE(dispositions->actions); i++) {
        sigaction_make_defaults(&dispositions->actions[i]);
    }

    return dispositions;
}

static struct libos_thread* alloc_new_thread(void) {
    struct libos_thread* thread = calloc(1, sizeof(struct libos_thread));
    if (!thread) {
        return NULL;
    }

    if (!create_lock(&thread->lock)) {
        free(thread);
        return NULL;
    }

    int ret = create_pollable_event(&thread->pollable_event);
    if (ret < 0) {
        destroy_lock(&thread->lock);
        free(thread);
        return NULL;
    }

    refcount_set(&thread->ref_count, 1);
    INIT_LIST_HEAD(thread, list);
    /* default value as sigalt stack isn't specified yet */
    thread->signal_altstack.ss_flags = SS_DISABLE;
    return thread;
}

int alloc_thread_libos_stack(struct libos_thread* thread) {
    assert(thread->libos_stack_bottom == NULL);

    void* addr = NULL;
    int prot = PROT_READ | PROT_WRITE;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | VMA_INTERNAL;
    int ret = bkeep_mmap_any(LIBOS_THREAD_LIBOS_STACK_SIZE, prot, flags, /*file=*/NULL,
                             /*offset=*/0, "libos_stack", &addr);
    if (ret < 0) {
        return ret;
    }

    bool need_mem_free = false;
    ret = PalVirtualMemoryAlloc(addr, LIBOS_THREAD_LIBOS_STACK_SIZE,
                                LINUX_PROT_TO_PAL(prot, flags));
    if (ret < 0) {
        ret = pal_to_unix_errno(ret);
        goto unmap;
    }
    need_mem_free = true;

    /* Create a stack guard page. */
    ret = PalVirtualMemoryProtect(addr, PAGE_SIZE, /*prot=*/0);
    if (ret < 0) {
        ret = pal_to_unix_errno(ret);
        goto unmap;
    }

    thread->libos_stack_bottom = (char*)addr + LIBOS_THREAD_LIBOS_STACK_SIZE;

    return 0;

unmap:;
    void* tmp_vma = NULL;
    if (bkeep_munmap(addr, LIBOS_THREAD_LIBOS_STACK_SIZE, /*is_internal=*/true, &tmp_vma) < 0) {
        log_error("[alloc_thread_libos_stack]"
                  " Failed to remove bookkeeped memory that was not allocated at %p-%p!",
                  addr, (char*)addr + LIBOS_THREAD_LIBOS_STACK_SIZE);
        BUG();
    }
    if (need_mem_free) {
        if (PalVirtualMemoryFree(addr, LIBOS_THREAD_LIBOS_STACK_SIZE) < 0) {
            BUG();
        }
    }
    bkeep_remove_tmp_vma(tmp_vma);
    return ret;
}

static int init_main_thread(void) {
    struct libos_thread* cur_thread = get_cur_thread();
    if (cur_thread) {
        /* Thread already initialized (e.g. received via checkpoint). */
        add_thread(cur_thread);
        assert(cur_thread->tid);
        return init_id_ranges(cur_thread->tid);
    }

    int ret = init_id_ranges(/*preload_tid=*/0);
    if (ret < 0) {
        return ret;
    }

    cur_thread = alloc_new_thread();
    if (!cur_thread) {
        return -ENOMEM;
    }

    cur_thread->tid = get_new_id(/*move_ownership_to=*/0);
    if (!cur_thread->tid) {
        log_error("Cannot allocate pid for the initial thread!");
        put_thread(cur_thread);
        return -ESRCH;
    }
    g_process.pid = cur_thread->tid;
    __atomic_store_n(&g_process.pgid, g_process.pid, __ATOMIC_RELEASE);

    int64_t uid_int64;
    ret = toml_int_in(g_manifest_root, "loader.uid", /*defaultval=*/0, &uid_int64);
    if (ret < 0) {
        log_error("Cannot parse 'loader.uid'");
        put_thread(cur_thread);
        return -EINVAL;
    }

    int64_t gid_int64;
    ret = toml_int_in(g_manifest_root, "loader.gid", /*defaultval=*/0, &gid_int64);
    if (ret < 0) {
        log_error("Cannot parse 'loader.gid'");
        put_thread(cur_thread);
        return -EINVAL;
    }

    if (uid_int64 < 0 || uid_int64 > IDTYPE_MAX) {
        log_error("'loader.uid' = %ld is negative or greater than %u", uid_int64, IDTYPE_MAX);
        put_thread(cur_thread);
        return -EINVAL;
    }

    if (gid_int64 < 0 || gid_int64 > IDTYPE_MAX) {
        log_error("'loader.gid' = %ld is negative or greater than %u", gid_int64, IDTYPE_MAX);
        put_thread(cur_thread);
        return -EINVAL;
    }

    cur_thread->uid = uid_int64;
    cur_thread->euid = uid_int64;
    cur_thread->gid = gid_int64;
    cur_thread->egid = gid_int64;

    cur_thread->signal_dispositions = alloc_default_signal_dispositions();
    if (!cur_thread->signal_dispositions) {
        put_thread(cur_thread);
        return -ENOMEM;
    }

    __sigset_t set;
    __sigemptyset(&set);
    lock(&cur_thread->lock);
    set_sig_mask(cur_thread, &set);
    unlock(&cur_thread->lock);

    ret = PalEventCreate(&cur_thread->scheduler_event, /*init_signaled=*/false,
                         /*auto_clear=*/true);
    if (ret < 0) {
        put_thread(cur_thread);
        return pal_to_unix_errno(ret);
    }

    /* TODO: I believe there is some PAL allocated initial stack which could be reused by the first
     * thread. Tracked in https://github.com/gramineproject/gramine/issues/84. */
    ret = alloc_thread_libos_stack(cur_thread);
    if (ret < 0) {
        put_thread(cur_thread);
        return ret;
    }

    cur_thread->pal_handle = g_pal_public_state->first_thread;

    cur_thread->cpu_affinity_mask = calloc(GET_CPU_MASK_LEN(),
                                           sizeof(*cur_thread->cpu_affinity_mask));
    if (!cur_thread->cpu_affinity_mask) {
        put_thread(cur_thread);
        return -ENOMEM;
    }
    ret = PalThreadGetCpuAffinity(cur_thread->pal_handle, cur_thread->cpu_affinity_mask,
                                  GET_CPU_MASK_LEN());
    if (ret < 0) {
        ret = pal_to_unix_errno(ret);
        log_error("Failed to get thread CPU affinity mask: %d", ret);
        put_thread(cur_thread);
        return ret;
    }

    set_cur_thread(cur_thread);
    add_thread(cur_thread);

    return 0;
}

int init_threading(void) {
    if (!create_lock(&g_thread_list_lock)) {
        return -ENOMEM;
    }

    return init_main_thread();
}

static struct libos_thread* __lookup_thread(IDTYPE tid) {
    assert(locked(&g_thread_list_lock));

    struct libos_thread* tmp;

    LISTP_FOR_EACH_ENTRY(tmp, &g_thread_list, list) {
        if (tmp->tid == tid) {
            get_thread(tmp);
            return tmp;
        }
    }

    return NULL;
}

struct libos_thread* lookup_thread(IDTYPE tid) {
    lock(&g_thread_list_lock);
    struct libos_thread* thread = __lookup_thread(tid);
    unlock(&g_thread_list_lock);
    return thread;
}

struct libos_thread* get_new_thread(void) {
    struct libos_thread* thread = alloc_new_thread();
    if (!thread) {
        return NULL;
    }

    struct libos_thread* cur_thread = get_cur_thread();
    size_t groups_size = cur_thread->groups_info.count * sizeof(cur_thread->groups_info.groups[0]);
    if (groups_size > 0) {
        thread->groups_info.groups = malloc(groups_size);
        if (!thread->groups_info.groups) {
            put_thread(thread);
            return NULL;
        }
        thread->groups_info.count = cur_thread->groups_info.count;
        memcpy(thread->groups_info.groups, cur_thread->groups_info.groups, groups_size);
    }

    thread->cpu_affinity_mask = malloc(GET_CPU_MASK_LEN() * sizeof(*thread->cpu_affinity_mask));
    if (!thread->cpu_affinity_mask) {
        put_thread(thread);
        return NULL;
    }

    lock(&cur_thread->lock);

    thread->uid       = cur_thread->uid;
    thread->gid       = cur_thread->gid;
    thread->euid      = cur_thread->euid;
    thread->egid      = cur_thread->egid;

    thread->stack     = cur_thread->stack;
    thread->stack_top = cur_thread->stack_top;
    thread->stack_red = cur_thread->stack_red;

    thread->signal_dispositions = cur_thread->signal_dispositions;
    get_signal_dispositions(thread->signal_dispositions);

    /* No need for this lock as we have just created `thread`, but `set_sig_mask` has an assert for
     * it. Also there is no problem with locking order as `thread` is not yet shared. */
    lock(&thread->lock);
    set_sig_mask(thread, &cur_thread->signal_mask);
    unlock(&thread->lock);
    thread->has_saved_sigmask = false;

    struct libos_handle_map* map = get_thread_handle_map(cur_thread);
    assert(map);
    set_handle_map(thread, map);

    memcpy(thread->cpu_affinity_mask, cur_thread->cpu_affinity_mask,
           GET_CPU_MASK_LEN() * sizeof(*thread->cpu_affinity_mask));

    unlock(&cur_thread->lock);

    int ret = PalEventCreate(&thread->scheduler_event, /*init_signaled=*/false,
                             /*auto_clear=*/true);
    if (ret < 0) {
        put_thread(thread);
        return NULL;
    }

    return thread;
}

struct libos_thread* get_new_internal_thread(void) {
    return alloc_new_thread();
}

void get_signal_dispositions(struct libos_signal_dispositions* dispositions) {
    refcount_inc(&dispositions->ref_count);
}

void put_signal_dispositions(struct libos_signal_dispositions* dispositions) {
    refcount_t ref_count = refcount_dec(&dispositions->ref_count);

    if (!ref_count) {
        destroy_lock(&dispositions->lock);
        free(dispositions);
    }
}

void get_thread(struct libos_thread* thread) {
    refcount_inc(&thread->ref_count);
}

void put_thread(struct libos_thread* thread) {
    refcount_t ref_count = refcount_dec(&thread->ref_count);

    if (!ref_count) {
        assert(LIST_EMPTY(thread, list));

        if (thread->libos_stack_bottom) {
            char* addr = (char*)thread->libos_stack_bottom - LIBOS_THREAD_LIBOS_STACK_SIZE;
#ifdef ASAN
            asan_unpoison_region((uintptr_t)addr, LIBOS_THREAD_LIBOS_STACK_SIZE);
#endif
            void* tmp_vma = NULL;
            if (bkeep_munmap(addr, LIBOS_THREAD_LIBOS_STACK_SIZE, /*is_internal=*/true,
                             &tmp_vma) < 0) {
                log_error("[put_thread] Failed to remove bookkeeped memory at %p-%p!",
                          addr, (char*)addr + LIBOS_THREAD_LIBOS_STACK_SIZE);
                BUG();
            }
            if (PalVirtualMemoryFree(addr, LIBOS_THREAD_LIBOS_STACK_SIZE) < 0) {
                BUG();
            }
            bkeep_remove_tmp_vma(tmp_vma);
        }

        free(thread->groups_info.groups);

        if (thread->pal_handle && thread->pal_handle != g_pal_public_state->first_thread)
            PalObjectClose(thread->pal_handle);

        if (thread->handle_map) {
            put_handle_map(thread->handle_map);
        }

        if (thread->signal_dispositions) {
            put_signal_dispositions(thread->signal_dispositions);
        }

        free_signal_queue(&thread->signal_queue);

        /* `signal_altstack` is provided by the user, no need for a clean up. */

        if (thread->robust_list) {
            release_robust_list(thread->robust_list);
        }

        if (thread->scheduler_event) {
            PalObjectClose(thread->scheduler_event);
        }

        /* `wake_queue` is only meaningful when `thread` is part of some wake up queue (is just
         * being woken up), which would imply `ref_count > 0`. */

        if (thread->tid && !is_internal(thread)) {
            release_id(thread->tid);
        }

        free(thread->cpu_affinity_mask);

        destroy_pollable_event(&thread->pollable_event);

        destroy_lock(&thread->lock);

        free(thread);
    }
}

void add_thread(struct libos_thread* thread) {
    assert(!is_internal(thread) && LIST_EMPTY(thread, list));

    struct libos_thread* tmp;
    struct libos_thread* prev = NULL;
    lock(&g_thread_list_lock);

    /* keep it sorted */
    LISTP_FOR_EACH_ENTRY_REVERSE(tmp, &g_thread_list, list) {
        if (tmp->tid < thread->tid) {
            prev = tmp;
            break;
        }
        assert(tmp->tid != thread->tid);
    }

    get_thread(thread);
    LISTP_ADD_AFTER(thread, prev, &g_thread_list, list);
    unlock(&g_thread_list_lock);
}

/*
 * Checks whether there are any other threads on `g_thread_list` (i.e. if we are the last thread).
 * If `mark_self_dead` is true additionally takes us off the `g_thread_list`.
 */
bool check_last_thread(bool mark_self_dead) {
    struct libos_thread* self = get_cur_thread();
    bool ret = true;

    lock(&g_thread_list_lock);

    struct libos_thread* thread;
    LISTP_FOR_EACH_ENTRY(thread, &g_thread_list, list) {
        if (thread != self) {
            ret = false;
            break;
        }
    }

    if (mark_self_dead) {
        LISTP_DEL_INIT(self, &g_thread_list, list);
    }

    unlock(&g_thread_list_lock);

    if (mark_self_dead) {
        put_thread(self);
    }

    return ret;
}

/* This function is called by async worker thread to wait on thread->clear_child_tid_pal to be
 * zeroed (PAL does it when thread finally exits). Since it is a callback to async worker thread,
 * this function must follow the `void (*callback) (IDTYPE caller, void* arg)` signature. */
void cleanup_thread(IDTYPE caller, void* arg) {
    __UNUSED(caller);

    struct libos_thread* thread = (struct libos_thread*)arg;
    assert(thread);

    /* wait on clear_child_tid_pal; this signals that PAL layer exited child thread */
    while (__atomic_load_n(&thread->clear_child_tid_pal, __ATOMIC_ACQUIRE) != 0)
        CPU_RELAX();

    if (thread->robust_list) {
        release_robust_list(thread->robust_list);
        thread->robust_list = NULL;
    }

    /* notify parent if any */
    release_clear_child_tid(thread->clear_child_tid);

    /* Put down our (possibly last) reference to this thread - we got the ownership from the caller.
     */
    put_thread(thread);
}

int walk_thread_list(int (*callback)(struct libos_thread*, void*), void* arg, bool one_shot) {
    struct libos_thread* tmp;
    struct libos_thread* n;
    bool success = false;
    int ret = -ESRCH;

    lock(&g_thread_list_lock);

    LISTP_FOR_EACH_ENTRY_SAFE(tmp, n, &g_thread_list, list) {
        ret = callback(tmp, arg);
        if (ret < 0 && ret != -ESRCH) {
            goto out;
        }
        if (ret > 0) {
            if (one_shot) {
                ret = 0;
                goto out;
            }
            success = true;
        }
    }

    ret = success ? 0 : -ESRCH;
out:
    unlock(&g_thread_list_lock);
    return ret;
}

BEGIN_CP_FUNC(signal_dispositions) {
    __UNUSED(size);
    assert(size == sizeof(struct libos_signal_dispositions));

    struct libos_signal_dispositions* dispositions = (struct libos_signal_dispositions*)obj;
    struct libos_signal_dispositions* new_dispositions = NULL;

    size_t off = GET_FROM_CP_MAP(obj);

    if (!off) {
        off = ADD_CP_OFFSET(sizeof(struct libos_signal_dispositions));
        ADD_TO_CP_MAP(obj, off);
        new_dispositions = (struct libos_signal_dispositions*)(base + off);

        lock(&dispositions->lock);

        *new_dispositions = *dispositions;
        clear_lock(&new_dispositions->lock);
        refcount_set(&new_dispositions->ref_count, 0);

        unlock(&dispositions->lock);

        ADD_CP_FUNC_ENTRY(off);
    } else {
        new_dispositions = (struct libos_signal_dispositions*)(base + off);
    }

    if (objp) {
        *objp = (void*)new_dispositions;
    }
}
END_CP_FUNC(signal_dispositions)

BEGIN_RS_FUNC(signal_dispositions) {
    __UNUSED(offset);
    __UNUSED(rebase);
    struct libos_signal_dispositions* dispositions = (void*)(base + GET_CP_FUNC_ENTRY());

    if (!create_lock(&dispositions->lock)) {
        return -ENOMEM;
    }
}
END_RS_FUNC(signal_dispositions)

BEGIN_CP_FUNC(thread) {
    __UNUSED(size);
    assert(size == sizeof(struct libos_thread));

    struct libos_thread* thread = (struct libos_thread*)obj;
    struct libos_thread* new_thread = NULL;

    size_t off = GET_FROM_CP_MAP(obj);

    if (!off) {
        off = ADD_CP_OFFSET(sizeof(struct libos_thread));
        ADD_TO_CP_MAP(obj, off);
        new_thread = (struct libos_thread*)(base + off);
        *new_thread = *thread;

        INIT_LIST_HEAD(new_thread, list);

        new_thread->libos_stack_bottom = NULL;

        if (thread->groups_info.count > 0) {
            size_t groups_size = thread->groups_info.count * sizeof(thread->groups_info.groups[0]);
            size_t toff = ADD_CP_OFFSET(groups_size);
            new_thread->groups_info.groups = (void*)(base + toff);
            memcpy(new_thread->groups_info.groups, thread->groups_info.groups, groups_size);
        }

        new_thread->pal_handle = NULL;

        size_t mask_off = ADD_CP_OFFSET(GET_CPU_MASK_LEN() * sizeof(*thread->cpu_affinity_mask));
        new_thread->cpu_affinity_mask = (void*)(base + mask_off);
        memcpy(new_thread->cpu_affinity_mask, thread->cpu_affinity_mask,
               GET_CPU_MASK_LEN() * sizeof(*thread->cpu_affinity_mask));

        memset(&new_thread->pollable_event, 0, sizeof(new_thread->pollable_event));

        new_thread->handle_map = NULL;
        memset(&new_thread->signal_queue, 0, sizeof(new_thread->signal_queue));
        new_thread->robust_list = NULL;
        refcount_set(&new_thread->ref_count, 0);

        DO_CP_MEMBER(signal_dispositions, thread, new_thread, signal_dispositions);

        DO_CP_MEMBER(handle_map, thread, new_thread, handle_map);

        ADD_CP_FUNC_ENTRY(off);

        if (thread->libos_tcb) {
            size_t toff = ADD_CP_OFFSET(sizeof(libos_tcb_t));
            new_thread->libos_tcb = (void*)(base + toff);
            struct libos_tcb* new_tcb = new_thread->libos_tcb;
            *new_tcb = *thread->libos_tcb;
            /* don't export stale pointers */
            new_tcb->self      = NULL;
            new_tcb->tp        = NULL;
            new_tcb->vma_cache = NULL;

            new_tcb->log_prefix[0] = '\0';

            size_t roff = ADD_CP_OFFSET(sizeof(*thread->libos_tcb->context.regs));
            new_thread->libos_tcb->context.regs = (void*)(base + roff);
            pal_context_copy(new_thread->libos_tcb->context.regs, thread->libos_tcb->context.regs);
        }
    } else {
        new_thread = (struct libos_thread*)(base + off);
    }

    if (objp)
        *objp = (void*)new_thread;
}
END_CP_FUNC(thread)

BEGIN_RS_FUNC(thread) {
    struct libos_thread* thread = (void*)(base + GET_CP_FUNC_ENTRY());
    __UNUSED(offset);

    CP_REBASE(thread->list);
    if (thread->groups_info.count) {
        CP_REBASE(thread->groups_info.groups);
    } else {
        assert(!thread->groups_info.groups);
    }
    CP_REBASE(thread->handle_map);
    CP_REBASE(thread->signal_dispositions);

    if (!create_lock(&thread->lock)) {
        return -ENOMEM;
    }

    int ret = create_pollable_event(&thread->pollable_event);
    if (ret < 0) {
        return ret;
    }

    ret = PalEventCreate(&thread->scheduler_event, /*init_signaled=*/false, /*auto_clear=*/true);
    if (ret < 0) {
        return pal_to_unix_errno(ret);
    }

    if (thread->handle_map) {
        get_handle_map(thread->handle_map);
    }

    if (thread->signal_dispositions) {
        get_signal_dispositions(thread->signal_dispositions);
    }

    if (thread->set_child_tid) {
        *thread->set_child_tid = thread->tid;
        thread->set_child_tid = NULL;
    }

    CP_REBASE(thread->cpu_affinity_mask);

    assert(!get_cur_thread());

    ret = alloc_thread_libos_stack(thread);
    if (ret < 0) {
        return ret;
    }

    CP_REBASE(thread->libos_tcb);
    CP_REBASE(thread->libos_tcb->context.regs);

    libos_tcb_t* tcb = libos_get_tcb();
    *tcb = *thread->libos_tcb;
    __libos_tcb_init(tcb);

    assert(tcb->context.regs);
    set_tls(tcb->context.tls);

    thread->pal_handle = g_pal_public_state->first_thread;

    set_cur_thread(thread);
    log_setprefix(thread->libos_tcb);
}
END_RS_FUNC(thread)
