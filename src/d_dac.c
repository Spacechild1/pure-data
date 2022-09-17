/* Copyright (c) 1997-1999 Miller Puckette.
* For information on usage and redistribution, and for a DISCLAIMER OF ALL
* WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

/*  The dac~ and adc~ routines.
*/

#include "m_pd.h"
#include "s_stuff.h"

#if PD_DSPTHREADS
#include "s_spinlock.h"
#endif

/* ----------------------------- dac~ --------------------------- */
static t_class *dac_class;

typedef struct _dac
{
    t_object x_obj;
    t_int x_n;
    t_int *x_vec;
    t_float x_f;
} t_dac;

static void *dac_new(t_symbol *s, int argc, t_atom *argv)
{
    t_dac *x = (t_dac *)pd_new(dac_class);
    t_atom defarg[2];
    int i;
    if (!argc)
    {
        argv = defarg;
        argc = 2;
        SETFLOAT(&defarg[0], 1);
        SETFLOAT(&defarg[1], 2);
    }
    x->x_n = argc;
    x->x_vec = (t_int *)getbytes(argc * sizeof(*x->x_vec));
    for (i = 0; i < argc; i++)
        x->x_vec[i] = atom_getfloatarg(i, argc, argv);
    for (i = 1; i < argc; i++)
        inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    x->x_f = 0;
    return (x);
}

#if PD_DSPTHREADS
t_int *dac_perform8(t_int *w)
{
    t_sample *in = (t_sample *)(w[1]);
    t_sample *out = (t_sample *)(w[2]);
    t_spinlock *lock = (t_spinlock *)(w[3]);
    int n = DEFDACBLKSIZE;
    spinlock_lock(lock);
    for (; n; n -= 8, in += 8, out += 8)
    {
        t_sample f0 = in[0], f1 = in[1], f2 = in[2], f3 = in[3];
        t_sample f4 = in[4], f5 = in[5], f6 = in[6], f7 = in[7];

        t_sample g0 = out[0], g1 = out[1], g2 = out[2], g3 = out[3];
        t_sample g4 = out[4], g5 = out[5], g6 = out[6], g7 = out[7];

        out[0] = f0 + g0; out[1] = f1 + g1; out[2] = f2 + g2; out[3] = f3 + g3;
        out[4] = f4 + g4; out[5] = f5 + g5; out[6] = f6 + g6; out[7] = f7 + g7;
    }
    spinlock_unlock(lock);
    return w+4;
}
#endif /* PD_DSPTHREADS */

static void dac_dsp(t_dac *x, t_signal **sp)
{
    t_int i, *ip;
    t_signal **sp2;
    for (i = x->x_n, ip = x->x_vec, sp2 = sp; i--; ip++, sp2++)
    {
        int ch = (int)(*ip - 1);
        if ((*sp2)->s_n != DEFDACBLKSIZE)
            pd_error(0, "dac~: bad vector size");
        else if (ch >= 0 && ch < sys_get_outchannels())
        {
            t_sample *in = (*sp2)->s_vec;
            t_sample *out = STUFF->st_soundout + DEFDACBLKSIZE*ch;
        #if PD_DSPTHREADS
            t_spinlock *lock = &STUFF->st_soundout_locks[ch];
            if (!(sp[0]->s_n & 7)) /* always true for DEFDACBLKSIZE */
                dsp_add(dac_perform8, 3, in, out, lock);
            else
                bug("dac_dsp");
        #else
            dsp_add_plus(out, in, out, DEFDACBLKSIZE);
        #endif
        }
    }
}

static void dac_set(t_dac *x, t_symbol *s, int argc, t_atom *argv)
{
    int i;
    for (i = 0; i < argc && i < x->x_n; i++)
        x->x_vec[i] = atom_getfloatarg(i, argc, argv);
    canvas_update_dsp();
}

static void dac_free(t_dac *x)
{
    freebytes(x->x_vec, x->x_n * sizeof(*x->x_vec));
}

static void dac_setup(void)
{
    dac_class = class_new(gensym("dac~"), (t_newmethod)dac_new,
        (t_method)dac_free, sizeof(t_dac), CLASS_DEFAULT, A_GIMME, 0);
    CLASS_MAINSIGNALIN(dac_class, t_dac, x_f);
    class_addmethod(dac_class, (t_method)dac_dsp, gensym("dsp"), A_CANT, 0);
    class_addmethod(dac_class, (t_method)dac_set, gensym("set"), A_GIMME, 0);
    class_sethelpsymbol(dac_class, gensym("adc~_dac~"));
}

/* ----------------------------- adc~ --------------------------- */
static t_class *adc_class;

typedef struct _adc
{
    t_object x_obj;
    t_int x_n;
    t_int *x_vec;
} t_adc;

static void *adc_new(t_symbol *s, int argc, t_atom *argv)
{
    t_adc *x = (t_adc *)pd_new(adc_class);
    t_atom defarg[2];
    int i;
    if (!argc)
    {
        argv = defarg;
        argc = 2;
        SETFLOAT(&defarg[0], 1);
        SETFLOAT(&defarg[1], 2);
    }
    x->x_n = argc;
    x->x_vec = (t_int *)getbytes(argc * sizeof(*x->x_vec));
    for (i = 0; i < argc; i++)
        x->x_vec[i] = atom_getfloatarg(i, argc, argv);
    for (i = 0; i < argc; i++)
        outlet_new(&x->x_obj, &s_signal);
    return (x);
}

static void adc_dsp(t_adc *x, t_signal **sp)
{
    t_int i, *ip;
    t_signal **sp2;
    for (i = x->x_n, ip = x->x_vec, sp2 = sp; i--; ip++, sp2++)
    {
        int ch = (int)(*ip - 1);
        if ((*sp2)->s_n != DEFDACBLKSIZE)
            pd_error(0, "adc~: bad vector size");
        else if (ch >= 0 && ch < sys_get_inchannels())
            dsp_add_copy(STUFF->st_soundin + DEFDACBLKSIZE*ch,
                (*sp2)->s_vec, DEFDACBLKSIZE);
        else dsp_add_zero((*sp2)->s_vec, DEFDACBLKSIZE);
    }
}

static void adc_set(t_adc *x, t_symbol *s, int argc, t_atom *argv)
{
    int i;
    for (i = 0; i < argc && i < x->x_n; i++)
        x->x_vec[i] = atom_getfloatarg(i, argc, argv);
    canvas_update_dsp();
}

static void adc_free(t_adc *x)
{
    freebytes(x->x_vec, x->x_n * sizeof(*x->x_vec));
}

static void adc_setup(void)
{
    adc_class = class_new(gensym("adc~"), (t_newmethod)adc_new,
        (t_method)adc_free, sizeof(t_adc), CLASS_DEFAULT, A_GIMME, 0);
    class_addmethod(adc_class, (t_method)adc_dsp, gensym("dsp"), A_CANT, 0);
    class_addmethod(adc_class, (t_method)adc_set, gensym("set"), A_GIMME, 0);
    class_sethelpsymbol(adc_class, gensym("adc~_dac~"));
}

void d_dac_setup(void)
{
    dac_setup();
    adc_setup();
}

