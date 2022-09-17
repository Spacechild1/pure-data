/* Copyright (c) 2021 Christof Ressi.
 * For information on usage and redistribution, and for a DISCLAIMER OF ALL
 * WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

/* header-only C/C++ spinlock library; can also be used by externals! */

#ifndef S_SPINLOCK_H
#define S_SPINLOCK_H

#include <stdint.h>
#include <assert.h>

#if defined(__cplusplus) && (__cplusplus >= 201103L)
/* C++11 atomics */
# include <atomic>
# define ALIGNAS(x) alignas(x)
using std::atomic_int;
using std::atomic_uint;
using std::atomic_load_explicit;
using std::atomic_exchange_explicit;
using std::atomic_fetch_add_explicit;
using std::atomic_fetch_sub_explicit;
using std::memory_order_acquire;
using std::memory_order_release;
using std::memory_order_acq_rel;
using std::memory_order_relaxed;
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L) \
    && !defined(__STDC_NO_ATOMICS__)
/* C11 atomics */
# include <stdatomic.h>
# include <stdalign.h>
# define ALIGNAS(x) _Alignas(x)
#elif defined(_MSC_VER)
/* fallback for MSVC (which doesn't yet provide <stdatomic.h> at the time of writing) */
# pragma message ("C11 atomics not supported, using fallback for MSVC.")
# include <intrin.h>
# define ALIGNAS(x) __declspec(align(x))
# define MSVC_INTERLOCKED
#else
# error "Missing support for C11/C++11 atomics."
#endif

#define CACHELINE_SIZE 64

/* t_spinlock */

typedef struct _spinlock
{
#ifdef MSVC_INTERLOCKED
    unsigned long state;
#else
    atomic_uint state;
#endif
} t_spinlock;

static inline void spinlock_init(t_spinlock *x);
static inline void spinlock_lock(t_spinlock *x);
static inline int spinlock_trylock(t_spinlock *x);
static inline void spinlock_unlock(t_spinlock *x);

/* t_padded_spinlock */

typedef struct _padded_spinlock
{
    ALIGNAS(CACHELINE_SIZE) t_spinlock lock;
    char padding[64 - sizeof(t_spinlock)];
} t_padded_spinlock;

#define padded_spinlock_init(x) spinlock_init(&((x)->lock))
#define padded_spinlock_lock(x) spinlock_lock(&((x)->lock))
#define padded_spinlock_trylock(x) spinlock_trylock(&((x)->lock))
#define padded_spinlock_unlock(x) spinlock_unlock(&((x)->lock))

/* t_rwspinlock */

typedef struct _rwspinlock
{
#ifdef MSVC_INTERLOCKED
    unsigned long state;
#else
    atomic_uint state;
#endif
} t_rwspinlock;

static inline void rwspinlock_init(t_rwspinlock *x);
/* writer */
static inline void rwspinlock_wrlock(t_rwspinlock *x);
static inline int rwspinlock_trywrlock(t_rwspinlock *x);
static inline void rwspinlock_wrunlock(t_rwspinlock *x);
/* reader */
static inline void rwspinlock_rdlock(t_rwspinlock *x);
static inline int rwspinlock_tryrdlock(t_rwspinlock *x);
static inline void rwspinlock_rdunlock(t_rwspinlock *x);

/* t_padded_rwspinlock */

typedef struct _padded_rwspinlock
{
    ALIGNAS(CACHELINE_SIZE) t_rwspinlock lock;
    char padding[64 - sizeof(t_rwspinlock)];
} t_padded_rwspinlock;

#define padded_rwspinlock_init(x) rwspinlock_init(&((x)->lock))
/* writer */
#define padded_rwspinlock_wrlock(x) rwspinlock_wrlock(&((x)->lock))
#define padded_rwspinlock_trywrlock(x) rwspinlock_trywrlock(&((x)->lock))
#define padded_rwspinlock_wrunlock(x) rwspinlock_wrunlock(&((x)->lock))
/* reader */
#define padded_rwspinlock_rdlock(x) rwspinlock_rdlock(&((x)->lock))
#define padded_rwspinlock_tryrdlock(x) rwspinlock_tryrdlock(&((x)->lock))
#define padded_rwspinlock_rdunlock(x) rwspinlock_rdunlock(&((x)->lock))


/* ------------------------ implementation --------------------------- */

#define CHECK_ALIGNMENT(x) assert((((uintptr_t)&x) & (sizeof(x)-1)) == 0)

/* Intel */
#if defined(__i386__) || defined(_M_IX86) || \
    defined(__x86_64__) || defined(_M_X64)
