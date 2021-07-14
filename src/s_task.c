/* Copyright (c) 2021 Christof Ressi.
* For information on usage and redistribution, and for a DISCLAIMER OF ALL
* WARRANTIES, see the file, "LICENSE.txt," in this distribution. */

#include "m_pd.h"
#include "s_stuff.h"

/* If PD_WORKERTHREADS is 1, tasks are pushed to a queue. A worker thread pops
 * tasks from the queue and executes the 'task_workfn' function. On completion,
 * tasks are pushed to another queue that is regularly polled by the scheduler.
 * This is were the 'task_callback' function is called.
 *
 * If PD_WORKERTHREADS is 0, tasks are executed on the audio/scheduler thread.
 * This is intended to support single threaded systems. To maintain the same
 * semantics as the threaded implementation (the callback is guaranteed to run
 * after task_sched() has returned), we can't just execute tasks and its callbacks
 * synchronously; instead we have to put them on a queue where they are popped and
 * dispatched with the next scheduler tick. */

#ifndef PD_WORKERTHREADS
#define PD_WORKERTHREADS 1
#endif

#if PD_WORKERTHREADS

#ifdef _WIN32
#include <windows.h>
#endif

#include <pthread.h>

#endif /* PD_WORKERTHREADS */

/* The task API is a safe and user-friendly way to run tasks in the background
 * without interfering with the audio thread. This is intended for CPU intensive
 * otherwise non-real-time safe operations, such as reading data from a file.
 *
 * See objtask.c in doc/6.externs for a complete code example.
 *
 * ================================================================================
 * Documentation:
 * --------------------------------------------------------------------------------
 *
 * t_task *task_sched(t_pd *owner, void *data, task_workfn workfn, task_callback cb)
 *
 * description: schedule a new task.
 *
 * arguments:
 * 1) (t_pd *) owner: the Pd object that owns the given task.
 * 2) (void *) data: a user-specific dynamically allocated struct.
 *    To ensure thread-safety, do not store any pointers to mutable data.
 *    Ideally, you would not store any pointers at all, but rather treat 'data'
 *    as a message that is sent between the object and the task system.
 * 3) (task_workfn) workfn: a function to be called in the background. It takes input
 *    from 'data', performs some operation and stores the result back to 'data'.
 * 4) (task_callback) cb: a function to be called after the task has completed.
 *    This is where the output data can be safely moved to the owning object.
 *    If the task has been cancelled, 'owner' is NULL.
 *    Here you can also safely free the 'data' object!
 *
 * returns: a handle to the new task. IMPORTANT: do not try to use the task handle
 *    after the task has finished (i.e. after the callback)!
 *
 * NOTE: the order in which tasks are executed is undefined! Implementations might
 * use multiple worker threads which execute task concurrently.
 *
 * ---------------------------------------------------------------------------------
 *
 * void task_cancel(t_task *task, int sync)
 *
 * description: try to cancel a given task.
 *     Note that the task might be currently executing or it might have completed
 *     already. Either way, the task handle will be invalidated and the callback
 *     will be called with 'owner' set to NULL.
 *
 * arguments:
 * 1) (t_task *) task: the task that shall be cancelled.
 * 2) (int) sync: synchronize with worker thread(s).
 *     true (1): check if the task is currently being executed and wait for its completion.
 *     false (0): return immediately without blocking.
 *     NOTE: Normally, your task data should not contain any mutable shared state,
 *     in which case is no need for any synchronization.
 *
 * returns: always 0.
 *     In the future, this may return additional information.
 *
 * ----------------------------------------------------------------------------------
 *
 * void task_join(t_pd *owner);
 *
 * description: cancel all tasks associated with the given owner.
 *     This is typically called in the owner's destructor to make sure that no task
 *     is running after the object has been freed.
 *     It also synchronizes with the worker thread(s).
 *
 * ----------------------------------------------------------------------------------
 *
 * void sys_taskqueue_start(t_pdinstance *pd, int external)
 * void sys_taskqueue_stop(t_pdinstance *pd, int external)
 *
 * description: start/stop the task queue system
 *
 * arguments:
 * 1) (t_pdinstance *) pd: the Pd instance
 * 2) (int) external: use external worker threads (true/false)
 *     true (1): the client is responsible for managing the worker thread(s)
 *         and calling sys_taskqueue_perform().
 *     false (0): the worker thread(s) are managed by Pd.
 *
 * NOTE: if sys_taskqueue_start() is not called, the tasks are executed on the
 * audio/scheduler thread. This happens, for example, when Pd runs in batch mode.
 *
 * ----------------------------------------------------------------------------------
 *
 * int sys_taskqueue_perform(t_pdinstance *pd, int nonblocking)
 *
 * description: execute pending tasks.
 *     This function is called from the internal or external worker thread(s).
 *     It can be called from multiple threads at the same time!
 *
 * arguments:
 * 1) (t_pdinstance *) pd: the Pd instance
 * 2) (int) nonblocking: call in non-blocking mode (true/false)
 *     true (1): returns immediately when there is no more work available.
 *     false (0): blocks until the task queue is stopped with sys_taskqueue_stop().
 *
 * returns: 1 if it did something or 0 if there was nothing to do.
 *    In a polling loop the return value can be used to decide when to sleep.
 *
 * It is possible to implement several scheduling scenarios:
 * a) 1:1 - each Pd instance has a single worker thread that calls sys_taskqueue_perform().
 * b) 1:N - each Pd instance has N worker threads that simultaneously call
 *     sys_taskqueue_perform().
 * c) M:1 - a single worker thread periodically calls sys_taskqueue_perform() on M
 *     Pd instances (in non-blocking mode).
 * d) M:N - N worker threads periodically call sys_taskqueue_perform() on M
 *     Pd instances (in non-blocking mode).
 *
 */


