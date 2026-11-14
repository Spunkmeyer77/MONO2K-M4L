/* SPDX-License-Identifier: GPL-3.0-only */
/* mono2k~: MONO2K (Inigo Hernandez's Monomachine-style instrument, github.com/InigoHernandez/mono2k) as a Max object
 * for Max for Live. The core runs unchanged; this object plays it, feeds it Live's clock and MIDI, and shows its screen.
 *
 * inlet:   signal: Live's beat phase (plugphasor~), and every message
 * outlets: left, right audio; the screen ("scr" + 1920 integers: 240 rows of 8 x 30 bits, MSB first, 1 ink, for the
 *          screen jsui, which draws it at the display's own pixels); (unused); the LEDs (list of 41: 27 keys then 14 buttons, 0..255) and "knobs" + 8 x (kind,
 *          position, range); the saved state ("set ..." for a [pattr] kept in the Live set); "dial i value" (Push's
 *          dials shown the knobs' values, for live.dial's set)
 * messages: btn i down | key i down | enc i delta | knob i position | encabs i value (0..127, a dial) | midi byte... | transport 0/1 | pos beats | sync 0/1 |
 *           route 0..6 (0 the track in view) | state ... (a saved state back) */
#include <string.h>
#include "ext.h"
#include "ext_obex.h"
#include "ext_critical.h"
#include "z_dsp.h"
#include "m2k_engine.h"

/* clang-cl says it is MSVC (_MSC_VER) but its preprocessor is the standard one, so the SDK's MSVC form of C74_VARFUN
 * (object_method, jit_object_new, jit_object_method...) leaves C74_VARFUN_n unexpanded: use its standard form */
#if defined(_MSC_VER) && defined(__clang__)
#undef C74_VARFUN
#define C74_VARFUN(VARFUN_IMPL, ...) C74_JOIN_2(C74_VARFUN_, C74_NUM_ARGS(__VA_ARGS__))( VARFUN_IMPL, __VA_ARGS__ )
#endif

#ifndef M2K_EXT_NAME
#define M2K_EXT_NAME "mono2k~"
#endif
#ifndef M2K_VERSION
#define M2K_VERSION "dev"
#endif

#define SMALL 320                 /* a 160-point view on a Retina screen: its pixels, so nothing scales it again */

