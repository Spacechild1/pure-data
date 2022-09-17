/* Copyright (c) 2021 Christof Ressi.
 * For information on usage and redistribution, and for a DISCLAIMER OF ALL
 * WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

#if PD_DSPTHREADS

#if !PD_PARALLEL
# error PD_DSPTHREADS requires PD_PARALLEL!
#endif

/* This one must be defined before including any headers! */
#ifdef __linux__
# ifndef _GNU_SOURCE
#  define _GNU_SOURCE
# endif
#endif

#include "m_pd.h"
#include "m_imp.h"
#include "s_stuff.h"
#include "s_sync.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <pthread.h>

#if defined(_WIN32)
# include <windows.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
# include <sys/sysctl.h>
# include <errno.h>
#else /* Linux */
# include <unistd.h>
# include <sys/sysinfo.h>
#endif

/* define for debugging DSP tasks and task queues */
// #define DEBUG_DSPTHREADS

/* ----------------------- thread utilities -------------------------- */

typedef struct _cpuinfo
{
    int physical_id;
    int core_id;
    int sibling_id;
    int id;
} t_cpuinfo;

    /* convert t_cpuinfo to an uint64_t for comparison.
     * We try to keep siblings as far apart as possible,
     * followed by physical packages, so that cores of
     * the same package are close to each other. */
static inline uint64_t cpuinfo2number(t_cpuinfo *x)
{
    return ((uint64_t)(x)->sibling_id << 24) |
        ((uint64_t)(x)->physical_id << 16) | ((uint64_t)(x)->core_id);
}

static t_cpuinfo *cpuvec = NULL;
static int numcpus = 0;
static int numcores = 0;
static int numpackages = 0;

static void cpuinfo_print(void)
{
    int i;
    fprintf(stderr, "hardware topology:\n");
    fprintf(stderr, "\tlogical processors: %d\n", numcpus);
    fprintf(stderr, "\tCPU cores: %d\n", numcores);
    fprintf(stderr, "\tphysical packages: %d\n", numpackages);
    fprintf(stderr, "\t---\n");
    for (i = 0; i < numcpus; i++)
    {
        fprintf(stderr, "\t#%d package: %d, core: %d, sibling: %d\n",
            i, cpuvec[i].physical_id, cpuvec[i].core_id, cpuvec[i].sibling_id);
    }
    fflush(stderr);
}

    /* sort the list so that we can simply pick
     * consecutive CPUs for effective thread pinning. */
static int cpuinfo_sort(const void *x, const void *y)
{
    uint64_t a = cpuinfo2number((t_cpuinfo *)x);
    uint64_t b = cpuinfo2number((t_cpuinfo *)y);
    return (a > b) ? 1 : (a < b) ? -1 : 0;
}

static void cpuinfo_done(void)
{
    if (sys_verbose)
        cpuinfo_print(); /* print original list */
        /* sort the list */
    qsort(cpuvec, numcpus, sizeof(t_cpuinfo), cpuinfo_sort);
#if 0
    cpuinfo_print(); /* print sorted list (for debugging) */
#endif
}

    /* 1: success, 0: failure */