struct _task
{
    struct _task *t_next;
    t_pd *t_owner;
    void *t_data;
    task_workfn t_workfn;
    task_callback t_cb;
    volatile int t_cancel; /* C89 doesn't have atomics */
};

typedef struct _tasklist
{
    t_task *tl_head;
    t_task *tl_tail;
#if PD_WORKERTHREADS
    pthread_mutex_t tl_mutex;
    pthread_cond_t tl_condition;
#endif
} t_tasklist;

static void tasklist_init(t_tasklist *list)
{
    list->tl_head = list->tl_tail = 0;
#if PD_WORKERTHREADS
    pthread_mutex_init(&list->tl_mutex, 0);
    pthread_cond_init(&list->tl_condition, 0);
#endif
}

static void tasklist_free(t_tasklist *list)
{
    t_task *task = list->tl_head;
    while (task)
    {
        t_task *next = task->t_next;
            /* the callback will free the data */
        task->t_cb(0, task->t_data);
        freebytes(task, sizeof(t_task));
        task = next;
    }
#if PD_WORKERTHREADS
    pthread_mutex_destroy(&list->tl_mutex);
    pthread_cond_destroy(&list->tl_condition);
#endif
}

static void tasklist_push(t_tasklist *list, t_task *task)
{
    if (list->tl_tail)
        list->tl_tail->t_next = task;
    else
        list->tl_head = list->tl_tail = task;
}

static t_task *tasklist_pop(t_tasklist *list)
{
    if (list->tl_head)
    {
        t_task *task = list->tl_head;
        list->tl_head = task->t_next;
        if (task == list->tl_tail)
            list->tl_tail = 0;
        return task;
    }
    else
        return 0;
}

struct _taskqueue
{
#if PD_WORKERTHREADS
    t_tasklist tq_fromsched;
    t_tasklist tq_tosched;
    t_task *tq_current;
    int tq_quit; /* C89 doesn't have atomics */
    pthread_t tq_thread;
    int tq_havethread;
#else
    t_tasklist tq_list;
#endif
    t_clock *tq_clock;
};

#if PD_WORKERTHREADS

static void *task_threadfn(void *x)
{
        /* set lower thread priority */
#ifdef _WIN32
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST))
        fprintf(stderr, "pd: couldn't set low priority for worker thread\n");
#else
    struct sched_param param;
    param.sched_priority = 0;
    if (pthread_setschedparam(pthread_self(), SCHED_OTHER, &param) != 0)
        fprintf(stderr, "pd: couldn't set low priority for worker thread\n");
#endif

    sys_taskqueue_perform((t_pdinstance *)x, 0);

    return 0;
}

