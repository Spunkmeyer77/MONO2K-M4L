/* SPDX-License-Identifier: GPL-3.0-only */
/* mono2k~: MONO2K (Inigo Hernandez's Monomachine-style instrument, github.com/InigoHernandez/mono2k) as a Max object
 * for Max for Live. The core runs unchanged; this object plays it, feeds it Live's clock and MIDI, and shows its screen.
 *
 * inlet:   signal: Live's beat phase (plugphasor~), and every message
 * outlets: left, right audio; the screen 160 x 160 (jit_matrix); the screen 240 x 240 (jit_matrix); the LEDs (list of
 *          41: 27 keys then 14 buttons, 0..255); the saved state ("set ..." for a [pattr] kept in the Live set)
 * messages: btn i down | key i down | enc i delta | encabs i value | midi byte... | transport 0/1 | pos beats | sync 0/1 |
 *           route 0..6 (0 the track in view) | state ... (a saved state back) | ink r g b | paper r g b */
#include "ext.h"
#include "ext_obex.h"
#include "ext_critical.h"
#include "z_dsp.h"
#include "jit.common.h"
#include "m2k_engine.h"

#define SMALL 160

typedef struct _mono2k {
    t_pxobject ob;
    void *out_small, *out_big, *out_leds, *out_state;
    void *qe;
    t_critical crit;
    long owner;                       /* 1: this one plays the core (one per Live set) */
    uint32_t seen_audio, seen_main;
    void *mat_small, *mat_big;
    t_symbol *name_small, *name_big;
    uint8_t fb[E_FB_BYTES];
    uint8_t leds[E_LEDS];
    long leds_sent[E_LEDS];
    long leds_valid;
    float ink[3], paper[3];
    /* the MIDI parser: [midiin]'s bytes */
    int mst, mn, md[2], msysex;
    /* the state's buffers */
    uint8_t *sbuf;
    int32_t *ibuf;
    t_atom *abuf;
    /* the perform's float buffers */
    float *fin, *fl, *fr;
    long fcap;
    long connected_phase;
    long enc_last[8], enc_have[8];    /* encabs: Live's dials (Push's knobs) as the encoders */
} t_mono2k;

static t_class *mono2k_class;
static t_mono2k *g_owner;