typedef struct _mono2k {
    t_pxobject ob;
    void *out_small, *out_big, *out_leds, *out_state, *out_dial;
    void *qe;
    t_critical crit;
    long owner;                       /* 1: this one plays the core (one per Live set) */
    uint32_t seen_audio, seen_main;
    uint8_t fb[E_FB_BYTES], fb_sent[E_FB_BYTES];
    long fb_valid;
    double fb_t;                      /* when the screen was last sent (at most 30 a second) */
    uint8_t leds[E_LEDS];
    long leds_sent[E_LEDS];
    long leds_valid;
    int16_t knobs[E_KNOBS * 3], knobs_sent[E_KNOBS * 3];
    long knobs_valid;
    long dial_sent[8];
    double dial_touch[8];             /* when Push last turned dial i (ms): its value then not set back for a moment */
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
    void *claim_clock;                /* a silent one checks now and then whether it can play (the other gone) */
    double sr;
    int32_t *pend;                    /* a saved state that came while silent: loaded once it plays */
    long pend_n;
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
/* a knob with ends: encoder i to position pos (0 .. its range) */
static void mono2k_knob(t_mono2k *x, long i, long pos) { push(x, EV_KNOB, (int)i, (int)pos); }

/* a dial's value 0..127 (a Live parameter, so Push's knobs reach it): the knob to the same share of its range; an
 * endless one (a list that steps) turned by the change */
static void mono2k_encabs(t_mono2k *x, long i, long v)
{
    if (i < 0 || i > 7) return;
    long last = x->dial_sent[i];
    x->dial_sent[i] = v;
    x->dial_touch[i] = gettime();
    int kind = x->knobs[i * 3], range = x->knobs[i * 3 + 2];
    if (kind == 1 && range > 0) push(x, EV_KNOB, (int)i, (int)((v * range + 63) / 127));
    else if (kind == 2 && v != last) push(x, EV_ENC, (int)i, v > last ? 1 : -1);
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
    if (argc < 3) return;
    long n = argc > e_state_atoms(E_STATE_MAX) ? e_state_atoms(E_STATE_MAX) : argc;
    if (!x->owner) {                  /* silent for now: kept for when it plays */
        for (long i = 0; i < n; i++) x->pend[i] = (int32_t)atom_getlong(argv + i);
        x->pend_n = n;
        return;
    }
    for (long i = 0; i < n; i++) x->ibuf[i] = (int32_t)atom_getlong(argv + i);
    int len = e_state_decode(x->ibuf, (int)n, x->sbuf, E_STATE_MAX);
    if (len > 0) e_state_put(x->sbuf, len);
    else object_warn((t_object *)x, "saved state not recognised: starting from the factory patterns");
}

void ext_main(void *r)
{
    t_class *c = class_new(M2K_EXT_NAME, (method)mono2k_new, (method)mono2k_free, sizeof(t_mono2k), 0L, A_GIMME, 0);
    class_addmethod(c, (method)mono2k_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)mono2k_assist, "assist", A_CANT, 0);
    class_addmethod(c, (method)mono2k_btn, "btn", A_LONG, A_LONG, 0);
    class_addmethod(c, (method)mono2k_key, "key", A_LONG, A_LONG, 0);
    class_addmethod(c, (method)mono2k_enc, "enc", A_LONG, A_LONG, 0);
    class_addmethod(c, (method)mono2k_encabs, "encabs", A_LONG, A_LONG, 0);
    class_addmethod(c, (method)mono2k_knob, "knob", A_LONG, A_LONG, 0);
    class_addmethod(c, (method)mono2k_transport, "transport", A_LONG, 0);
    class_addmethod(c, (method)mono2k_pos, "pos", A_FLOAT, 0);
    class_addmethod(c, (method)mono2k_sync, "sync", A_LONG, 0);
    class_addmethod(c, (method)mono2k_route, "route", A_LONG, 0);
    class_addmethod(c, (method)mono2k_midi, "midi", A_GIMME, 0);
    class_addmethod(c, (method)mono2k_int, "int", A_LONG, 0);
    class_addmethod(c, (method)mono2k_state, "state", A_GIMME, 0);
    class_dspinit(c);
    class_register(CLASS_BOX, c);
    mono2k_class = c;
    post("%s: MONO2K for Max for Live v%s - GPL-3.0 - core by Inigo Hernandez", M2K_EXT_NAME, M2K_VERSION);
}

/* the core is one per process: the first MONO2K plays it. Another (a second device, or the copy a freeze makes while
 * the old one is still loaded) waits and takes it over once it is free, with the state it was given meanwhile */
static int mono2k_try_claim(t_mono2k *x)
{
    if (x->owner) return 1;
    if (g_owner) return 0;
    g_owner = x;
    x->owner = 1;
    e_init(x->sr > 0 ? x->sr : 44100);
    x->seen_main = x->seen_audio = 0;
    x->leds_valid = x->knobs_valid = x->fb_valid = 0;
    if (x->pend_n >= 3) {
        int len = e_state_decode(x->pend, (int)x->pend_n, x->sbuf, E_STATE_MAX);
        if (len > 0) e_state_put(x->sbuf, len);
        x->pend_n = 0;
    }
    return 1;
}

static void mono2k_claim(t_mono2k *x)
{
    if (!mono2k_try_claim(x)) clock_fdelay(x->claim_clock, 250);
    else post("mono2k~: playing");
}

static void *mono2k_new(t_symbol *s, long argc, t_atom *argv)
{
    t_mono2k *x = (t_mono2k *)object_alloc(mono2k_class);
    if (!x) return NULL;
    dsp_setup((t_pxobject *)x, 1);
    x->out_dial = outlet_new(x, NULL);
    x->out_state = outlet_new(x, NULL);
    x->out_leds = outlet_new(x, NULL);
    x->out_big = outlet_new(x, NULL);
    x->out_small = outlet_new(x, NULL);
    outlet_new(x, "signal");
    outlet_new(x, "signal");
    x->ob.z_misc |= Z_NO_INPLACE;
    critical_new(&x->crit);
    x->qe = qelem_new(x, (method)mono2k_qfn);
    x->sbuf = (uint8_t *)sysmem_newptr(E_STATE_MAX);
    x->ibuf = (int32_t *)sysmem_newptr(sizeof(int32_t) * e_state_atoms(E_STATE_MAX));
    x->abuf = (t_atom *)sysmem_newptr(sizeof(t_atom) * e_state_atoms(E_STATE_MAX));
    x->pend = (int32_t *)sysmem_newptr(sizeof(int32_t) * e_state_atoms(E_STATE_MAX));
    x->sr = sys_getsr();
    x->claim_clock = clock_new(x, (method)mono2k_claim);
    if (!mono2k_try_claim(x)) {
        object_warn((t_object *)x, "another MONO2K is playing: this one waits, and plays once that one is gone");
        clock_fdelay(x->claim_clock, 250);
    }
    return x;
}