int sys_taskqueue_perform(t_pdinstance *pd, int nonblocking)
{
    t_taskqueue *queue = pd->pd_stuff->st_taskqueue;
    t_tasklist *fromsched = &queue->tq_fromsched,
        *tosched = &queue->tq_tosched;
    int didsomething = 0;

#ifdef PDINSTANCE
    pd_setinstance(pd);
#endif

    pthread_mutex_lock(&fromsched->tl_mutex);
    while (!queue->tq_quit)
    {
            /* try to pop item from list */
        t_task *task = tasklist_pop(fromsched);
        if (task)
        {
            queue->tq_current = task;
            pthread_mutex_unlock(&fromsched->tl_mutex);

                /* execute task (without lock!) */
            if (!task->t_cancel)
                task->t_workfn(task->t_data);
            didsomething = 1;

                /* move task back to scheduler */
            pthread_mutex_lock(&tosched->tl_mutex);
            tasklist_push(tosched, task);
            queue->tq_current = 0;
            pthread_mutex_unlock(&tosched->tl_mutex);
            pthread_cond_signal(&tosched->tl_condition);

            pthread_mutex_lock(&fromsched->tl_mutex);
        }
        else
        {
            if (nonblocking)
                break;
            else /* wait for new task */
                pthread_cond_wait(&fromsched->tl_condition, &fromsched->tl_mutex);
        }
    }
    pthread_mutex_unlock(&fromsched->tl_mutex);

    return didsomething;
}

#else

int sys_taskqueue_perform(t_pdinstance *pd, int nonblocking)
{
    return 0;
}

#endif /* PD_WORKERTHREADS */

void sys_taskqueue_start(t_pdinstance *pd, int external)
{
#if PD_WORKERTHREADS
    t_taskqueue *queue = pd->pd_stuff->st_taskqueue;
    if (queue->tq_havethread)
    {
        bug("sys_taskqueue_start: already started");
        return;
    }
    if (!external)
    {   /* spawn internal worker thread */
        pthread_create(&queue->tq_thread, 0, task_threadfn, pd);
    }
    queue->tq_havethread = 1;
#endif
}

void sys_taskqueue_stop(t_pdinstance *pd, int external)
{
#if PD_WORKERTHREADS
    t_taskqueue *queue = pd->pd_stuff->st_taskqueue;
    t_tasklist *fromsched = &queue->tq_fromsched;
    if (!queue->tq_havethread)
    {
        bug("sys_taskqueue_stop: not started");
        return;
    }
        /* quit and notify worker thread(s) */
    pthread_mutex_lock(&fromsched->tl_mutex);
    queue->tq_quit = 1;
    pthread_mutex_unlock(&fromsched->tl_mutex);
    pthread_cond_broadcast(&fromsched->tl_condition);
    if (!external)
    {   /* join internal worker thread */
        pthread_join(queue->tq_thread, 0);
    }
    queue->tq_havethread = 0;
#endif
}

void taskqueue_dopoll(t_taskqueue *queue);

void s_task_newpdinstance(void)
{
    t_taskqueue *queue =
        STUFF->st_taskqueue = getbytes(sizeof(t_taskqueue));

#if PD_WORKERTHREADS
    tasklist_init(&queue->tq_tosched);
    tasklist_init(&queue->tq_fromsched);
    queue->tq_current = 0;
    queue->tq_havethread = 0;
    queue->tq_quit = 0;
#else
    tasklist_init(&queue->tq_list);
#endif
        /* the clock is used to defer the callback to the next
        clock timeout in case there is no worker thread. */
    queue->tq_clock = clock_new(queue, (t_method)taskqueue_dopoll);
}

void s_task_freepdinstance(void)
{
    t_taskqueue *queue = STUFF->st_taskqueue;

#if PD_WORKERTHREADS
    tasklist_free(&queue->tq_fromsched);
    tasklist_free(&queue->tq_tosched);
#else
    tasklist_free(&queue->tq_list);
#endif
    clock_free(queue->tq_clock);

    freebytes(queue, sizeof(t_taskqueue));
}

void taskqueue_dopoll(t_taskqueue *queue)
{
#if PD_WORKERTHREADS
    t_tasklist *tosched = &queue->tq_tosched;
    t_task *task;
    while (1)
    {
            /* try to pop task from list */
        pthread_mutex_lock(&tosched->tl_mutex);
        task = tasklist_pop(tosched);
        pthread_mutex_unlock(&tosched->tl_mutex);
        if (task)
        {
                /* run callback (without lock!)
                if the task has been cancelled, 'owner' is set to 0! */
            task->t_cb(task->t_cancel ? 0 : task->t_owner, task->t_data);
            freebytes(task, sizeof(t_task));
        }
        else
            break;
    }
#else
    t_tasklist *list = &queue->tq_list;
    while (1)
    {
            /* try to pop task from list */
        t_task *task = tasklist_pop(list);
        if (task)
        {
                /* run callback.
                if the task has been cancelled, 'owner' is set to 0! */
            task->t_cb(task->t_cancel ? 0 : task->t_owner, task->t_data);
            freebytes(task, sizeof(t_task));
        }
        else
            break;
    }
#endif
}

    /* called in sched_tick() */
