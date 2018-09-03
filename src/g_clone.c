#include "m_pd.h"
#include "g_canvas.h"
#include "m_imp.h"
#include <string.h>

/* ---------- clone - maintain copies of a patch ----------------- */

#ifdef _WIN32
# include <malloc.h> /* MSVC or mingw on windows */
#elif defined(__linux__) || defined(__APPLE__)
# include <alloca.h> /* linux, mac, mingw, cygwin */
#else
# include <stdlib.h> /* BSDs for example */
#endif
#define LIST_NGETBYTE 100 /* bigger that this we use alloc, not alloca */

#define ATOMS_ALLOCA(x, n) ((x) = (t_atom *)((n) < LIST_NGETBYTE ?  \
        alloca((n) * sizeof(t_atom)) : getbytes((n) * sizeof(t_atom))))
#define ATOMS_FREEA(x, n) ( \
    ((n) < LIST_NGETBYTE || (freebytes((x), (n) * sizeof(t_atom)), 0)))

t_class *clone_class;
static t_class *clone_in_class, *clone_out_class;

typedef struct _copy
{
    t_glist *c_gl;
    int c_on;           /* DSP running */
} t_copy;

typedef struct _in
{
    t_class *i_pd;
    struct _clone *i_owner;
    int i_signal;
    int i_n;
} t_in;

typedef struct _out
{
    t_class *o_pd;
    t_outlet *o_outlet;
    int o_signal;
    int o_n;
} t_out;

typedef struct _clone
{
    t_object x_obj;
    t_canvas *x_owner;
    int x_n;            /* number of copies */
    t_copy *x_vec;      /* the copies */
    int x_nin;
    t_in *x_invec;      /* inlet proxies */
    int x_nout;
    t_out **x_outvec;   /* outlet proxies */
    t_outlet **x_outlets; /* extra outlet handles */
    t_symbol *x_s;      /* name of abstraction */
    int x_argc;         /* creation arguments for abstractions */
    t_atom *x_argv;
    int x_phase;
    int x_startvoice;   /* number of first voice, 0 by default */
    int x_suppressvoice; /* suppress voice number as $1 arg */
} t_clone;

int clone_match(t_pd *z, t_symbol *name, t_symbol *dir)
{
    t_clone *x = (t_clone *)z;
    if (!x->x_n)
        return (0);
    return (x->x_vec[0].c_gl->gl_name == name &&
        canvas_getdir(x->x_vec[0].c_gl) == dir);
}

void obj_sendinlet(t_object *x, int n, t_symbol *s, int argc, t_atom *argv);

static void clone_in_list(t_in *x, t_symbol *s, int argc, t_atom *argv)
{
    int n;
    if (argc < 1 || argv[0].a_type != A_FLOAT)
        pd_error(x->i_owner, "clone: no instance number in message");
    else if ((n = argv[0].a_w.w_float - x->i_owner->x_startvoice) < 0 ||
        n >= x->i_owner->x_n)
            pd_error(x->i_owner, "clone: instance number %d out of range",
                n + x->i_owner->x_startvoice);
    else if (argc > 1 && argv[1].a_type == A_SYMBOL)
        obj_sendinlet(&x->i_owner->x_vec[n].c_gl->gl_obj, x->i_n,
            argv[1].a_w.w_symbol, argc-2, argv+2);
    else obj_sendinlet(&x->i_owner->x_vec[n].c_gl->gl_obj, x->i_n,
            &s_list, argc-1, argv+1);
}

static void clone_in_this(t_in *x, t_symbol *s, int argc, t_atom *argv)
{
    int phase = x->i_owner->x_phase;
    if (phase < 0 || phase >= x->i_owner->x_n)
        phase = 0;
    if (argc <= 0)
        return;
    else if (argv->a_type == A_SYMBOL)
        obj_sendinlet(&x->i_owner->x_vec[phase].c_gl->gl_obj, x->i_n,
            argv[0].a_w.w_symbol, argc-1, argv+1);
    else obj_sendinlet(&x->i_owner->x_vec[phase].c_gl->gl_obj, x->i_n,
            &s_list, argc, argv);
}

static void clone_in_next(t_in *x, t_symbol *s, int argc, t_atom *argv)
{
    int phase = x->i_owner->x_phase + 1;
    if (phase < 0 || phase >= x->i_owner->x_n)
        phase = 0;
    x->i_owner->x_phase = phase;
    clone_in_this(x, s, argc, argv);
}

