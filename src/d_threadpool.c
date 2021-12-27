/* Copyright (c) 2021 Christof Ressi.
 * For information on usage and redistribution, and for a DISCLAIMER OF ALL
 * WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

#include "m_pd.h"
#include "s_stuff.h"
#include "m_imp.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#if PD_DSPTHREADS

#include "s_sync.h"

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
#if defined(_WIN32)
    typedef BOOL (WINAPI *LPFN_GLPI)(
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION, PDWORD);

    LPFN_GLPI glpi;
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION info;
    DWORD err, size = 0;
    int i, n, count = 0;

        /* available since Windows XP SP3 */
    glpi = (LPFN_GLPI) GetProcAddress(
        GetModuleHandleA("kernel32"), "GetLogicalProcessorInformation");
    if (!glpi)
    {
        fprintf(stderr, "GetLogicalProcessorInformation() not supported;\n"
            "fall back to thread_hardware_concurrency\n");
        return thread_hardware_concurrency();
    }
        /* call with size 0 to retrieve actual size;
         * ERROR_INSUFFICIENT_BUFFER is expected. */
    glpi(NULL, &size);
    if ((err = GetLastError()) != ERROR_INSUFFICIENT_BUFFER)
        goto fail;
    info = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)malloc(size);
    if (glpi(info, &size) == FALSE)
    {
        err = GetLastError();
        free(info);
        goto fail;
    }
    n = size / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
    for (i = 0; i < n; ++i)
    {
        if (info[i].Relationship == RelationProcessorCore)
            count++;
    }
    free(info);
    return count;