static int parse_hardware_topology(void)
{
    /* Make sure to call this only once. This is not really thread-safe,
     * but in practice the function is called for the first time either in
     * threadpool_init() or via sys_argparse() -> sys_set_audio_settings().
     * LATER replace with C11 call_once(). */
    static int initted = 0;
    if (initted)
        return (numcpus > 0);
    initted = 1;

#ifdef _WIN32 /* Windows */
    typedef BOOL (WINAPI *t_func)(
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION, PDWORD);

    t_func fn;
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION info;
    DWORD err, size = 0;
    int i, n;

        /* available since Windows XP SP3 */
    fn = (t_func)GetProcAddress(
        GetModuleHandleA("kernel32"), "GetLogicalProcessorInformation");
    if (!fn)
    {
        fprintf(stderr, "GetLogicalProcessorInformation() not supported\n");
        return 0;
    }
        /* call with size 0 to retrieve actual size;
         * ERROR_INSUFFICIENT_BUFFER is expected. */
    fn(NULL, &size);
    if ((err = GetLastError()) != ERROR_INSUFFICIENT_BUFFER)
        goto fail;
    info = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)malloc(size);
    if (fn(info, &size) == FALSE)
    {
        err = GetLastError();
        free(info);
        goto fail;
    }
    n = size / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
    for (i = 0; i < n; ++i)
    {
        if (info[i].Relationship == RelationProcessorCore)
        {
                /* add all siblings to CPU list */
            int j, nsiblings = 0;
            ULONG_PTR mask = info[i].ProcessorMask;
            for (j = 0; mask; j++, mask >>= 1)
            {
                if (mask & 1)
                {
                    t_cpuinfo info = { 0, numcores, nsiblings, j };
                    int index = numcpus++;
                    cpuvec = realloc(cpuvec, sizeof(t_cpuinfo) * numcpus);
                    cpuvec[index] = info;
                    nsiblings++;
                }
            }
            numcores++;
        }
    }
        /* loop again for physical packages */
    for (i = 0; i < n; ++i)
    {
        if (info[i].Relationship == RelationProcessorPackage)
        {
            int j, k;
            ULONG_PTR mask = info[i].ProcessorMask;
                /* loop over all processors and find corresponding t_cpuinfo */
            for (j = 0; mask; j++, mask >>= 1)
            {
                if (mask & 1)
                {
                    for (k = 0; k < numcpus; ++k)
                    {
                        if (cpuvec[k].id == j)
                            cpuvec[k].physical_id = numpackages;
                    }
                }
            }
            numpackages++;
        }
    }
    free(info);
    cpuinfo_done();
    return 1;
fail:
    fprintf(stderr, "GetLogicalProcessorInformation() failed (%d)\n", err);
    return 0;
#elif defined(__linux__) /* Linux */
    /* The file /proc/cpusinfo contains all logical CPUs where
     * each entry has a property "physical id" and "core id". */
    t_cpuinfo cpu;
    FILE *fp;
    char *line = 0;
    size_t len;
    if (!(fp = fopen("/proc/cpuinfo", "r")))
    {
        fprintf(stderr, "could not open /proc/cpuinfo\n");
        return 0;
    }
    cpu.physical_id = cpu.core_id = -1;
    while (getline(&line, &len, fp) >= 0)
    {
        const char *pos, *colon;
        if (len == 0)
            continue;

            /* search for "physical id" and "core id" */
        if ((pos = strstr(line, "physical id")))
        {
            if (!(colon = strchr(pos, ':')) ||
                (sscanf(colon + 1, "%d", &cpu.physical_id) < 1))
                    goto fail;
        }
        else if ((pos = strstr(line, "core id")))
        {
            if (!(colon = strchr(pos, ':')) ||
                (sscanf(colon + 1, "%d", &cpu.core_id) < 1))
                    goto fail;
        }
            /* found both */
        if (cpu.physical_id >= 0 && cpu.core_id >= 0)
        {
            int i, found, index;
            cpu.sibling_id = 0;
            cpu.id = numcpus;
                /* get sibling number */
            for (i = 0; i < numcpus; ++i)
            {
                if ((cpuvec[i].physical_id == cpu.physical_id) &&
                    (cpuvec[i].core_id == cpu.core_id))
                {
                    cpu.sibling_id++;
                }
            }
            if (cpu.sibling_id == 0)
                numcores++;
                /* check for new physical package */
            found = 0;
            for (i = 0; i < numcpus; ++i)
            {
                if (cpuvec[i].physical_id == cpu.physical_id)
                    found = 1;
            }
            if (!found)
                numpackages++;
            index = numcpus++;
            cpuvec = realloc(cpuvec, sizeof(t_cpuinfo) * numcpus);
            cpuvec[index] = cpu;

            cpu.physical_id = cpu.core_id = -1; /* reset for next CPU */
        }
    }
    if (line)
        free(line);
    fclose(fp);
    cpuinfo_done();
    return 1;
fail:
    fprintf(stderr, "/proc/cpuinfo: unexpected format\n");
    fclose(fp);
    if (line)
        free(line);
    if (cpuvec)
        free(cpuvec);
    cpuvec = NULL;
    numcpus = 0;
    numcores = 0;
    numpackages = 0;
    return 0;
#else /* Apple, BSDs, etc. */
    fprintf(stderr, "parsse_hardware_topology() not implemented\n");
    return 0;
#endif
}

    /* 0: failure */
