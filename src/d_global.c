/* Copyright (c) 1997-1999 Miller Puckette.
* For information on usage and redistribution, and for a DISCLAIMER OF ALL
* WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

/*  send~, receive~, throw~, catch~ */

#include "m_pd.h"
#include "s_stuff.h"
#include <string.h>

#if PD_DSPTHREADS
# include "s_spinlock.h"
# define LOCK(x) rwspinlock_wrlock((t_rwspinlock *)&x)
# define UNLOCK(x) rwspinlock_wrunlock((t_rwspinlock *)&x)
# define LOCK_SHARED(x) rwspinlock_rdlock((t_rwspinlock *)&x)
# define UNLOCK_SHARED(x) rwspinlock_rdunlock((t_rwspinlock *)&x)
#else
# define LOCK(x)
# define UNLOCK(x)
# define LOCK_SHARED(x)
# define UNLOCK_SHARED(x)
#endif

#define DEFSENDVS 64    /* LATER get send to get this from canvas */

/* ----------------------------- send~ ----------------------------- */
static t_class *sigsend_class;

typedef struct _sigsend
{
    t_object x_obj;
    t_symbol *x_sym;
    int x_n;
    t_sample *x_vec;
    t_float x_f;
#if PD_DSPTHREADS
    t_rwspinlock x_lock;
#endif
} t_sigsend;

static void *sigsend_new(t_symbol *s)
{
    t_sigsend *x = (t_sigsend *)pd_new(sigsend_class);
    pd_bind(&x->x_obj.ob_pd, s);
    x->x_sym = s;
    x->x_n = DEFSENDVS;
    x->x_vec = (t_sample *)getbytes(DEFSENDVS * sizeof(t_sample));
    memset((char *)(x->x_vec), 0, DEFSENDVS * sizeof(t_sample));
    x->x_f = 0;
#if PD_DSPTHREADS
    rwspinlock_init((t_rwspinlock *)&x->x_lock);
#endif
    return (x);
}