static void clone_in_set(t_in *x, t_floatarg f)
{
    int phase = f;
    if (phase < 0 || phase >= x->i_owner->x_n)
        phase = 0;
    x->i_owner->x_phase = phase;
}

static void clone_in_all(t_in *x, t_symbol *s, int argc, t_atom *argv)
{
    int phasewas = x->i_owner->x_phase, i;
    for (i = 0; i < x->i_owner->x_n; i++)
    {
        x->i_owner->x_phase = i;
        clone_in_this(x, s, argc, argv);
    }
    x->i_owner->x_phase = phasewas;
}

static void clone_in_vis(t_in *x, t_floatarg fn, t_floatarg vis)
{
    int n = fn - x->i_owner->x_startvoice;
    if (n < 0)
        n = 0;
    else if (n >= x->i_owner->x_n)
        n = x->i_owner->x_n - 1;
    canvas_vis(x->i_owner->x_vec[n].c_gl, (vis != 0));
}

static void clone_out_anything(t_out *x, t_symbol *s, int argc, t_atom *argv)
{
    t_atom *outv;
    int first =
        1 + (s != &s_list && s != &s_float && s != &s_symbol && s != &s_bang),
            outc = argc + first;
    ATOMS_ALLOCA(outv, outc);
    SETFLOAT(outv, x->o_n);
    if (first == 2)
        SETSYMBOL(outv + 1, s);
    memcpy(outv+first, argv, sizeof(t_atom) * argc);
    outlet_list(x->o_outlet, 0, outc, outv);
    ATOMS_FREEA(outv, outc);
}

static void clone_instance_init(t_clone *x, int i, t_canvas *c)
{
    x->x_vec[i].c_gl = c;
    x->x_vec[i].c_on = 0;
    t_out *outvec = x->x_outvec[i] =
        (t_out *)getbytes(x->x_nout * sizeof(*outvec));
    int j;
    for (j = 0; j < x->x_nout; j++)
    {
        outvec[j].o_pd = clone_out_class;
        outvec[j].o_signal = obj_issignaloutlet(&c->gl_obj, j);
        outvec[j].o_n = x->x_startvoice + i;
        outvec[j].o_outlet = x->x_outlets[j];
        obj_connect(&c->gl_obj, j,
            (t_object *)(&outvec[j]), 0);
    }
}

static void clone_instance_free(t_clone *x, int i)
{
    canvas_closebang(x->x_vec[i].c_gl);
    pd_free(&x->x_vec[i].c_gl->gl_pd);
    t_freebytes(x->x_outvec[i],
        x->x_nout * sizeof(*x->x_outvec[i]));
}

static t_canvas *clone_makeone(t_symbol *s, int argc, t_atom *argv)
{
    t_canvas *retval;
    pd_this->pd_newest = 0;
    typedmess(&pd_objectmaker, s, argc, argv);
    if (pd_this->pd_newest == 0)
    {
        error("clone: can't create subpatch '%s'",
            s->s_name);
        return (0);
    }
    if (*pd_this->pd_newest != canvas_class)
    {
        error("clone: can't clone '%s' because it's not an abstraction",
            s->s_name);
        pd_free(pd_this->pd_newest);
        pd_this->pd_newest = 0;
        return (0);
    }
    retval = (t_canvas *)pd_this->pd_newest;
    pd_this->pd_newest = 0;
    retval->gl_owner = 0;
    retval->gl_isclone = 1;
    return (retval);
}