static int thread_hardware_concurrency(void)
{
#if defined(_WIN32)
    SYSTEM_INFO info;
    memset(&info, 0, sizeof(info));
    GetSystemInfo(&info);
    return info.dwNumberOfProcessors;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
    int count;
    size_t size = sizeof(count);
    if (sysctlbyname("hw.ncpu", &count, &size, NULL, 0) == 0)
        return count;
    else
    {
        fprintf(stderr, "sysctlbyname() failed (%d)\n", errno);
        return 0;
    }
#elif defined(__SC_NPROCESSORS_ONLN)
    int count = sysconf(_SC_NPROCESSORS_ONLN);
    if (count > 0)
        return count;
    else
    {
        fprintf(stderr, "sysconf() failed (%d)\n", errno);
        return 0;
    }
#elif defined(__linux__)
    return get_nprocs();
#else
    #warning "thread_hardware_concurrency() not implemented"
    return 0;
#endif
}

    /* 0: failure */
static int thread_physical_concurrency(void)
{
#if defined(_WIN32) || defined(__linux__)
    parse_hardware_topology(); /* see comment */
    return numcores;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
    int count;
    size_t size = sizeof(count);
    if (sysctlbyname("hw.physicalcpu", &count, &size, NULL, 0) == 0)
        return count;
    else
    {
        fprintf(stderr, "sysctlbyname() failed (%d)\n", errno);
        return 0;
    }
#else
    #warning "thread_physical_concurrency() not implemented"
        /* fall back to hardware concurrency */
    return thread_hardware_concurrency();
#endif
}

    /* 1: success, 0: failure */
static int thread_set_realtime(void)
{
#if defined(_WIN32)
    /* Force high thread priority in case we're not a high priority process.
     * This might be necessary for libpd when using the internal thread pool. */
    int pc = GetPriorityClass(GetCurrentProcess());
    if (!pc)
    {
        fprintf(stderr, "GetPriorityClass() failed (%d)\n", GetLastError());
        return 0;
    }
    if (pc < HIGH_PRIORITY_CLASS)
    {
        if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL))
        {
            fprintf(stderr, "SetThreadPriority() failed (%d)\n", GetLastError());
            return 0;
        }
    }
    return 1;
#elif defined(__APPLE__)
    struct sched_param param;
    int policy = SCHED_RR;
    int err;
    param.sched_priority = 80; /* adjust 0 : 100 */

    err = pthread_setschedparam(pthread_self(), policy, &param);
    if (err)
    {
        fprintf(stderr, "pthread_setschedparam() failed (%d)\n", err);
        return 0;
    }
    return 1;
#else /* Linux + BSD, see sys_set_priority() */
    struct sched_param par;
    int p;
#ifdef USEAPI_JACK
    p = sched_get_priority_min(SCHED_FIFO) + 5;
#else
    p = sched_get_priority_max(SCHED_FIFO) - 7;
#endif
    par.sched_priority = p;
    if (sched_setscheduler(0, SCHED_FIFO, &par) < 0)
    {
        fprintf(stderr, "sched_setscheduler() failed (%d)\n", errno);
        return 0;
    }
    return 1;
#endif
}

/* -------------------------- helper functions -------------------------- */

static int sys_maxnumdspthreads(void)
{
        /* only obtain once per thread (value is fixed) */
    static PERTHREAD int count = -1;
    if (count < 0)
    {
        count = thread_hardware_concurrency();
        if (count <= 0)
        {
            fprintf(stderr, "thread_hardware_concurrency() failed; default to 1\n");
            count = 1;
        }
    }
    return count;
}

    /* also used in sys_get_audio_settings() */
int sys_defnumdspthreads(void)
{
        /* only obtain once per thread (value is fixed) */
    static PERTHREAD int count = -1;
    if (count < 0)
    {
    #if 1
            /* use number of physical cores because SMT with
             * all available CPUs can lead to worse performance */
        count = thread_physical_concurrency();
        if (count <= 0)
        {
            fprintf(stderr, "thread_physical_concurrency() failed, "
                "use all available CPUs.\n");
            count = sys_maxnumdspthreads();
        }
    #else
            /* use all available CPUs. */
        count = sys_maxnumdspthreads();
    #endif
    }
    return count;
}

static void dspthread_setrealtime(int index)
{
    if (thread_set_realtime())
    {
        if (sys_verbose)
            fprintf(stderr, "DSP thread %d: set realtime priority\n", index);
    }
    else
        fprintf(stderr, "DSP thread %d: couldn't set realtime priority\n", index);
}

/* -------------------------- t_dspthreadpool --------------------------- */