static t_int *sigsend_perform(t_int *w)
{
    t_sigsend *x = (t_sigsend *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *out = x->x_vec;
    int n = (int)(w[3]);
    LOCK(x->x_lock);
    while (n--)
    {
        *out = (PD_BIGORSMALL(*in) ? 0 : *in);
        out++;
        in++;
    }
    UNLOCK(x->x_lock);
    return (w+4);
}

static void sigsend_dsp(t_sigsend *x, t_signal **sp)
{
    if (x->x_n == sp[0]->s_n)
        dsp_add(sigsend_perform, 3, x, sp[0]->s_vec, (t_int)sp[0]->s_n);
    else pd_error(0, "sigsend %s: unexpected vector size", x->x_sym->s_name);
}

static void sigsend_free(t_sigsend *x)
{
    pd_unbind(&x->x_obj.ob_pd, x->x_sym);
    freebytes(x->x_vec, x->x_n * sizeof(t_sample));
}

static void sigsend_setup(void)
{
    sigsend_class = class_new(gensym("send~"), (t_newmethod)sigsend_new,
        (t_method)sigsend_free, sizeof(t_sigsend), CLASS_DEFAULT, A_DEFSYM, 0);
    class_addcreator((t_newmethod)sigsend_new, gensym("s~"), A_DEFSYM, 0);
    CLASS_MAINSIGNALIN(sigsend_class, t_sigsend, x_f);
    class_addmethod(sigsend_class, (t_method)sigsend_dsp,
        gensym("dsp"), A_CANT, 0);
}

/* ----------------------------- receive~ ----------------------------- */
static t_class *sigreceive_class;

typedef struct _sigreceive
{
    t_object x_obj;
    t_symbol *x_sym;
    t_sample *x_wherefrom;
    int x_n;
#if PD_DSPTHREADS
    t_rwspinlock *x_lock;
#endif
} t_sigreceive;

static void *sigreceive_new(t_symbol *s)
{
    t_sigreceive *x = (t_sigreceive *)pd_new(sigreceive_class);
    x->x_n = DEFSENDVS;             /* LATER find our vector size correctly */
    x->x_sym = s;
    x->x_wherefrom = 0;
#if PD_DSPTHREADS
    x->x_lock = 0;
#endif
    outlet_new(&x->x_obj, &s_signal);
    return (x);
}

static t_int *sigreceive_perform(t_int *w)
{
    t_sigreceive *x = (t_sigreceive *)(w[1]);
    t_sample *out = (t_sample *)(w[2]);
    int n = (int)(w[3]);
    t_sample *in = x->x_wherefrom;
    if (in)
    {
        LOCK_SHARED(*x->x_lock);
        while (n--)
            *out++ = *in++;
        UNLOCK_SHARED(*x->x_lock);
    }
    else
    {
        while (n--)
            *out++ = 0;
    }
    return (w+4);
}

/* tb: vectorized receive function */
static t_int *sigreceive_perf8(t_int *w)
{
    t_sigreceive *x = (t_sigreceive *)(w[1]);
    t_sample *out = (t_sample *)(w[2]);
    int n = (int)(w[3]);
    t_sample *in = x->x_wherefrom;
    if (in)
    {
        LOCK_SHARED(*x->x_lock);
        for (; n; n -= 8, in += 8, out += 8)
        {
            out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; out[3] = in[3];
            out[4] = in[4]; out[5] = in[5]; out[6] = in[6]; out[7] = in[7];
        }
        UNLOCK_SHARED(*x->x_lock);
    }
    else
    {
        for (; n; n -= 8, in += 8, out += 8)
        {
            out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 0;
            out[4] = 0; out[5] = 0; out[6] = 0; out[7] = 0;
        }
    }
    return (w+4);
}

static void sigreceive_set(t_sigreceive *x, t_symbol *s)
{
    t_sigsend *sender = (t_sigsend *)pd_findbyclass((x->x_sym = s),
        sigsend_class);
    if (sender)
    {
        if (sender->x_n == x->x_n)
        {
            x->x_wherefrom = sender->x_vec;
        #if PD_DSPTHREADS
            x->x_lock = &sender->x_lock;
        #endif
        }
        else
        {
            pd_error(x, "receive~ %s: vector size mismatch", x->x_sym->s_name);
            x->x_wherefrom = 0;
        #if PD_DSPTHREADS
            x->x_lock = 0;
        #endif
        }
    }
    else
    {
        pd_error(x, "receive~ %s: no matching send", x->x_sym->s_name);
        x->x_wherefrom = 0;
    }
}

static void sigreceive_dsp(t_sigreceive *x, t_signal **sp)
{
    if (sp[0]->s_n != x->x_n)
    {
        pd_error(x, "receive~ %s: vector size mismatch", x->x_sym->s_name);
    }
    else
    {
        sigreceive_set(x, x->x_sym);
        if (sp[0]->s_n&7)
            dsp_add(sigreceive_perform, 3,
                x, sp[0]->s_vec, (t_int)sp[0]->s_n);
        else dsp_add(sigreceive_perf8, 3,
            x, sp[0]->s_vec, (t_int)sp[0]->s_n);
    }
}

static void sigreceive_setup(void)
{
    sigreceive_class = class_new(gensym("receive~"),
        (t_newmethod)sigreceive_new, 0,
        sizeof(t_sigreceive), CLASS_DEFAULT, A_DEFSYM, 0);
    class_addcreator((t_newmethod)sigreceive_new, gensym("r~"), A_DEFSYM, 0);
    class_addmethod(sigreceive_class, (t_method)sigreceive_set, gensym("set"),
        A_SYMBOL, 0);
    class_addmethod(sigreceive_class, (t_method)sigreceive_dsp,
        gensym("dsp"), A_CANT, 0);
    class_sethelpsymbol(sigreceive_class, gensym("send~"));
}

/* ----------------------------- catch~ ----------------------------- */
static t_class *sigcatch_class;

typedef struct _sigcatch
{
    t_object x_obj;
    t_symbol *x_sym;
    int x_n;
    t_sample *x_vec;
#if PD_DSPTHREADS
    t_rwspinlock x_lock;
#endif
} t_sigcatch;

static void *sigcatch_new(t_symbol *s)
{
    t_sigcatch *x = (t_sigcatch *)pd_new(sigcatch_class);
    pd_bind(&x->x_obj.ob_pd, s);
    x->x_sym = s;
    x->x_n = DEFSENDVS;
    x->x_vec = (t_sample *)getbytes(DEFSENDVS * sizeof(t_sample));
    memset((char *)(x->x_vec), 0, DEFSENDVS * sizeof(t_sample));
#if PD_DSPTHREADS
    rwspinlock_init((t_rwspinlock *)&x->x_lock);
#endif
    outlet_new(&x->x_obj, &s_signal);
    return (x);
}

static t_int *sigcatch_perform(t_int *w)
{
    t_sigcatch *x = (t_sigcatch *)(w[1]);
    t_sample *in = x->x_vec;
    t_sample *out = (t_sample *)(w[2]);
    int n = (int)(w[3]);
    LOCK(x->x_lock);
    while (n--) *out++ = *in, *in++ = 0;
    UNLOCK(x->x_lock);
    return (w+4);
}

/* tb: vectorized catch function */
static t_int *sigcatch_perf8(t_int *w)
{
    t_sigcatch *x = (t_sigcatch *)(w[1]);
    t_sample *in = x->x_vec;
    t_sample *out = (t_sample *)(w[2]);
    int n = (int)(w[3]);
    /* reading + writing */
    LOCK(x->x_lock);
    for (; n; n -= 8, in += 8, out += 8)
    {
       out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; out[3] = in[3];
       out[4] = in[4]; out[5] = in[5]; out[6] = in[6]; out[7] = in[7];

       in[0] = 0; in[1] = 0; in[2] = 0; in[3] = 0;
       in[4] = 0; in[5] = 0; in[6] = 0; in[7] = 0;
    }
    UNLOCK(x->x_lock);
    return (w+4);
}

static void sigcatch_dsp(t_sigcatch *x, t_signal **sp)
{
    if (x->x_n == sp[0]->s_n)
    {
        if(sp[0]->s_n&7)
            dsp_add(sigcatch_perform, 3, x, sp[0]->s_vec, (t_int)sp[0]->s_n);
        else
            dsp_add(sigcatch_perf8, 3, x, sp[0]->s_vec, (t_int)sp[0]->s_n);
    }
    else pd_error(0, "sigcatch %s: unexpected vector size", x->x_sym->s_name);
}

static void sigcatch_free(t_sigcatch *x)
{
    pd_unbind(&x->x_obj.ob_pd, x->x_sym);
    freebytes(x->x_vec, x->x_n * sizeof(t_sample));
}

static void sigcatch_setup(void)
{
    sigcatch_class = class_new(gensym("catch~"), (t_newmethod)sigcatch_new,
        (t_method)sigcatch_free, sizeof(t_sigcatch),
            CLASS_THREADSAFE | CLASS_NOINLET, A_DEFSYM, 0);
    class_addmethod(sigcatch_class, (t_method)sigcatch_dsp,
        gensym("dsp"), A_CANT, 0);
    class_sethelpsymbol(sigcatch_class, gensym("throw~"));
}

/* ----------------------------- throw~ ----------------------------- */
static t_class *sigthrow_class;

typedef struct _sigthrow
{
    t_object x_obj;
    t_symbol *x_sym;
    t_sample *x_whereto;
    int x_n;
    t_float x_f;
#if PD_DSPTHREADS
    t_rwspinlock *x_lock;
#endif
} t_sigthrow;

static void *sigthrow_new(t_symbol *s)
{
    t_sigthrow *x = (t_sigthrow *)pd_new(sigthrow_class);
    x->x_sym = s;
    x->x_whereto  = 0;
    x->x_n = DEFSENDVS;
    x->x_f = 0;
#if PD_DSPTHREADS
    x->x_lock = 0;
#endif
    return (x);
}

static t_int *sigthrow_perform(t_int *w)
{
    t_sigthrow *x = (t_sigthrow *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    int n = (int)(w[3]);
    t_sample *out = x->x_whereto;
    if (out)
    {
        LOCK(*x->x_lock);
        while (n--)
        {
            *out += (PD_BIGORSMALL(*in) ? 0 : *in);
            out++;
            in++;
        }
        UNLOCK(*x->x_lock);
    }
    return (w+4);
}

static void sigthrow_set(t_sigthrow *x, t_symbol *s)
{
    t_sigcatch *catcher = (t_sigcatch *)pd_findbyclass((x->x_sym = s),
        sigcatch_class);
    if (catcher)
    {
        if (catcher->x_n == x->x_n)
        {
            x->x_whereto = catcher->x_vec;
        #if PD_DSPTHREADS
            x->x_lock = &catcher->x_lock;
        #endif
        }
        else
        {
            pd_error(x, "throw~ %s: vector size mismatch", x->x_sym->s_name);
            x->x_whereto = 0;
        #if PD_DSPTHREADS
            x->x_lock = 0;
        #endif
        }
    }
    else x->x_whereto = 0;  /* no match: now no longer considered an error */
}

static void sigthrow_dsp(t_sigthrow *x, t_signal **sp)
{
    if (sp[0]->s_n != x->x_n)
    {
        pd_error(x, "throw~ %s: vector size mismatch", x->x_sym->s_name);
    }
    else
    {
        sigthrow_set(x, x->x_sym);
        dsp_add(sigthrow_perform, 3,
            x, sp[0]->s_vec, (t_int)sp[0]->s_n);
    }
}

static void sigthrow_setup(void)
{
    sigthrow_class = class_new(gensym("throw~"), (t_newmethod)sigthrow_new, 0,
        sizeof(t_sigthrow), CLASS_DEFAULT, A_DEFSYM, 0);
    class_addmethod(sigthrow_class, (t_method)sigthrow_set, gensym("set"),
        A_SYMBOL, 0);
    CLASS_MAINSIGNALIN(sigthrow_class, t_sigthrow, x_f);
    class_addmethod(sigthrow_class, (t_method)sigthrow_dsp,
        gensym("dsp"), A_CANT, 0);
}

/* ----------------------- global setup routine ---------------- */

void d_global_setup(void)
{
    sigsend_setup();
    sigreceive_setup();
    sigcatch_setup();
    sigthrow_setup();
}