/* this public routine is used in canvas_doreload() in g_editor.c */
int clone_reload(t_pd *z, t_canvas *except)
{
    int i, success = 0;
    int dspstate = canvas_suspend_dsp();
    t_clone *x = (t_clone *)z;
    canvas_setcurrent(x->x_owner);
    /* check if inlets/outlets have changed. if so,
    canvas_doreload() will remake the whole object.
    LATER find a way to handle it better. */
    if (except)
    {
        int nin = obj_ninlets(&except->gl_obj);
        int nout = obj_noutlets(&except->gl_obj);
        if (nin != x->x_nin || nout != x->x_nout)
            goto done_reloading;
        for (i = 0; i < nin; i++)
            if (obj_issignalinlet(&x->x_obj, i) 
                != obj_issignalinlet(&except->gl_obj, i))
                goto done_reloading;
        for (i = 0; i < nout; i++)
            if (obj_issignaloutlet(&x->x_obj, i) 
                != obj_issignaloutlet(&except->gl_obj, i))
                goto done_reloading;
    }
    else
    {
        bug("clone: error reloading '%s'", x->x_s->s_name);
        goto done_reloading;    
    }
    
    for (i = 0; i < x->x_n; i++)
    {
        if (x->x_vec[i].c_gl != except)
        {
            t_canvas *c;
            SETFLOAT(x->x_argv, x->x_startvoice + i);
            if (!(c = clone_makeone(x->x_s, x->x_argc - x->x_suppressvoice,
                x->x_argv + x->x_suppressvoice)))
            {
                pd_error(x, "clone: couldn't create '%s'", x->x_s->s_name);
                goto done_reloading;
            }
            clone_instance_free(x, i);
            clone_instance_init(x, i, c);
            canvas_loadbang(c);
        }
        else
        {
            /* update outgoing connections 
              (in case an outlet got deleted and re-added) */
            int j;
            for (j = 0; j < x->x_nout; j++)
            {
                t_out *outvec = x->x_outvec[i];
                obj_disconnect(&except->gl_obj, j,
                    (t_object *)(&outvec[j]), 0);
                obj_connect(&except->gl_obj, j,
                    (t_object *)(&outvec[j]), 0);
            }
        }
    }
    success = 1;
done_reloading:
    canvas_unsetcurrent(x->x_owner);
    canvas_resume_dsp(dspstate);
    return success;
}

static void clone_setn(t_clone *x, t_floatarg f)
{
    int dspstate = canvas_suspend_dsp();
    int nwas = x->x_n, wantn = f, i, j;
    if (wantn < 1)
    {
        pd_error(x, "can't resize to zero or negative number; setting to 1");
        wantn = 1;
    }
    if (wantn > nwas)
        for (i = nwas; i < wantn; i++)
    {
        t_canvas *c;
        SETFLOAT(x->x_argv, x->x_startvoice + i);
        if (!(c = clone_makeone(x->x_s, x->x_argc - x->x_suppressvoice,
            x->x_argv + x->x_suppressvoice)))
        {
            pd_error(x, "clone: couldn't create '%s'", x->x_s->s_name);
            goto done;
        }
        x->x_vec = (t_copy *)t_resizebytes(x->x_vec, i * sizeof(t_copy),
            (i+1) * sizeof(t_copy));
        x->x_outvec = (t_out **)t_resizebytes(x->x_outvec,
            i * sizeof(*x->x_outvec), (i+1) * sizeof(*x->x_outvec));
        clone_instance_init(x, i, c);
        x->x_n++;
    }
    if (wantn < nwas)
    {
        for (i = wantn; i < nwas; i++)
            clone_instance_free(x, i);
        x->x_vec = (t_copy *)t_resizebytes(x->x_vec, nwas * sizeof(t_copy),
            wantn * sizeof(*x->x_vec));
        x->x_outvec = (t_out **)t_resizebytes(x->x_outvec,
            nwas * sizeof(*x->x_outvec), wantn * sizeof(*x->x_outvec));
        x->x_n = wantn;
    }
done:
    canvas_resume_dsp(dspstate);
}

static void clone_click(t_clone *x, t_floatarg xpos, t_floatarg ypos,
    t_floatarg shift, t_floatarg ctrl, t_floatarg alt)
{
    if (!x->x_n)
        return;
    canvas_vis(x->x_vec[0].c_gl, 1);
}

static void clone_loadbang(t_clone *x, t_floatarg f)
{
    int i;
    if (f == LB_LOAD)
        for (i = 0; i < x->x_n; i++)
            canvas_loadbang(x->x_vec[i].c_gl);
    else if (f == LB_CLOSE)
        for (i = 0; i < x->x_n; i++)
            canvas_closebang(x->x_vec[i].c_gl);
}

void canvas_dodsp(t_canvas *x, int toplevel, t_signal **sp);
t_signal *signal_newfromcontext(int borrowed);
void signal_makereusable(t_signal *sig);