typedef struct _backoff
{
    int b_n;
} t_backoff;

#define BACKOFF_MINLOOPS 16
#define BACKOFF_MAXLOOPS 4096

void backoff_reset(t_backoff *x)
{
    x->b_n = BACKOFF_MINLOOPS;
}

void backoff_perform(t_backoff *x)
{
    int i, n = x->b_n;
    for (i = 0; i < n; i++)
        pause_cpu();
    x->b_n *= 2;
    if (x->b_n > BACKOFF_MAXLOOPS)
        x->b_n = BACKOFF_MAXLOOPS;
}

typedef struct _dspthreadpool
{
#ifdef MSVC_INTERLOCKED
    long tp_running;
#else
    atomic_int tp_running;
#endif
    int tp_n;
    pthread_t *tp_threads;
    t_lockfree_stack tp_tasks;
    t_fast_semaphore tp_sem;
#ifdef MSVC_INTERLOCKED
    long tp_remaining;
#else
    atomic_int tp_remaining;
#endif
} t_dspthreadpool;

static t_dspthreadpool *d_threadpool = NULL;

static void dspthread_dorun(int index);

static void * thread_function(void *x)
{
    int index = (int)(intptr_t)x;
    if (sys_hipriority != 0) /* -1 or 1 */
        dspthread_setrealtime(index);
    if (!d_threadpool)
    {
        bug("DSP thread pool not initialized!");
        return 0;
    }
    if (index == 0)
    {
        bug("thread index 0 reserved for main audio thread!");
        return 0;
    }
    else if (index < 0 || index > d_threadpool->tp_n)
    {
        bug("thread index %d out of range!", index);
        return 0;
    }

    dspthread_dorun(index);

    return NULL;
}

int sys_havedspthreadpool(void)
{
    return 1;
}

    /* called with global lock set! */
static void dspthreadpool_init(void)
{
    if (!d_threadpool)
    {
        d_threadpool = (t_dspthreadpool *)getbytes(sizeof(t_dspthreadpool));
        d_threadpool->tp_running = 0;
        d_threadpool->tp_n = 0;
        d_threadpool->tp_threads = 0;
        lockfree_stack_init(&d_threadpool->tp_tasks);
        fast_semaphore_init(&d_threadpool->tp_sem);
        d_threadpool->tp_remaining = 0;
            /* for thread pinning */
        if (sys_threadaffinity)
            parse_hardware_topology();
    }
}

void dspthreadpool_stop(int external)
{
    int n = d_threadpool->tp_n;
    if (!n) /* no threads or already stopped */
        return;
    if (sys_verbose)
        fprintf(stderr, "stop DSP thread pool\n");
#ifdef MSVC_INTERLOCKED
    _InterlockedExchange(&d_threadpool->tp_running, 0);
#else
    atomic_store(&d_threadpool->tp_running, 0);
#endif
    /* wake up helper threads */
    fast_semaphore_postn(&d_threadpool->tp_sem, n);
    if (!external)
    {
        /* join helper threads */
        for (int i = 1; i < n; ++i)
            pthread_join(d_threadpool->tp_threads[i], NULL);
    }
    if (d_threadpool->tp_threads)
        freebytes(d_threadpool->tp_threads, sizeof(pthread_t) * n);
    d_threadpool->tp_threads = 0;
    d_threadpool->tp_n = 0;
}

int sys_dspthreadpool_start(int *numthreads, int external)
{
    int n, maxnumthreads;
    pd_globallock(); /* global lock begin */
    dspthreadpool_init();
    dspthreadpool_stop(external);
        /* validate DSP thread count */
    if (!numthreads || *numthreads < 1)
        n = sys_defnumdspthreads();
    else
        n = *numthreads;
    maxnumthreads = sys_maxnumdspthreads();
    if (n > maxnumthreads)
        n = maxnumthreads;
    if (numthreads)
        *numthreads = n;

    if (sys_verbose)
        fprintf(stderr, "start DSP thread pool (using %d of %d CPUs)\n",
            n, maxnumthreads);

    n--; /* we already have 1 audio thread */

    d_threadpool->tp_running = 1;
    if (external) /* DSP threads are created and run by the user */
    {
        d_threadpool->tp_threads = NULL;
        d_threadpool->tp_n = n;
    }
    else /* use internal DSP threads */
    {
        if (n > 0) /* multi-threaded */
        {
            d_threadpool->tp_threads = (pthread_t *)getbytes(sizeof(pthread_t) * n);
            d_threadpool->tp_n = n;
            /* spawn new threads; index for DSP helper threads starts at 1 */
            for (int i = 0; i < n; ++i)
                pthread_create(&d_threadpool->tp_threads[i],
                    NULL, thread_function, (void *)(intptr_t)(i + 1));
        }
        else /* single threaded */
        {
            d_threadpool->tp_threads = 0;
            d_threadpool->tp_n = 0;
        }
    }
    pd_globalunlock(); /* global lock end */
    return 1;
}

