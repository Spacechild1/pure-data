/* Copyright (c) 2021 Christof Ressi.
 * For information on usage and redistribution, and for a DISCLAIMER OF ALL
 * WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

/* thread synchronisation tools */

#ifndef S_SYNC_H
#define S_SYNC_H

/* for atomics */
#include "s_spinlock.h"

#ifdef _WIN32
/* use Win32 Semaphores */
#elif defined(__APPLE__)
/* macOS doesn't support unnamed Posix semaphores,
 * so we use Mach semaphores instead. */
# include <mach/mach.h>
#elif defined(__linux__) || defined(__FreeBSD__) \
    || defined(__NetBSD__) || defined(__OpenBSD__)
/* Linux or BSD: use Posix semaphores */
# include <semaphore.h>
# include <errno.h>
#else
# error "Platform not supported!"
#endif

/* -------------------- t_lockfree_stack ---------------------- */

/* nodes must have t_lfs_node as its first member */

typedef struct _lfs_node
{
    struct _lfs_node *x_next;
} t_lfs_node;

#define lfs_node_init(x) ((t_lfs_node *)(x))->x_next = 0
#define lfs_node_next(x) ((void *)((t_lfs_node *)(x))->x_next)

typedef struct _lockfree_stack
{
#ifdef MSVC_INTERLOCKED
    t_lfs_node *x_head;
#else
    t_lfs_node * _Atomic x_head;
#endif
} t_lockfree_stack;

void lockfree_stack_init(t_lockfree_stack *x);
void lockfree_stack_push(t_lockfree_stack *x, void *node);
void * lockfree_stack_pop(t_lockfree_stack *x);
void * lockfree_stack_release(t_lockfree_stack *x);

/* ------------------- t_native_semaphore -------------------- */

typedef struct _native_semaphore
{
#if defined(_WIN32)
    void *sem;
#elif defined(__APPLE__)
    semaphore_t sem;
#else /* posix */
    sem_t sem;
#endif
} t_native_semaphore;

int native_semaphore_init(t_native_semaphore *x);
int native_semaphore_destroy(t_native_semaphore *x);
int native_semaphore_post(t_native_semaphore *x);
int native_semaphore_postn(t_native_semaphore *x, int count);
int native_semaphore_wait(t_native_semaphore *x);

/* ------------------- t_fast_semaphore ------------------ */

/* thanks to https://preshing.com/20150316/semaphores-are-surprisingly-versatile */

typedef struct _fast_semaphore
{
    t_native_semaphore sem;
#ifdef MSVC_INTERLOCKED
    long count;
#else
    atomic_int count;
#endif
} t_fast_semaphore;

int fast_semaphore_init(t_fast_semaphore *x);
int fast_semaphore_destroy(t_fast_semaphore *x);
int fast_semaphore_post(t_fast_semaphore *x);
int fast_semaphore_postn(t_fast_semaphore *x, int count);
int fast_semaphore_wait(t_fast_semaphore *x);
/* returns 1 on success, 0 on failure */
int fast_semaphore_trywait(t_fast_semaphore *x);

#endif /* S_SYNC_H */