static void *mono2k_new(t_symbol *s, long argc, t_atom *argv);
static void mono2k_free(t_mono2k *x);
static void mono2k_assist(t_mono2k *x, void *b, long m, long a, char *s);
static void mono2k_dsp64(t_mono2k *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
static void mono2k_qfn(t_mono2k *x);

static void push(t_mono2k *x, int kind, int a, int b)
{
    if (!x->owner) return;
    critical_enter(x->crit);
    e_push(kind, a, b);
    critical_exit(x->crit);
}

static void mono2k_btn(t_mono2k *x, long i, long d) { push(x, EV_BTN, (int)i, d != 0); }
static void mono2k_key(t_mono2k *x, long i, long d) { push(x, EV_KEY, (int)i, d != 0); }
static void mono2k_enc(t_mono2k *x, long i, long d) { if (d) push(x, EV_ENC, (int)i, (int)d); }
/* a dial's value (a Live parameter, so Push's knobs reach it): its change turns encoder i */
static void mono2k_encabs(t_mono2k *x, long i, long v)
{
    if (i < 0 || i > 7) return;
    if (x->enc_have[i] && v != x->enc_last[i]) push(x, EV_ENC, (int)i, (int)(v - x->enc_last[i]));
    x->enc_last[i] = v;
    x->enc_have[i] = 1;
}
static void mono2k_transport(t_mono2k *x, long on) { push(x, EV_TRANSPORT, on != 0, 0); }
static void mono2k_pos(t_mono2k *x, double beats) { push(x, EV_POS, 0, (int)(beats * 1000.0 + 0.5)); }
static void mono2k_sync(t_mono2k *x, long on) { push(x, EV_SYNC, on != 0, 0); }
static void mono2k_route(t_mono2k *x, long r) { push(x, EV_ROUTE, (int)r, 0); }

static void midi_byte(t_mono2k *x, int b)
{
    b &= 255;
    if (b >= 0xF8) return;                            /* real time: Live's clock comes as the phase */
    if (b & 0x80) {
        if (b == 0xF0) { x->msysex = 1; return; }
        x->msysex = 0;
        if (b >= 0xF0) { x->mst = 0; return; }
        x->mst = b;
        x->mn = 0;
        return;
    }
    if (x->msysex || !x->mst) return;
    x->md[x->mn++] = b;
    int need = ((x->mst & 0xF0) == 0xC0 || (x->mst & 0xF0) == 0xD0) ? 1 : 2;
    if (x->mn < need) return;
    push(x, EV_MIDI, x->mst, x->md[0] | (need == 2 ? x->md[1] << 8 : 0));
    x->mn = 0;                                        /* (running status) */
}

static void mono2k_midi(t_mono2k *x, t_symbol *s, long argc, t_atom *argv)
{
    for (long i = 0; i < argc; i++) midi_byte(x, (int)atom_getlong(argv + i));
}

static void mono2k_int(t_mono2k *x, long n) { midi_byte(x, (int)n); }   /* [midiin] straight in */

static void mono2k_state(t_mono2k *x, t_symbol *s, long argc, t_atom *argv)
{
    if (!x->owner || argc < 3) return;
    long n = argc > e_state_atoms(E_STATE_MAX) ? e_state_atoms(E_STATE_MAX) : argc;
    for (long i = 0; i < n; i++) x->ibuf[i] = (int32_t)atom_getlong(argv + i);
    int len = e_state_decode(x->ibuf, (int)n, x->sbuf, E_STATE_MAX);
    if (len > 0) e_state_put(x->sbuf, len);
    else object_warn((t_object *)x, "saved state not recognised: starting from the factory patterns");
}

static void set_col(float *c, double r, double g, double b)
{
    c[0] = (float)(r < 0 ? 0 : r > 1 ? 1 : r);
    c[1] = (float)(g < 0 ? 0 : g > 1 ? 1 : g);
    c[2] = (float)(b < 0 ? 0 : b > 1 ? 1 : b);
}
static void mono2k_ink(t_mono2k *x, double r, double g, double b) { set_col(x->ink, r, g, b); x->seen_main = 0; }
static void mono2k_paper(t_mono2k *x, double r, double g, double b) { set_col(x->paper, r, g, b); x->seen_main = 0; }

void ext_main(void *r)
{
    t_class *c = class_new("mono2k~", (method)mono2k_new, (method)mono2k_free, sizeof(t_mono2k), 0L, A_GIMME, 0);
    class_addmethod(c, (method)mono2k_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)mono2k_assist, "assist", A_CANT, 0);
    class_addmethod(c, (method)mono2k_btn, "btn", A_LONG, A_LONG, 0);
    class_addmethod(c, (method)mono2k_key, "key", A_LONG, A_LONG, 0);
    class_addmethod(c, (method)mono2k_enc, "enc", A_LONG, A_LONG, 0);
    class_addmethod(c, (method)mono2k_encabs, "encabs", A_LONG, A_LONG, 0);
    class_addmethod(c, (method)mono2k_transport, "transport", A_LONG, 0);
    class_addmethod(c, (method)mono2k_pos, "pos", A_FLOAT, 0);
    class_addmethod(c, (method)mono2k_sync, "sync", A_LONG, 0);
    class_addmethod(c, (method)mono2k_route, "route", A_LONG, 0);
    class_addmethod(c, (method)mono2k_midi, "midi", A_GIMME, 0);
    class_addmethod(c, (method)mono2k_int, "int", A_LONG, 0);
    class_addmethod(c, (method)mono2k_state, "state", A_GIMME, 0);
    class_addmethod(c, (method)mono2k_ink, "ink", A_FLOAT, A_FLOAT, A_FLOAT, 0);
    class_addmethod(c, (method)mono2k_paper, "paper", A_FLOAT, A_FLOAT, A_FLOAT, 0);
    class_dspinit(c);
    class_register(CLASS_BOX, c);
    mono2k_class = c;
    post("mono2k~ (MONO2K for Max for Live) - GPL-3.0 - core by Inigo Hernandez");
}