int sys_dspthreadpool_stop(int external)
{
    pd_globallock();
    dspthreadpool_init();
    dspthreadpool_stop(external);
    pd_globalunlock();
    return 1;
}

void dspthreadpool_tick(int ntasks)
{
    if (ntasks > 0 && sys_threadspinwait && d_threadpool && d_threadpool->tp_n)
    {
        /* use atomic increment, so it also works with PDINSTANCE! */
    #ifdef MSVC_INTERLOCKED
        int prev = _InterlockedExchangeAdd(&d_threadpool->tp_remaining,
            ntasks);
    #else
        int prev = atomic_fetch_add(&d_threadpool->tp_remaining, ntasks);
    #endif
        /* only notify DSP helper threads if necessary */
        if (prev == 0)
            fast_semaphore_postn(&d_threadpool->tp_sem, d_threadpool->tp_n);
    #ifdef DEBUG_DSPTHREADS
        fprintf(stderr, "-- DSP thread pool: start tick with %d active tasks\n", ntasks);
    #endif
    #ifndef PDINSTANCE
        if (prev != 0)
            pd_error(0, "DSP thread pool: bad task count (%d)", prev);
    #endif
    }
}

static void dspthreadpool_push(t_dsptask *task)
{
    lockfree_stack_push(&d_threadpool->tp_tasks, task);
}

static t_dsptask * dspthreadpool_pop(void)
{
    return lockfree_stack_pop(&d_threadpool->tp_tasks);
}

static void dsptask_run(t_dsptask *x, int index);

void dspthread_setindex(int index);
void mayer_init(void);
void mayer_term(void);

static void dspthread_dorun(int index)
{
#ifdef DEBUG_DSPTHREADS
    fprintf(stderr, "DSP thread %d: start\n", index);
#endif
    dspthread_setindex(index);
    mayer_init(); /* init FFT */

#ifdef MSVC_INTERLOCKED
    while (d_threadpool->tp_running)
#else
    while (atomic_load_explicit(&d_threadpool->tp_running,
            memory_order_relaxed))
#endif
    {
            /* run as many tasks as possible */
        t_dsptask *t;
        if (sys_threadspinwait) /* spin */
        {
            int remaining;
            t_backoff backoff;
            backoff_reset(&backoff);
        tryagain:
            while ((t = dspthreadpool_pop()))
            {
                dsptask_run(t, index);
                backoff_reset(&backoff);
            }
        #ifdef MSVC_INTERLOCKED
            remaining = d_threadpool->tp_remaining);
        #else
            remaining = atomic_load_explicit(
                &d_threadpool->tp_remaining, memory_order_acquire);
        #endif
            if (remaining > 0)
            {
                backoff_perform(&backoff);
                goto tryagain;
            }
            /* wait for next tick (or quit) */
        }
        else /* wait */
        {
            while ((t = dspthreadpool_pop()))
                dsptask_run(t, index);
            /* wait for more tasks (or quit) */
        }
    #ifdef DEBUG_DSPTHREADS
        fprintf(stderr, "DSP thread %d: wait\n", index);
    #endif
        fast_semaphore_wait(&d_threadpool->tp_sem);
    #ifdef DEBUG_DSPTHREADS
        fprintf(stderr, "DSP thread %d: wake up\n", index);
    #endif
    }

    mayer_term(); /* term FFT */
#ifdef DEBUG_DSPTHREADS
    fprintf(stderr, "DSP thread %d: finish\n", index);
#endif
}

int sys_dspthread_run(int index)
{
    if (!d_threadpool)
    {
        fprintf(stderr, "sys_dspthread_run: DSP thread pool not initialized!\n");
        return 0;
    }
    if (index == 0)
    {
        fprintf(stderr, "sys_dspthread_run: thread index 0 reserved for main audio thread!\n");
        return 0;
    }
    else if (index < 0 || index > d_threadpool->tp_n)
    {
        fprintf(stderr, "sys_dspthread_run: thread index %d out of range!\n", index);
        return 0;
    }

    dspthread_dorun(index);

    return 1;
}