fail:
    fprintf(stderr, "GetLogicalProcessorInformation() failed (%d)\n", err);
    return 0;
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
#elif defined(__linux__)
    /* The file /proc/cpusinfo contains all logical CPUs where
     * each entry has a property "physical id" and "core id".
     * We filter entries where those properties are the same
     * (= SMT), so we end up with the number of physical CPUs. */
    #define MAXNUMCPUS 1024
    unsigned int cpus[MAXNUMCPUS];
        /* upper 2 bytes: physical ID, lower 2 bytes: core ID */
    unsigned int current = 0;
    FILE *fp;
    char *line = 0;
    size_t len;
    int num = 0, count = 0;
    fp = fopen("/proc/cpuinfo", "r");
    if (!fp)
    {
        fprintf(stderr, "could not open /proc/cpuinfo\n");
        return 0;
    }
    while ((getline(&line, &len, fp) >= 0) && (count < MAXNUMCPUS))
    {
        const char *colon;
        int i, value;
        if (len == 0)
            continue;
            /* "physical id" comes first */
        if (strstr(line, "physical id"))
        {
            if (!(colon = strchr(line + strlen("physical id"), ':')) ||
                (sscanf(colon + 1, "%d", &value) < 1))
            {
                count = 0;
                break;
            }
            current = ((unsigned int)value) << 16;
        }
            /* followed by "core id" */
        else if (strstr(line, "core id"))
        {
            if (!(colon = strchr(line + strlen("core id"), ':')) ||
                (sscanf(colon + 1, "%d", &value) < 1))
            {
                count = 0;
                break;
            }
            current |= (unsigned int)value;
                /* now check if this entry already exists */
            for (i = 0; i < count; ++i)
            {
                if (cpus[i] == current)
                    goto skip;
            }
            cpus[count++] = current;
        skip:
        #if 0
            fprintf(stderr, "CPU %d: physical id: %d, core id: %d\n",
                num, current >> 16, current & 0xffff);
        #endif
            num++;
        }
    }
    if (line)
        free(line);
    fclose(fp);
    if (count == 0)
        fprintf(stderr, "/proc/cpuinfo: unexpected format\n");
    return count;
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
        if (n > 0)
        {
            d_threadpool->tp_threads = (pthread_t *)getbytes(sizeof(pthread_t) * n);
            d_threadpool->tp_n = n;
            /* spawn new threads; thread index starts at 1 */
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

static void dspthread_dorun(int index)
{
#ifdef DEBUG_DSPTHREADS
    fprintf(stderr, "DSP thread %d: start\n", index);
#endif
    dspthread_setindex(index);

#ifdef MSVC_INTERLOCKED
    while (d_threadpool->tp_running)
#else
    while (atomic_load_explicit(&d_threadpool->tp_running, memory_order_relaxed))
#endif
    {
        /* run as many tasks as possible */
        t_dsptask *t;
        while ((t = dspthreadpool_pop()))
            dsptask_run(t, index);
        /* wait for more tasks (or quit) */
    #ifdef DEBUG_DSPTHREADS
        fprintf(stderr, "DSP thread %d: wait\n", index);
    #endif
        fast_semaphore_wait(&d_threadpool->tp_sem);
    #ifdef DEBUG_DSPTHREADS
        fprintf(stderr, "DSP thread %d: wake up\n", index);
    #endif
    }
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
#ifdef MSVC_INTERLOCKED
    long dq_remaining;
#else
    atomic_int dq_remaining;
#endif
    t_fast_semaphore dq_sem;
};

t_dsptaskqueue * dsptaskqueue_new(void)
{
    t_dsptaskqueue *x = (t_dsptaskqueue *)getbytes(sizeof(t_dsptaskqueue));
    x->dq_numtasks = 0;
    x->dq_remaining = 0;
    fast_semaphore_init(&x->dq_sem);
    return x;
}

    /* this is also called by dsptask_free(). we only free the queue
     * when the reference count drops *below* zero. */
void dsptaskqueue_release(t_dsptaskqueue *x)
{
    int oldcount = x->dq_numtasks--;
    if (oldcount < 0)
        bug("dsptaskqueue_free");
    else if (oldcount == 0)
    {
        fast_semaphore_destroy(&x->dq_sem);
        freebytes(x, sizeof(t_dsptaskqueue));
    }
}

void dsptaskqueue_reset(t_dsptaskqueue *x)
{
    if (x->dq_numtasks > 0)
    {
        x->dq_remaining = x->dq_numtasks;
    #ifdef DEBUG_DSPTHREADS
        fprintf(stderr, "queue %p: reset with %d tasks\n",
            x, x->dq_numtasks);
    #endif
    }
}

void dsptaskqueue_join(t_dsptaskqueue *x)
{
    if (!d_threadpool || !d_threadpool->tp_n)
        /* single-threaded -> nothing to do, see dsptask_sched() */
        return;
    if (!x->dq_numtasks) /* no tasks */
        return;
    /* multi-threaded */
#ifdef DEBUG_DSPTHREADS
    fprintf(stderr, "queue %p: begin join\n", x);
#endif
    /* We don't want to put the thread to sleep, so we first try to
     * participate in DSP thread pool.
     * NB: if PDINSTANCE defined, we might actually run tasks that
     * belong to other Pd instances! LATER decide if we should push
     * such tasks back to the queue? */
    while (!fast_semaphore_trywait(&x->dq_sem))
    {
        /* Pop and run a *single* task, then try again.
         * Unlike in dspthread_dorun(), we do not pop tasks in a loop
         * because we might end up running tasks that don't belong to
         * this queue (and have a much later deadline). */
        t_dsptask *t = dspthreadpool_pop();
        if (t)
            dsptask_run(t, 0);
        else
        {
            /* nothing to do, wait */
        #ifdef DEBUG_DSPTHREADS
            fprintf(stderr, "queue %p: wait\n", x);
        #endif
            fast_semaphore_wait(&x->dq_sem);
            break; /* ! */
        }
    }
#ifdef DEBUG_DSPTHREADS
    fprintf(stderr, "queue %p: end join\n", x);
#endif
}

/* ---------------------------- t_dsptask ----------------------------- */

struct _dsptask
{
    t_lfs_node dt_node;
#ifdef PDINSTANCE
    t_pdinstance *dt_pdinstance;
#endif
    t_dsptaskqueue *dt_queue;
    t_dsptaskfn dt_fn;
    void *dt_data;
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
    queue->dq_numtasks++; /* increment refcount */
    return x;
}

void dsptask_free(t_dsptask *x)
{
    dsptaskqueue_release(x->dt_queue); /* release */
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
        fast_semaphore_post(&d_threadpool->tp_sem);
    }
    else
    {
        /* execute immediately, see dsptaskqueue_join().
         * NOTE: don't use dsptask_run() here! */
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
    (x->dt_fn)(x->dt_data);
#ifdef MSVC_INTERLOCKED
    remaining = _InterlockedDecrement(&queue->dq_remaining); /* returns new value! */
#else
    remaining = atomic_fetch_sub(&queue->dq_remaining, 1) - 1;
#endif
#ifdef DEBUG_DSPTHREADS
    fprintf(stderr, "queue %p: %d remaining tasks\n", queue, remaining);
#endif
    if (!remaining)
    {
        /* last task, notify waiting main audio thread;
         * see dsptaskqueue_join() */
        fast_semaphore_post(&queue->dq_sem);
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