# define HAVE_PAUSE
# include <immintrin.h>
/* ARM */
#elif (defined(__ARM_ARCH_6K__) || \
       defined(__ARM_ARCH_6Z__) || \
       defined(__ARM_ARCH_6ZK__) || \
       defined(__ARM_ARCH_6T2__) || \
       defined(__ARM_ARCH_7__) || \
       defined(__ARM_ARCH_7A__) || \
       defined(__ARM_ARCH_7R__) || \
       defined(__ARM_ARCH_7M__) || \
       defined(__ARM_ARCH_7S__) || \
       defined(__ARM_ARCH_8A__) || \
       defined(__aarch64__))
/* the 'yield' instruction is supported from ARMv6k onwards */
# define HAVE_YIELD
#else
/* fallback */
# ifdef __cplusplus
#  include <thread>
# else
#  include <threads.h>
# endif
#endif

static inline void pause_cpu(void)
{
#if defined(HAVE_PAUSE)
    _mm_pause();
#elif defined(HAVE_YIELD)
    __asm__ __volatile__("yield");
#else /* fallback */
  #warning "architecture does not support yield/pause instruction"
# ifdef __cplusplus
    std::this_thread::yield();
# else
    thrd_yield();
# endif
#endif
}

/* -------------------- t_spinlock ---------------------- */

static inline void spinlock_init(t_spinlock *x)
{
    CHECK_ALIGNMENT(x->state);
    x->state = 0;
}

static inline int spinlock_trylock(t_spinlock *x)
{
#ifdef MSVC_INTERLOCKED
    return _InterlockedExchange(&x->state, 1) == 0;
#else
    return atomic_exchange_explicit(&x->state, 1, memory_order_acquire) == 0;
#endif
}

static inline void spinlock_lock(t_spinlock *x)
{
#ifdef MSVC_INTERLOCKED
    do {
        while (x->state != 0)
            pause_cpu();
    } while (_InterlockedExchange(&x->state, 1) != 0);
#else
    /* only try to modify the shared state if the lock seems to be available.
     * this should prevent unnecessary cache invalidation. */
    do {
        while (atomic_load_explicit(&x->state, memory_order_relaxed) != 0)
            pause_cpu();
    } while (atomic_exchange_explicit(&x->state, 1, memory_order_acquire) != 0);
#endif
}

static inline void spinlock_unlock(t_spinlock *x)
{
#ifdef MSVC_INTERLOCKED
    _InterlockedExchange(&x->state, 0);
#else
    atomic_store_explicit(&x->state, 0, memory_order_release);
#endif
}

/* -------------------------- t_rwspinlock -------------------------- */

#define RWSPINLOCK_UNLOCKED 0
#define RWSPINLOCK_LOCKED 0x80000000
/* use fetch-and-add version (optimized for readers) */
#define RWSPINLOCK_FETCH_AND_ADD 1

static inline void rwspinlock_init(t_rwspinlock *x)
{
    CHECK_ALIGNMENT(x->state);
    x->state = 0;
}

static inline int rwspinlock_trywrlock(t_rwspinlock *x)
{
#ifdef MSVC_INTERLOCKED
    return _InterlockedCompareExchange(&x->state, RWSPINLOCK_LOCKED, RWSPINLOCK_UNLOCKED) == RWSPINLOCK_UNLOCKED;
#else
    uint32_t expected = RWSPINLOCK_UNLOCKED;
    return atomic_compare_exchange_strong_explicit(&x->state, &expected, RWSPINLOCK_LOCKED,
        memory_order_acquire, memory_order_relaxed);
#endif
}

static inline void rwspinlock_wrlock(t_rwspinlock *x)
{
    /* only try to modify the shared state if the lock seems to be available.
     * this should prevent unnecessary cache invalidation. */
#ifdef MSVC_INTERLOCKED
    for (;;)
    {
        if (x->state == RWSPINLOCK_UNLOCKED)
        {
            /* check if state is UNLOCKED and set LOCKED bit on success. */
            if (_InterlockedCompareExchange(&x->state, RWSPINLOCK_LOCKED, RWSPINLOCK_UNLOCKED) == RWSPINLOCK_UNLOCKED)
                return;
            /* CAS failed -> retry immediately */
        } else
            pause_cpu();
    }
#else
    for (;;)
    {
        if (atomic_load_explicit(&x->state, memory_order_relaxed) == RWSPINLOCK_UNLOCKED)
        {
            /* check if state is UNLOCKED and set LOCKED bit on success. */
            uint32_t expected = RWSPINLOCK_UNLOCKED;
            if (atomic_compare_exchange_weak_explicit(&x->state, &expected, RWSPINLOCK_LOCKED,
                memory_order_acquire, memory_order_relaxed)) return;
            /* CAS failed -> retry immediately */
        } else
            pause_cpu();
    }
#endif
}