/* -------------------------- t_dsptaskqueue --------------------------- */

struct _dsptaskqueue
{
    int dq_numtasks; /* number of tasks, also doubles as reference count */
    int dq_numswitchoff; /* number of switched of tasks */
#ifdef MSVC_INTERLOCKED
    long dq_remaining;
#else
    atomic_int dq_remaining;
#endif
    t_fast_semaphore dq_sem; /* not needed for spinning */
    t_canvas *dq_owner; /* canvas or NULL */
    char dq_threadsafe;
    char dq_warned;
};

t_dsptaskqueue * dsptaskqueue_new(t_canvas *owner)
{
    t_dsptaskqueue *x = (t_dsptaskqueue *)getbytes(sizeof(t_dsptaskqueue));
    x->dq_numtasks = 0;
    x->dq_numswitchoff = 0;
    x->dq_remaining = 0;
    if (!sys_threadspinwait)
        fast_semaphore_init(&x->dq_sem);
    x->dq_owner = owner;
    x->dq_threadsafe = 0;
    x->dq_warned = 0;
    return x;
}

    /* this is also called by dsptask_free(). we only free the queue
     * when the reference count drops *below* zero. */
void dsptaskqueue_release(t_dsptaskqueue *x)
{
    int oldcount = x->dq_numtasks--;
    if (oldcount > 0)
    {
    #ifdef DEBUG_DSPTHREADS
        fprintf(stderr, "queue %p: %d tasks (%d switched off)\n",
            x, oldcount-1, x->dq_numswitchoff);
    #endif
    }
    else if (oldcount == 0) /* release queue */
    {
        if (x->dq_numswitchoff != 0)
            bug("dsptaskqueue_release: bad switch count (%d)",
                x->dq_numswitchoff);
    #ifdef DEBUG_DSPTHREADS
        fprintf(stderr, "queue %p: release\n");
    #endif
        if (!sys_threadspinwait)
            fast_semaphore_destroy(&x->dq_sem);
        freebytes(x, sizeof(t_dsptaskqueue));
    }
    else if (oldcount < 0)
        bug("dsptaskqueue_release: bad refcount (%d)", oldcount);
}

    /* check if our sub-tree is thread-safe and cache the result.
     * Called once per DSP graph update in ugen_start() and
     * ugen_done_graph(); see also canvas_markthreadsafe(). */
void dsptaskqueue_update(t_dsptaskqueue *x)
{
    x->dq_threadsafe = sys_threadsafe ?
        canvas_isthreadsafe(x->dq_owner, 0) : 1; /* silent! */
    x->dq_warned = 0;
}

    /* check if our sub-tree is thread-safe, using the cached result
     * of dsptaskqueue_update() above. Called by block~ objects
     * associated with this queue, see ugen_done_graph(). */
int dsptaskqueue_check(t_dsptaskqueue *x)
{
    if (x->dq_threadsafe)
        return 1;
    else
    {
    #if 1
        if (!x->dq_warned) /* only warn once per DSP task queue */
    #endif
        {
            if (canvas_isthreadsafe(x->dq_owner, 1)) /* loud */
                /* dq_threadsafe should have been true */
                bug("dsptaskqueue_check");
            x->dq_warned = 1;
        }
        return 0;
    }
}

void dsptaskqueue_reset(t_dsptaskqueue *x)
{
    int count = x->dq_numtasks - x->dq_numswitchoff;
    if (count > 0)
    {
        x->dq_remaining = count;
    #ifdef DEBUG_DSPTHREADS
        fprintf(stderr, "queue %p: reset with %d active tasks "
            "(%d total, %d switched off)\n",
            x, count, x->dq_numtasks, x->dq_numswitchoff);
    #endif
    }
    else if (count < 0)
        fprintf(stderr, "dsptaskqueue_reset: queue %p: bad task count (%d)\n",
            x, count);
}

static t_int *dsptaskqueue_doreset(t_int *w)
{
    t_dsptaskqueue *x = (t_dsptaskqueue *)w[1];
    dsptaskqueue_reset(x);
    return w + 2;
}

void dsp_add_reset(t_dsptaskqueue *x)
{
    dsp_add(dsptaskqueue_doreset, 1, x);
}