static void clone_dsp(t_clone *x, t_signal **sp)
{
    int i, j, nin, nout;
    t_signal **tempsigs;
    if (!x->x_n)
        return;
    for (i = nin = 0; i < x->x_nin; i++)
        if (x->x_invec[i].i_signal)
            nin++;
    for (i = nout = 0; i < x->x_nout; i++)
        if (x->x_outvec[0][i].o_signal)
            nout++;
    for (j = 0; j < x->x_n; j++)
    {
        if (obj_ninlets(&x->x_vec[j].c_gl->gl_obj) != x->x_nin ||
            obj_noutlets(&x->x_vec[j].c_gl->gl_obj) != x->x_nout ||
                obj_nsiginlets(&x->x_vec[j].c_gl->gl_obj) != nin ||
                    obj_nsigoutlets(&x->x_vec[j].c_gl->gl_obj) != nout)
        {
            pd_error(x, "clone: can't do DSP until edited copy is saved");
            for (i = 0; i < nout; i++)
                dsp_add_zero(sp[nin+i]->s_vec, sp[nin+i]->s_n);
            return;
        }
    }
    tempsigs = (t_signal **)alloca((nin + 3 * nout) * sizeof(*tempsigs));
        /* load input signals into signal vector to send subpatches */
    for (i = 0; i < nin; i++)
    {
            /* we already have one reference "counted" for our presumed
            use of this input signal but we must add the others. */
        sp[i]->s_refcount += x->x_n-1;
        tempsigs[2 * nout + i] = sp[i];
    }
        /* for first copy, write output to first nout temp sigs */
    for (i = 0; i < nout; i++)
        tempsigs[i] = tempsigs[2 * nout + nin + i] = signal_newfromcontext(1);
    canvas_dodsp(x->x_vec[0].c_gl, 0, tempsigs + 2*nout);
        /* for remaining copies, write to second nout temp sigs */
    for (j = 1; j < x->x_n; j++)
    {
        for (i = 0; i < nout; i++)
            tempsigs[nout+i] = tempsigs[2 * nout + nin + i] =
                signal_newfromcontext(1);
        canvas_dodsp(x->x_vec[j].c_gl, 0, tempsigs + 2*nout);
        for (i = 0; i < nout; i++)
        {
            dsp_add_plus(tempsigs[nout + i]->s_vec, tempsigs[i]->s_vec,
                tempsigs[i]->s_vec, tempsigs[i]->s_n);
            signal_makereusable(tempsigs[nout + i]);
        }
    }
        /* copy to output signsls */
    for (i = 0; i < nout; i++)
    {
        dsp_add_copy(tempsigs[i]->s_vec, sp[nin+i]->s_vec, tempsigs[i]->s_n);
        signal_makereusable(tempsigs[i]);
    }
}

static void clone_free(t_clone *x)
{
    if (x->x_vec)
    {
        int i;
        for (i = 0; i < x->x_n; i++)
            clone_instance_free(x, i);
        t_freebytes(x->x_vec, x->x_n * sizeof(*x->x_vec));
        t_freebytes(x->x_argv, x->x_argc * sizeof(*x->x_argv));
        t_freebytes(x->x_invec, x->x_nin * sizeof(*x->x_invec));
        t_freebytes(x->x_outvec, x->x_n * sizeof(*x->x_outvec));
        t_freebytes(x->x_outlets, x->x_n * sizeof(*x->x_outlets));
    }
}

