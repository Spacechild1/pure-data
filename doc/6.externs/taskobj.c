/* This object shows how to run asynchronous tasks with the task API.
 * The "read" method reads the content of the given file in the background.
 * It outputs the file size on success and -1 on failure.
 * With the "get" method you can read a single byte. */

#include "m_pd.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static t_class *taskobj_class;

typedef struct _taskobj
{
    t_object x_obj;
    t_outlet *x_statusout;
    t_outlet *x_byteout;
    char *x_data;
    int x_size;
    t_task *x_current;
} t_taskobj;

typedef struct _taskdata
{
    char filename[256];
    char *data;
    int size;
    int err;
    float ms;
} t_taskdata;

static void taskobj_worker(void *z)
{
    t_taskdata *x = (t_taskdata *)z;
        /* free old file data */
    if (x->data)
        free(x->data);
    x->data = 0;
    x->size = 0;
        /* try to read new file data */
    FILE *fp = fopen(x->filename, "rb");
    if (fp)
    {
        char *data;
        size_t size;
        // get file size
        fseek(fp, 0, SEEK_END);
        size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        // allocate buffer
        data = malloc(size);
        if (data)
        {
                /* read file into buffer */
            size_t result = fread(data, 1, size, fp);
            if (result == size) /* success */
            {
                x->data = data;
                x->size = size;
                if (x->ms > 0)
                {
                    /* sleep to simulate work */
                #ifdef _WIN32
                    Sleep(x->ms);
                #else
                    usleep(x->ms * 1000.0);
                #endif
                }
            }
            else /* fail */
            {
                free(data);
                x->err = errno;
            }
        }
    }
    else /* catch error */
        x->err = errno;
}

static void taskobj_callback(t_pd *owner, void *data)
{
    t_taskobj *x = (t_taskobj *)owner;
    t_taskdata *y = (t_taskdata *)data;
        /* check if task has been cancelled */
    if (x)
    {
            /* move file data to object */
        x->x_data = y->data;
        x->x_size = y->size;
            /* task has completed and is now invalid.
            do this *before* outputting the message! */
        x->x_current = 0;
            /* output message */
        if (x->x_data) /* success */
            outlet_float(x->x_statusout, x->x_size);
        else /* fail */
        {
            pd_error(x, "could not read file '%s': %s (%d)",
                y->filename, strerror(y->err), y->err);
            outlet_float(x->x_statusout, -1);
        }
    }
        /* free the task data */
    freebytes(y, sizeof(t_taskdata));
}

static void taskobj_read(t_taskobj *x, t_symbol *filename, t_floatarg ms)
{
    t_taskdata *y = (t_taskdata *)getbytes(sizeof(t_taskdata));
    y->err = 0;
    y->ms = ms;
        /* move old file data */
    y->data = x->x_data;
    y->size = x->x_size;
    x->x_data = 0;
    x->x_size = 0;
        /* store file name */
    snprintf(y->filename, sizeof(y->filename), "%s", filename->s_name);
        /* cancel running task, so that we don't accidentally output
        a message while we are still reading *this* file */
    if (x->x_current)
        task_cancel(x->x_current, 0);
        /* schedule task */
    x->x_current = task_sched((t_pd *)x, y, taskobj_worker, taskobj_callback);
}

static void taskobj_get(t_taskobj *x, t_floatarg f)
{
    if (x->x_data)
    {
        int i = f;
        if (i >= 0 && i < x->x_size)
            outlet_float(x->x_byteout, x->x_data[i]);
        else
            pd_error(x, "index %d out of range!", i);
    }
    else
        pd_error(x, "no file loaded!");
}

static void *taskobj_new(void)
{
    t_taskobj *x = (t_taskobj *)pd_new(taskobj_class);
    x->x_data = 0;
    x->x_size = 0;
    x->x_current = 0;
    x->x_statusout = outlet_new(&x->x_obj, 0);
    x->x_byteout = outlet_new(&x->x_obj, 0);
    return x;
}

static void taskobj_free(t_taskobj *x)
{
        /* cancel pending tasks */
    task_join((t_pd *)x);
        /* free file data */
    if (x->x_data)
        free(x->x_data);
}

void taskobj_setup(void)
{
    taskobj_class = class_new(gensym("taskobj"),
        (t_newmethod)taskobj_new, (t_method)taskobj_free,
        sizeof(t_taskobj), 0, 0);
    class_addmethod(taskobj_class, (t_method)taskobj_read,
        gensym("read"), A_SYMBOL, A_DEFFLOAT, 0);
    class_addmethod(taskobj_class, (t_method)taskobj_get,
        gensym("get"), A_FLOAT, 0);
}