void dsptaskqueue_join(t_dsptaskqueue *x)
{
    int count = x->dq_numtasks - x->dq_numswitchoff;
    assert(count >= 0);
    if (!d_threadpool || !d_threadpool->tp_n || !count)
        /* single-threaded or no tasks, see also dsptask_sched() */
        return;
#ifdef DEBUG_DSPTHREADS
    fprintf(stderr, "queue %p: begin join\n", x);
#endif
    /* We don't want to put the thread to sleep, so we first try to
     * participate in DSP thread pool.
     * NB: if PDINSTANCE defined, we might actually run tasks that
     * belong to other Pd instances! LATER decide if we should push
     * such tasks back to the queue? */
    if (sys_threadspinwait) /* spin */
    {
        t_backoff backoff;
        backoff_reset(&backoff);
    #ifdef MSVC_INTERLOCKED
        while (x->dq_remaining)
    #else
        while (atomic_load_explicit(&x->dq_remaining,
                memory_order_relaxed))
    #endif
        {
            /* Pop and run a *single* task, then try again.
             * Unlike in dspthread_dorun(), we do not pop tasks in a loop
             * because we might end up running tasks that don't belong to
             * this queue (and have a much later deadline). */
            t_dsptask *t = dspthreadpool_pop();
            if (t)
            {
                dsptask_run(t, 0);
                backoff_reset(&backoff);
            }
            else
                backoff_perform(&backoff);
        }
        /* decrement global task counter.
        /* NB: we *could* simply decrement all tasks at once in dsp_tick(),
         * but then the DSP helper threads would always spin for the whole
         * duration of the tick. By doing it here we make sure that they
         * go to sleep as soon as all tasks have finished. */
    #ifdef MSVC_INTERLOCKED
        _InterlockedExchangeAdd(&d_threadpool->tp_remaining, -count);
    #else
        atomic_fetch_sub_explicit(&d_threadpool->tp_remaining, count,
            memory_order_release);
    #endif
    }
    else /* wait */
    {
        while (!fast_semaphore_trywait(&x->dq_sem))
        {
            /* Pop and run a *single* task, see explanation above. */
            t_dsptask *t = dspthreadpool_pop();
            if (t)
                dsptask_run(t, 0);
            else
            {
            #ifdef DEBUG_DSPTHREADS
                fprintf(stderr, "queue %p: wait\n", x);
            #endif
                fast_semaphore_wait(&x->dq_sem);
                break; /* ! */
            }
        }
    }
#ifdef DEBUG_DSPTHREADS
    fprintf(stderr, "queue %p: end join\n", x);
#endif
}

static t_int *dsptaskqueue_dojoin(t_int *w)
{
    t_dsptaskqueue *x = (t_dsptaskqueue *)w[1];
    dsptaskqueue_join(x);
    return w + 2;
}

void dsp_add_join(t_dsptaskqueue *x)
{
    dsp_add(dsptaskqueue_dojoin, 1, x);
}

/* ---------------------------- t_dsptask ----------------------------- */

void ugen_addtask(t_dsptask *x);
void ugen_removetask(t_dsptask *x, int on);
void ugen_switchtask(t_dsptask *x, int on);

struct _dsptask
{
    t_lfs_node dt_node;
#ifdef PDINSTANCE
    t_pdinstance *dt_pdinstance;
#endif
    t_dsptaskqueue *dt_queue;
    t_dsptaskfn dt_fn;
    void *dt_data;
    int dt_switchoff;
};

t_dsptask * dsptask_new(t_dsptaskqueue *queue, t_dsptaskfn fn, void *data)
{
    t_dsptask *x = (t_dsptask *)getbytes(sizeof(t_dsptask));
    lfs_node_init(x);
#ifdef PDINSTANCE
    x->dt_pdinstance = pd_this;
#endif
    x->dt_queue = queue;
    x->dt_fn = fn;
    x->dt_data = data;
    x->dt_switchoff = 0;
    queue->dq_numtasks++; /* increment refcount */
#ifdef DEBUG_DSPTHREADS
    fprintf(stderr, "queue %p: %d tasks (%d switched off)\n",
        queue, queue->dq_numtasks, queue->dq_numswitchoff);
#endif
    ugen_addtask(x);
    return x;
}

