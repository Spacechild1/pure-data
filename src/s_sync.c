/* Copyright (c) 2021 Christof Ressi.
 * For information on usage and redistribution, and for a DISCLAIMER OF ALL
 * WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

/* thread synchronisation tools. */

/* currently, this file is only needed for PD_DSPTHREADS */
#if PD_DSPTHREADS

#include "s_sync.h"

#ifdef _WIN32
# include <windows.h>
#endif

/* ----------------------- t_lockfree_stack ---------------------- */

void lockfree_stack_init(t_lockfree_stack *x)
{
    CHECK_ALIGNMENT(x->x_head);
    x->x_head = NULL;
}

void lockfree_stack_push(t_lockfree_stack *x, void *y)
{
    t_lfs_node *node = (t_lfs_node *)y;
#ifdef MSVC_INTERLOCKED
    do
    {
        node->x_next = x->x_head;
    }
    while (_InterlockedCompareExchangePointer(&x->x_head, node, node->x_next) != node->x_next);
#else
    node->x_next = atomic_load_explicit(&x->x_head, memory_order_relaxed);
    while (!atomic_compare_exchange_weak_explicit(&x->x_head, &node->x_next, node,
        memory_order_release, memory_order_relaxed)) ;
#endif
}

void * lockfree_stack_pop(t_lockfree_stack *x)
{
#ifdef MSVC_INTERLOCKED
    t_lfs_node *head;
    do
    {
        head = x->x_head;
    }
    while (head && _InterlockedCompareExchangePointer(&x->x_head, head->x_next, head) != head);
#else
    t_lfs_node *head = atomic_load_explicit(&x->x_head, memory_order_relaxed);
    while (head && !atomic_compare_exchange_weak_explicit(&x->x_head, &head,
        head->x_next, memory_order_acquire, memory_order_relaxed)) ;
#endif
    return head;
}

void * lockfree_stack_release(t_lockfree_stack *x)
{
#ifdef MSVC_INTERLOCKED
    return (void *)_InterlockedExchangePointer(&x->x_head, NULL);
#else
    return (void *)atomic_exchange(&x->x_head, NULL);
#endif
}

/* -------------------- t_native_semaphore -------------------- */

int native_semaphore_init(t_native_semaphore *x)
{
#if defined(_WIN32)
    return (x->sem = CreateSemaphoreA(0, 0, INT_MAX, 0)) ? 0 : -1;
#elif defined(__APPLE__)
    return (semaphore_create(mach_task_self(), &x->sem, SYNC_POLICY_FIFO, 0)
        == KERN_SUCCESS) ? 0 : -1;
#else /* posix */
    return sem_init(&x->sem, 0, 0);
#endif
}

int native_semaphore_destroy(t_native_semaphore *x)
{
#if defined(_WIN32)
    return CloseHandle(x->sem) ? 0 : -1;
#elif defined(__APPLE__)
    return (semaphore_destroy(mach_task_self(), x->sem) == KERN_SUCCESS) ? 0 : -1;
#else /* posix */
    return sem_destroy(&x->sem);
#endif
}

int native_semaphore_post(t_native_semaphore *x)
{
#if defined(_WIN32)
    return ReleaseSemaphore(x->sem, 1, 0) ? 0 : -1;
#elif defined(__APPLE__)
    return (semaphore_signal(x->sem) == KERN_SUCCESS) ? 0 : 1;
#else /* posix */
    return sem_post(&x->sem);
#endif
}

int native_semaphore_postn(t_native_semaphore *x, int count)
{
#if defined(_WIN32)
    return ReleaseSemaphore(x->sem, count, 0) ? 0 : -1;
#else
    for (int i = 0; i < count; ++i)
    {
        if (native_semaphore_post(x) < 0)
            return -1;
    }
    return 0;
#endif
}

int native_semaphore_wait(t_native_semaphore *x)
{
#if defined(_WIN32)
    return (WaitForSingleObject(x->sem, INFINITE) != WAIT_FAILED) ? 0 : -1;
#elif defined(__APPLE__)
    return (semaphore_wait(x->sem) == KERN_SUCCESS) ? 0 : -1;
#else /* posix */
    for (;;)
    {
        int ret = sem_wait(&x->sem);
        if (ret == 0)
            return 0;
        else if (errno == EINTR)
            continue;
        else
            return -1;
    }
#endif
}

/* t_fast_semaphore */

int fast_semaphore_init(t_fast_semaphore *x)
{
    CHECK_ALIGNMENT(x->count);
    x->count = 0;
    return native_semaphore_init(&x->sem);
}

int fast_semaphore_destroy(t_fast_semaphore *x){
    return native_semaphore_destroy(&x->sem);
}

int fast_semaphore_post(t_fast_semaphore *x)
{
#ifdef MSVC_INTERLOCKED
    int old = _InterlockedIncrement(&x->count) - 1; /* returns new value! */
#else
    int old = atomic_fetch_add_explicit(&x->count, 1, memory_order_release);
#endif
    if (old < 0)
        return native_semaphore_post(&x->sem);
    else
        return 0;
}

int fast_semaphore_postn(t_fast_semaphore *x, int count)
{
#ifdef MSVC_INTERLOCKED
    int old = _InterlockedExchangeAdd(&x->count, count); /* returns old value */
#else
    int old = atomic_fetch_add_explicit(&x->count, count, memory_order_release);
#endif
    if (old < 0)
    {
        int release = -old < count ? -old : count;
        return native_semaphore_postn(&x->sem, release);
    }
    else
        return 0;
}

int fast_semaphore_wait(t_fast_semaphore *x)
{
#ifdef MSVC_INTERLOCKED
    int old = _InterlockedDecrement(&x->count) + 1; /* returns new value! */
#else
    int old = atomic_fetch_sub_explicit(&x->count, 1, memory_order_acquire);
#endif
    if (old <= 0)
        return native_semaphore_wait(&x->sem);
    else
        return 0;
}

/* returns 1 on success, 0 on failure */
int fast_semaphore_trywait(t_fast_semaphore *x) {
#ifdef MSVC_INTERLOCKED
    int value = x->count;
#else
    int value = atomic_load_explicit(&x->count, memory_order_relaxed);
#endif
    /* NOTE: we need a loop because another thread might decrement the count
     * concurrently, which does not necessarily mean that we have failed! */
    while (value > 0)
    {
    #ifdef MSVC_INTERLOCKED
        if (_InterlockedCompareExchange(&x->count, value - 1, value) == value)
            return 1;
        /* CAS failed -> retry and update */
        value = x->count;
    #else
        if (atomic_compare_exchange_weak_explicit(&x->count, &value, value - 1,
                memory_order_acquire, memory_order_relaxed)) return 1;
        /* CAS failed -> retry; 'value' has been updated */
    #endif
    }
    return 0;
}

#endif /* PD_DSPTHREADS */