void taskqueue_poll(void)
{
#if PD_WORKERTHREADS
    t_taskqueue *queue = STUFF->st_taskqueue;
    if (queue->tq_havethread)
        taskqueue_dopoll(queue);
#endif
}

t_task *task_sched(t_pd *owner, void *data,
    task_workfn workfn, task_callback cb)
{
    t_taskqueue *queue = STUFF->st_taskqueue;
#if PD_WORKERTHREADS
    t_tasklist *fromsched = &queue->tq_fromsched;
#else
    t_tasklist *list = &STUFF->st_taskqueue->tq_list;
#endif

    t_task *task = getbytes(sizeof(t_task));
    task->t_next = 0;
    task->t_owner = owner;
    task->t_data = data;
    task->t_workfn = workfn;
    task->t_cb = cb;
    task->t_cancel = 0;

#if PD_WORKERTHREADS
    if (queue->tq_havethread)
    {
            /* push task to list and notify worker thread */
        pthread_mutex_lock(&fromsched->tl_mutex);
        tasklist_push(fromsched, task);
        pthread_mutex_unlock(&fromsched->tl_mutex);
        pthread_cond_signal(&fromsched->tl_condition);
    }
    else
    {
            /* execute work function synchronously */
        task->t_workfn(task->t_data);
            /* push to list and defer to next clock timeout */
        tasklist_push(&queue->tq_tosched, task);
        clock_delay(queue->tq_clock, 0);
    }
#else
        /* execute work function synchronously */
    task->t_workfn(task->t_data);
        /* push to list and defer to next clock timeout */
    tasklist_push(list, task);
    clock_delay(queue->tq_clock, 0);
#endif

    return task;
}

int task_cancel(t_task *task, int sync)
{
    task->t_cancel = 1;
#if PD_WORKERTHREADS
    if (sync)
    {
        t_taskqueue *queue = STUFF->st_taskqueue;
        t_tasklist *fromsched = &queue->tq_fromsched,
             *tosched = &queue->tq_tosched;
            /* check if our task is currently being executed.
            first we lock the "fromsched" queue, so the worker thread
            can't pick a new task; then we lock the "tosched" queue,
            and wait until the current task is not our task (anymore) */
        pthread_mutex_lock(&fromsched->tl_mutex);
        pthread_mutex_lock(&tosched->tl_mutex);
        while (queue->tq_current == task)
        {
            pthread_cond_wait(&tosched->tl_condition, &tosched->tl_mutex);
        }
        pthread_mutex_unlock(&tosched->tl_mutex);
        pthread_mutex_unlock(&fromsched->tl_mutex);
    }
#endif
    return 0;
}

void task_join(t_pd *owner)
{
#if PD_WORKERTHREADS
    t_taskqueue *queue = STUFF->st_taskqueue;
    t_tasklist *fromsched = &queue->tq_fromsched,
         *tosched = &queue->tq_tosched;
    t_task *task;
        /* we lock both queues, so we can safely check
        check all tasks, including the current one. */
    pthread_mutex_lock(&fromsched->tl_mutex);
    pthread_mutex_lock(&tosched->tl_mutex);
        /* check tasks from the scheduler */
    task = fromsched->tl_head;
    while (task)
    {
        if (task->t_owner == owner)
            task->t_cancel = 1;
        task = task->t_next;
    }
        /* check tasks to the scheduler */
    task = tosched->tl_head;
    while (task)
    {
        if (task->t_owner == owner)
            task->t_cancel = 1;
        task = task->t_next;
    }
        /* check current task */
    if (queue->tq_current &&
        (queue->tq_current->t_owner == owner))
    {
        queue->tq_current->t_cancel = 1;
            /* wait for the task to finish */
        while (queue->tq_current)
        {
            pthread_cond_wait(&tosched->tl_condition,
                &tosched->tl_mutex);
        }
    }

    pthread_mutex_unlock(&tosched->tl_mutex);
    pthread_mutex_unlock(&fromsched->tl_mutex);
#else
    t_tasklist *list = &STUFF->st_taskqueue->tq_list;
    t_task *task = list->tl_head;
    while (task)
    {
        if (task->t_owner == owner)
            task->t_cancel = 1;
        task = task->t_next;
    }
#endif
}