static void *mono2k_new(t_symbol *s, long argc, t_atom *argv)
{
    t_mono2k *x = (t_mono2k *)object_alloc(mono2k_class);
    if (!x) return NULL;
    dsp_setup((t_pxobject *)x, 1);
    x->out_state = outlet_new(x, NULL);
    x->out_leds = outlet_new(x, NULL);
    x->out_big = outlet_new(x, NULL);
    x->out_small = outlet_new(x, NULL);
    outlet_new(x, "signal");
    outlet_new(x, "signal");
    x->ob.z_misc |= Z_NO_INPLACE;
    critical_new(&x->crit);
    x->qe = qelem_new(x, (method)mono2k_qfn);
    set_col(x->paper, 0.894, 0.882, 0.839);
    set_col(x->ink, 0.090, 0.094, 0.102);
    x->sbuf = (uint8_t *)sysmem_newptr(E_STATE_MAX);
    x->ibuf = (int32_t *)sysmem_newptr(sizeof(int32_t) * e_state_atoms(E_STATE_MAX));
    x->abuf = (t_atom *)sysmem_newptr(sizeof(t_atom) * e_state_atoms(E_STATE_MAX));
    if (!g_owner) {
        g_owner = x;
        x->owner = 1;
        e_init(sys_getsr());
    } else
        object_error((t_object *)x, "only one MONO2K can play per Live set: this one stays silent");
    return x;
}

static void mono2k_free(t_mono2k *x)
{
    dsp_free((t_pxobject *)x);
    qelem_free(x->qe);
    if (x->owner && g_owner == x) g_owner = NULL;
    if (x->mat_small) jit_object_free(x->mat_small);
    if (x->mat_big) jit_object_free(x->mat_big);
    critical_free(x->crit);
    sysmem_freeptr(x->sbuf);
    sysmem_freeptr(x->ibuf);
    sysmem_freeptr(x->abuf);
    if (x->fin) sysmem_freeptr(x->fin);
    if (x->fl) sysmem_freeptr(x->fl);
    if (x->fr) sysmem_freeptr(x->fr);
}

static void mono2k_assist(t_mono2k *x, void *b, long m, long a, char *s)
{
    static const char *outs[] = { "left", "right", "screen 160 x 160 (jit_matrix)", "screen 240 x 240 (jit_matrix)",
                                  "LEDs: 27 keys, 14 buttons", "saved state (to a pattr)" };
    if (m == ASSIST_INLET) sprintf(s, "beat phase (plugphasor~), messages");
    else if (a >= 0 && a < 6) sprintf(s, "%s", outs[a]);
}

/* ------------------------------------------------------------------------------------------------ audio */

static void mono2k_perform64(t_mono2k *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts,
                             long n, long flags, void *userparam)
{
    double *ol = outs[0], *or_ = outs[1];
    if (!x->owner || n > x->fcap) {
        for (long i = 0; i < n; i++) ol[i] = or_[i] = 0;
        return;
    }
    if (x->connected_phase) for (long i = 0; i < n; i++) x->fin[i] = (float)ins[0][i];
    e_process(x->connected_phase ? x->fin : NULL, x->fl, x->fr, (int)n);
    for (long i = 0; i < n; i++) { ol[i] = x->fl[i]; or_[i] = x->fr[i]; }
    uint32_t s = e_frame_seq();
    if (s != x->seen_audio) { x->seen_audio = s; qelem_set(x->qe); }
}

static void mono2k_dsp64(t_mono2k *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags)
{
    if (maxvectorsize > x->fcap) {
        if (x->fin) sysmem_freeptr(x->fin);
        if (x->fl) sysmem_freeptr(x->fl);
        if (x->fr) sysmem_freeptr(x->fr);
        x->fin = (float *)sysmem_newptrclear(sizeof(float) * maxvectorsize);
        x->fl = (float *)sysmem_newptrclear(sizeof(float) * maxvectorsize);
        x->fr = (float *)sysmem_newptrclear(sizeof(float) * maxvectorsize);
        x->fcap = maxvectorsize;
    }
    x->connected_phase = count[0];
    if (x->owner) e_set_sr(samplerate);
    object_method(dsp64, gensym("dsp_add64"), x, mono2k_perform64, 0, NULL);
}

/* ------------------------------------------------------------------------------------------------ the screen */

static void *make_matrix(int w, int h, t_symbol **name)
{
    t_jit_matrix_info info;
    jit_matrix_info_default(&info);
    info.type = gensym("char");
    info.planecount = 4;
    info.dimcount = 2;
    info.dim[0] = w;
    info.dim[1] = h;
    void *m = jit_object_new(gensym("jit_matrix"), &info);
    if (!m) return NULL;
    *name = jit_symbol_unique();
    m = jit_object_register(m, *name);
    return m;
}

static inline int ink_at(const uint8_t *fb, int xx, int yy)
{
    return (fb[yy * (E_W / 8) + (xx >> 3)] >> (7 - (xx & 7))) & 1;
}