static void *clone_new(t_symbol *s, int argc, t_atom *argv)
{
    t_clone *x = (t_clone *)pd_new(clone_class);
    t_canvas *c;
    int wantn, dspstate, i;
    x->x_owner = canvas_getcurrent();
    x->x_invec = 0;
    x->x_outvec = 0;
    x->x_startvoice = 0;
    x->x_suppressvoice = 0;
    if (argc == 0)
    {
        x->x_vec = 0;
        x->x_n = 0;
        return (x);
    }
    dspstate = canvas_suspend_dsp();
    while (argc > 0 && argv[0].a_type == A_SYMBOL &&
        argv[0].a_w.w_symbol->s_name[0] == '-')
    {
        if (!strcmp(argv[0].a_w.w_symbol->s_name, "-s") && argc > 1 &&
            argv[1].a_type == A_FLOAT)
        {
            x->x_startvoice = argv[1].a_w.w_float;
            argc -= 2; argv += 2;
        }
        else if (!strcmp(argv[0].a_w.w_symbol->s_name, "-x"))
            x->x_suppressvoice = 1, argc--, argv++;
        else goto usage;
    }
    if (argc >= 2 && (wantn = atom_getfloatarg(0, argc, argv)) >= 0
        && argv[1].a_type == A_SYMBOL)
            x->x_s = argv[1].a_w.w_symbol;
    else if (argc >= 2 && (wantn = atom_getfloatarg(1, argc, argv)) >= 0
        && argv[0].a_type == A_SYMBOL)
            x->x_s = argv[0].a_w.w_symbol;
    else goto usage;
        /* store a copy of the argmuents with an extra space (argc+1) for
        supplying an instance number, which we'll bash as we go. */
    x->x_argc = argc - 1;
    x->x_argv = getbytes(x->x_argc * sizeof(*x->x_argv));
    memcpy(x->x_argv, argv+1, x->x_argc * sizeof(*x->x_argv));
    SETFLOAT(x->x_argv, x->x_startvoice);
    if (!(c = clone_makeone(x->x_s, x->x_argc - x->x_suppressvoice,
        x->x_argv + x->x_suppressvoice)))
            goto fail;
    x->x_vec = (t_copy *)getbytes(sizeof(*x->x_vec));
    x->x_n = 1;
    x->x_nin = obj_ninlets(&c->gl_obj);
    x->x_invec = (t_in *)getbytes(x->x_nin * sizeof(*x->x_invec));
    for (i = 0; i < x->x_nin; i++)
    {
        x->x_invec[i].i_pd = clone_in_class;
        x->x_invec[i].i_owner = x;
        x->x_invec[i].i_signal =
            obj_issignalinlet(&c->gl_obj, i);
        x->x_invec[i].i_n = i;
        if (x->x_invec[i].i_signal)
            signalinlet_new(&x->x_obj, 0);
        else inlet_new(&x->x_obj, &x->x_invec[i].i_pd, 0, 0);
    }
    x->x_nout = obj_noutlets(&c->gl_obj);
    x->x_outlets = (t_outlet **)getbytes(x->x_nout * sizeof(*x->x_outlets));
    for (i = 0; i < x->x_nout; i++)
        x->x_outlets[i] = outlet_new(&x->x_obj, 
            (obj_issignaloutlet(&c->gl_obj, i) ? &s_signal : 0));
    /* init first instance */
    x->x_outvec = (t_out **)getbytes(sizeof(*x->x_outvec));
    clone_instance_init(x, 0, c);
    /* then resize */
    clone_setn(x, (t_floatarg)(wantn));
    x->x_phase = wantn-1;
    canvas_resume_dsp(dspstate);
    return (x);
usage:
    error("usage: clone [-s starting-number] <number> <name> [arguments]");
fail:
    freebytes(x, sizeof(t_clone));
    canvas_resume_dsp(dspstate);
    return (0);
}

void clone_setup(void)
{
    clone_class = class_new(gensym("clone"), (t_newmethod)clone_new,
        (t_method)clone_free, sizeof(t_clone), CLASS_NOINLET, A_GIMME, 0);
    class_addmethod(clone_class, (t_method)clone_click, gensym("click"),
        A_FLOAT, A_FLOAT, A_FLOAT, A_FLOAT, A_FLOAT, 0);
    class_addmethod(clone_class, (t_method)clone_loadbang, gensym("loadbang"),
        A_FLOAT, 0);
    class_addmethod(clone_class, (t_method)clone_dsp,
        gensym("dsp"), A_CANT, 0);

    clone_in_class = class_new(gensym("clone-inlet"), 0, 0,
        sizeof(t_in), CLASS_PD, 0);
    class_addmethod(clone_in_class, (t_method)clone_in_next, gensym("next"),
        A_GIMME, 0);
    class_addmethod(clone_in_class, (t_method)clone_in_this, gensym("this"),
        A_GIMME, 0);
    class_addmethod(clone_in_class, (t_method)clone_in_set, gensym("set"),
        A_FLOAT, 0);
    class_addmethod(clone_in_class, (t_method)clone_in_all, gensym("all"),
        A_GIMME, 0);
    class_addmethod(clone_in_class, (t_method)clone_in_vis, gensym("vis"),
        A_FLOAT, A_FLOAT, 0);
    class_addlist(clone_in_class, (t_method)clone_in_list);

    clone_out_class = class_new(gensym("clone-outlet"), 0, 0,
        sizeof(t_in), CLASS_PD, 0);
    class_addanything(clone_out_class, (t_method)clone_out_anything);
}