static inline void rwspinlock_wrunlock(t_rwspinlock *x)
{
#if RWSPINLOCK_FETCH_AND_ADD
    /* clear "locked" bit, see rwspinlock_tryrdlock() */
# ifdef MSVC_INTERLOCKED
    _InterlockedAnd(&x->state, ~RWSPINLOCK_LOCKED);
# else
    atomic_fetch_and_explicit(&x->state, ~RWSPINLOCK_LOCKED, memory_order_release);
# endif
#else /* CAS */
# ifdef MSVC_INTERLOCKED
    _InterlockedExchange(&x->state, RWSPINLOCK_UNLOCKED);
# else
    atomic_store_explicit(&x->state, RWSPINLOCK_UNLOCKED, memory_order_release);
# endif
#endif
}

static inline int rwspinlock_tryrdlock(t_rwspinlock *x)
{
#if RWSPINLOCK_FETCH_AND_ADD
    /* optimistically increment the reader count and then check if the "locked"
     * bit is set, otherwise we simply decrement the reader count again.
     * This is optimized for the likely case that there's no writer. */
# ifdef MSVC_INTERLOCKED
    unsigned long state = _InterlockedIncrement(&x->state);
    if ((state & RWSPINLOCK_LOCKED) == 0)
        return 1;
    else
    {
        _InterlockedDecrement(&x->state);
        return 0;
    }
# else
    uint32_t state = atomic_fetch_add_explicit(&x->state, 1, memory_order_acquire);
    if ((state & RWSPINLOCK_LOCKED) == 0)
        return 1;
    else
    {
        atomic_fetch_sub_explicit(&x->state, 1, memory_order_acq_rel);
        return 0;
    }
# endif
#else /* CAS */
    /* We need a loop because the CAS can fail if another *reader* aquires/releases
     * the lock concurrently. We shouldn't consider this a failure! */
# ifdef MSVC_INTERLOCKED
    for (;;)
    {
        unsigned long state = x->state;
        if ((state & RWSPINLOCK_LOCKED) == 0)
        {
            if (_InterlockedCompareExchange(&x->state, state + 1, state) == state)
                return 1;
            /* CAS failed -> retry */
        }
        else
            return 0;
    }
# else
    uint32_t state = atomic_load_explicit(&x->state, memory_order_relaxed);
    for (;;)
    {
        if ((state & RWSPINLOCK_LOCKED) == 0)
        {
            if (atomic_compare_exchange_weak_explicit(&x->state, &state, state + 1,
                memory_order_acquire, memory_order_relaxed)) return 1;
            /* CAS failed -> retry; 'state' has been updated */
        }
        else
            return 0;
    }
# endif
#endif
}

static inline void rwspinlock_rdlock(t_rwspinlock *x)
{
#if RWSPINLOCK_FETCH_AND_ADD
    /* only try to modify the shared state if the lock seems to be available.
     * this should prevent unnecessary cache invalidation. */
    for (;;)
    {
# ifdef MSVC_INTERLOCKED
        unsigned long state = x->state;
# else
        uint32_t state = atomic_load_explicit(&x->state, memory_order_relaxed);
# endif
        if (!(state & RWSPINLOCK_LOCKED) && rwspinlock_tryrdlock(x))
            return;
        else
            pause_cpu();
    }
#else /* CAS */
    /* with RWSPINLOCK_LOCKED masked away, the CAS will fail if the
     * spinlock is currently locked. NB: the CAS can also fail if
     * another reader acquired/releases the lock concurrently. */
# ifdef MSVC_INTERLOCKED
    for (;;)
    {
        unsigned long state = x->state & ~RWSPINLOCK_LOCKED;
        if (_InterlockedCompareExchange(&x->state, state + 1, state) == state)
            return;
        else
            pause_cpu();
    }
# else
    for (;;)
    {
        uint32_t state = atomic_load_explicit(&x->state, memory_order_relaxed);
        state &= ~RWSPINLOCK_LOCKED;
        if (atomic_compare_exchange_weak_explicit(&x->state, &state, state + 1,
                memory_order_acquire, memory_order_relaxed)) return;
        else /* NB: don't use updated 'state', instead read again after pause! */
            pause_cpu();
    }
# endif
#endif
}

static inline void rwspinlock_rdunlock(t_rwspinlock *x)
{
#ifdef MSVC_INTERLOCKED
    _InterlockedDecrement(&x->state);
#else
    atomic_fetch_sub_explicit(&x->state, 1, memory_order_release);
#endif
}

#undef CACHELINE_SIZE
#undef ALIGNAS
/* keep MSVC_INTERLOCKED and CHECK_ALIGNMENT */

#endif /* S_SPINLOCK_H */