void dsptask_free(t_dsptask *x)
{
    /* make sure to decrement switch count! */
    if (x->dt_switchoff > 0)
    {
        if (--x->dt_queue->dq_numswitchoff < 0)
            bug("dsptask_free: bad queue switch count (%d)",
                x->dt_queue->dq_numswitchoff);
    }
    /* remove and free */
    ugen_removetask(x, x->dt_switchoff == 0);
    dsptaskqueue_release(x->dt_queue);
    freebytes(x, sizeof(t_dsptask));
}

void dsptask_sched(t_dsptask *x)
{
    if (d_threadpool && d_threadpool->tp_n > 0)
    {
    #ifdef DEBUG_DSPTHREADS
        fprintf(stderr, "queue %p: sched task %p\n", x->dt_queue, x);
    #endif
        dspthreadpool_push(x);
        if (!sys_threadspinwait)
            fast_semaphore_post(&d_threadpool->tp_sem);
    }
    else /* single-threaded */
    {
        /* execute immediately, see dsptaskqueue_join().
         * NB: don't use dsptask_run() here! */
        (x->dt_fn)(x->dt_data);
    }
}

static void dsptask_run(t_dsptask *x, int index)
{
    t_dsptaskqueue *queue = x->dt_queue;
    int remaining;
#ifdef DEBUG_DSPTHREADS
    fprintf(stderr, "queue %p: run task %p on thread %d\n",
        queue, x, index);
#endif
#ifdef PDINSTANCE
    pd_setinstance(x->dt_pdinstance);
#endif
    assert(x->dt_switchoff == 0);
    /* execute task */
    (x->dt_fn)(x->dt_data);
    /* atomically decrement task counter */
#ifdef MSVC_INTERLOCKED
    remaining = _InterlockedDecrement(&queue->dq_remaining); /* returns new value! */
#else
    remaining = atomic_fetch_sub(&queue->dq_remaining, 1) - 1;
#endif
#ifdef DEBUG_DSPTHREADS
    fprintf(stderr, "queue %p: %d remaining tasks\n", queue, remaining);
#endif
    if (!remaining) /* last task */
    {
        if (!sys_threadspinwait) /* wait */
        {
            /* last task, notify waiting main audio thread;
             * see dsptaskqueue_join() */
            fast_semaphore_post(&queue->dq_sem);
        }
    }
    else if (remaining < 0)
        fprintf(stderr, "dsptask_run: queue %p: bad remaining task count (%d)\n",
            queue, remaining);
}

/* This is called whenever an enclosing switch~ object has changed state.
 * Note that there can be several switch~ objects beyond this task;
 * as soon as one of them is switched off, the DSP task won't run and it
 * must notify the queue and DSP context to prevent them from locking up.
 * Conversely, *all* enclosing switch~ objects must be switched on for
 * the task to run (again), i.e. the counter must reach 0. */
void dsptask_switch(t_dsptask *x, int on)
{
    t_dsptaskqueue *queue = x->dt_queue;
    int state, oldstate = x->dt_switchoff > 0;
    if (on)
    {
        if (--x->dt_switchoff < 0)
            bug("dsptask_switch: bad switch count (%d)", x->dt_switchoff);
    }
    else
        x->dt_switchoff++;

    state = x->dt_switchoff > 0;
    if (oldstate != state)
    {
        /* only notify if the state has changed! */
    #ifdef DEBUG_DSPTHREADS
        fprintf(stderr, "queue %p: switch %s task %p \n",
            x->dt_queue, (on ? "on" : "off"), x);
    #endif
        if (on) /* off -> on */
        {
            if (--queue->dq_numswitchoff < 0)
                bug("dsptask_switch: bad queue switch count (%d)",
                    queue->dq_numswitchoff);
            ugen_switchtask(x, 1);
        }
        else /* on -> off */
        {
            if (++queue->dq_numswitchoff > queue->dq_numtasks)
                bug("dsptask_switch: queue switch count (%d) "
                    "exceeds queue task count (%d)",
                        queue->dq_numswitchoff, queue->dq_numtasks);
            ugen_switchtask(x, 0);
        }
    }
}

#else /* PD_DSPTHREADS */

/* dummy implementations of public API functions */

int sys_havedspthreadpool(void)
{
    return 0;
}

int sys_dspthreadpool_start(int *numthreads, int external)
{
    return 0;
}

int sys_dspthreadpool_stop(int external)
{
    return 0;
}

int sys_dspthread_run(int index)
{
    return 0;
}

#endif /* PD_DSPTHREADS */