static void mono2k_free(t_mono2k *x)
{
    dsp_free((t_pxobject *)x);
    qelem_free(x->qe);
    clock_unset(x->claim_clock);
    object_free(x->claim_clock);
    if (x->owner && g_owner == x) g_owner = NULL;
    sysmem_freeptr(x->pend);
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
    static const char *outs[] = { "left", "right", "screen (scr + 1920 ints, to the screen jsui)", "(unused)",
                                  "LEDs (list), knobs", "saved state (to a pattr)", "dial i value (to Push's dials)" };
    if (m == ASSIST_INLET) sprintf(s, "beat phase (plugphasor~), messages");
    else if (a >= 0 && a < 7) sprintf(s, "%s", outs[a]);
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
    x->sr = samplerate;
    if (!x->owner) mono2k_try_claim(x);
    if (x->owner) e_set_sr(samplerate);
    object_method(dsp64, gensym("dsp_add64"), x, mono2k_perform64, 0, NULL);
}

/* ------------------------------------------------------------------------------------------------ the screen */

/* the frame as 240 rows of 8 integers of 30 bits (MSB the leftmost dot), exact in any number Max or JavaScript keeps */
static void send_screen(t_mono2k *x)
{
    static t_atom a[E_H * 8];
    for (int y = 0; y < E_H; y++)
        for (int w = 0; w < 8; w++) {
            long v = 0;
            for (int b = 0; b < 30; b++) {
                int xx = w * 30 + b;
                v = (v << 1) | ((x->fb[y * (E_W / 8) + (xx >> 3)] >> (7 - (xx & 7))) & 1);
            }
            atom_setlong(a + y * 8 + w, v);
        }
    outlet_anything(x->out_small, gensym("scr"), E_H * 8, a);
}

static void mono2k_qfn(t_mono2k *x)
{
    if (!x->owner) return;
    uint32_t s = e_frame_seq();
    if (s != x->seen_main) {
        x->seen_main = s;
        e_frame_copy(x->fb, x->leds, x->knobs);
        double now0 = gettime();
        if ((!x->fb_valid || memcmp(x->fb, x->fb_sent, E_FB_BYTES)) && now0 - x->fb_t >= 32) {
            memcpy(x->fb_sent, x->fb, E_FB_BYTES);
            x->fb_valid = 1;
            x->fb_t = now0;
            send_screen(x);
        }
        int changed = !x->leds_valid;
        for (int i = 0; i < E_LEDS && !changed; i++) changed = x->leds_sent[i] != x->leds[i];
        if (changed) {
            t_atom l[E_LEDS];
            for (int i = 0; i < E_LEDS; i++) { x->leds_sent[i] = x->leds[i]; atom_setlong(l + i, x->leds[i]); }
            x->leds_valid = 1;
            outlet_list(x->out_leds, NULL, E_LEDS, l);
        }
        int kch = !x->knobs_valid;
        for (int i = 0; i < E_KNOBS * 3 && !kch; i++) kch = x->knobs_sent[i] != x->knobs[i];
        if (kch) {
            t_atom k[E_KNOBS * 3];
            for (int i = 0; i < E_KNOBS * 3; i++) { x->knobs_sent[i] = x->knobs[i]; atom_setlong(k + i, x->knobs[i]); }
            x->knobs_valid = 1;
            outlet_anything(x->out_leds, gensym("knobs"), E_KNOBS * 3, k);
        }
        /* Push's dials shown the knobs' values (not while Push turns one, so it is not fought) */
        double now = gettime();
        for (int i = 0; i < 8; i++) {
            int kind = x->knobs[i * 3], pos = x->knobs[i * 3 + 1], range = x->knobs[i * 3 + 2];
            if (kind != 1 || range <= 0 || now - x->dial_touch[i] < 400) continue;
            long v = (pos * 127 + range / 2) / range;
            if (v == x->dial_sent[i]) continue;
            x->dial_sent[i] = v;
            t_atom a2[2];
            atom_setlong(a2, i);
            atom_setlong(a2 + 1, v);
            outlet_anything(x->out_dial, gensym("dial"), 2, a2);
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