static void put_px(unsigned char *p, const float *ink, const float *paper, float c)
{
    p[0] = 255;
    for (int k = 0; k < 3; k++) {
        float v = paper[k] + (ink[k] - paper[k]) * c;
        p[k + 1] = (unsigned char)(v * 255.0f + 0.5f);
    }
}

static void fill_big(t_mono2k *x)
{
    t_jit_matrix_info info;
    char *bp = NULL;
    long lock = (long)jit_object_method(x->mat_big, gensym("lock"), 1);
    jit_object_method(x->mat_big, gensym("getinfo"), &info);
    jit_object_method(x->mat_big, gensym("getdata"), &bp);
    if (bp) {
        for (int yy = 0; yy < E_H; yy++) {
            unsigned char *row = (unsigned char *)bp + yy * info.dimstride[1];
            for (int xx = 0; xx < E_W; xx++) put_px(row + xx * 4, x->ink, x->paper, (float)ink_at(x->fb, xx, yy));
        }
    }
    jit_object_method(x->mat_big, gensym("lock"), lock);
}

/* 240 -> 160: each pixel the share of ink over the 1.5 x 1.5 source pixels it covers (two from three: whole + half,
 * half + whole), so the dithering reads as grey rather than breaking up */
static void fill_small(t_mono2k *x)
{
    static float cov[E_H][SMALL];
    for (int yy = 0; yy < E_H; yy++)
        for (int j = 0; j < SMALL; j++) {
            int m = (j >> 1) * 3;
            cov[yy][j] = (j & 1) ? (0.5f * ink_at(x->fb, m + 1, yy) + ink_at(x->fb, m + 2, yy)) / 1.5f
                                 : (ink_at(x->fb, m, yy) + 0.5f * ink_at(x->fb, m + 1, yy)) / 1.5f;
        }
    t_jit_matrix_info info;
    char *bp = NULL;
    long lock = (long)jit_object_method(x->mat_small, gensym("lock"), 1);
    jit_object_method(x->mat_small, gensym("getinfo"), &info);
    jit_object_method(x->mat_small, gensym("getdata"), &bp);
    if (bp) {
        for (int i = 0; i < SMALL; i++) {
            int m = (i >> 1) * 3;
            unsigned char *row = (unsigned char *)bp + i * info.dimstride[1];
            for (int j = 0; j < SMALL; j++) {
                float c = (i & 1) ? (0.5f * cov[m + 1][j] + cov[m + 2][j]) / 1.5f : (cov[m][j] + 0.5f * cov[m + 1][j]) / 1.5f;
                put_px(row + j * 4, x->ink, x->paper, c);
            }
        }
    }
    jit_object_method(x->mat_small, gensym("lock"), lock);
}

static void mono2k_qfn(t_mono2k *x)
{
    if (!x->owner) return;
    uint32_t s = e_frame_seq();
    if (s != x->seen_main) {
        x->seen_main = s;
        e_frame_copy(x->fb, x->leds);
        if (!x->mat_small) x->mat_small = make_matrix(SMALL, SMALL, &x->name_small);   /* (Jitter loaded by now) */
        if (!x->mat_big) x->mat_big = make_matrix(E_W, E_H, &x->name_big);
        t_atom a;
        if (x->mat_big) {
            fill_big(x);
            atom_setsym(&a, x->name_big);
            outlet_anything(x->out_big, gensym("jit_matrix"), 1, &a);
        }
        if (x->mat_small) {
            fill_small(x);
            atom_setsym(&a, x->name_small);
            outlet_anything(x->out_small, gensym("jit_matrix"), 1, &a);
        }
        int changed = !x->leds_valid;
        for (int i = 0; i < E_LEDS && !changed; i++) changed = x->leds_sent[i] != x->leds[i];
        if (changed) {
            t_atom l[E_LEDS];
            for (int i = 0; i < E_LEDS; i++) { x->leds_sent[i] = x->leds[i]; atom_setlong(l + i, x->leds[i]); }
            x->leds_valid = 1;
            outlet_list(x->out_leds, NULL, E_LEDS, l);
        }
    }
    int n = e_state_take(x->sbuf, E_STATE_MAX);
    if (n > 0) {
        int na = e_state_atoms(n);
        e_state_encode(x->sbuf, n, x->ibuf);
        for (int i = 0; i < na; i++) atom_setlong(x->abuf + i, x->ibuf[i]);
        outlet_anything(x->out_state, gensym("set"), na, x->abuf);
    }
}
