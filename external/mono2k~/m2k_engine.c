/* SPDX-License-Identifier: GPL-3.0-only */
/* MONO2K for Max for Live: the engine layer (see m2k_engine.h). */
#include <string.h>
#include <math.h>
#include "m2k.h"
#include "m2k_engine.h"

int m2k_ext_midi_ch(void);        /* m2k_core_glue.c */

#define LD(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define ST(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)

/* ------------------------------------------------------------------------------------------------ the queue */

#define QN 1024u
static struct {
    uint8_t kind[QN];
    int32_t a[QN], b[QN];
    uint32_t w, r;                /* w written by the producers (serialised), r by the audio thread */
    uint32_t dropped;
} q;

int e_push(int kind, int a, int b)
{
    uint32_t w = q.w, r = LD(&q.r);
    if (w - r >= QN) { q.dropped++; return 0; }
    q.kind[w % QN] = (uint8_t)kind;
    q.a[w % QN] = a;
    q.b[w % QN] = b;
    ST(&q.w, w + 1);
    return 1;
}

int e_dropped(void) { return (int)q.dropped; }

/* ------------------------------------------------------------------------------------------------ the state */

static struct {
    double sr, ratio, frac;       /* Live's rate; core samples per Live sample; the read position between hist[1], [2] */
    float hist[4][2];             /* the last four core frames, oldest first */
    int16_t blk[M2K_SUB * 2];     /* the core's current block */
    int bpos;                     /* frames of blk used */
    uint64_t core_n;              /* core frames rendered */
    /* Live's clock */
    int sync, route;
    int playing;                  /* Live's transport, as last told */
    int started;                  /* MONO2K started on it */
    int want_start, want_stop;
    int32_t pos_milli;            /* Live's song time when it started, 1/1000 beats; -1 none */
    float last_ph;
    int last_idx;
    uint32_t still;               /* samples the phase has not moved */
    uint32_t moved_at;            /* samples since the phase began to move again */
    int moved_pulses;             /* pulse crossings since then */
    int pending;                  /* pulses waiting for the next core block */
    /* the screen */
    uint32_t next_frame;          /* core frames to the next drawing */
    /* the state */
    uint32_t next_pack;
    uint32_t last_hash;
} e;

static uint8_t fb[2][E_FB_BYTES];
static uint8_t leds[2][E_LEDS];
static uint32_t fseq;

static uint8_t st_buf[E_STATE_MAX];
static int st_len;
static uint32_t st_seq, st_taken;

static uint8_t rs_buf[E_STATE_MAX];
static int rs_len, rs_flag;

#define FRAME_EVERY 735u          /* core frames: 60 drawings a second (the core's held-button timing reads them) */
#define PACK_EVERY (M2K_SR * 2u)  /* the state looked at every 2 s */

static uint32_t fnv(const uint8_t *p, int n)
{
    uint32_t h = 2166136261u;
    while (n-- > 0) { h ^= *p++; h *= 16777619u; }
    return h ? h : 1u;
}

/* the settings the device owns, put back after any load: the clock in from Live (USB's) or none, nothing out */
static void apply_settings(void)
{
    m2k_setting(S_SYNI, e.sync ? 48 : 0);
    m2k_setting(S_SYNO, 0);
    m2k_setting(S_NOTO, 0);
    m2k_setting(S_NTIN, 0);
    m2k_setting(S_THRU, 0);
}

void e_set_sr(double sr)
{
    if (sr < 8000) sr = 44100;
    e.sr = sr;
    e.ratio = (double)M2K_SR / sr;
}

void e_init(double sr)
{
    memset(&e, 0, sizeof e);
    memset(&q, 0, sizeof q);
    m2k_init(0x4D324B21u);
    e.sync = 1;
    e.pos_milli = -1;
    e.bpos = M2K_SUB;             /* the first block rendered at the first sample */
    e.next_pack = M2K_SR;         /* the first look a second in */
    e_set_sr(sr);
    apply_settings();
}

/* ------------------------------------------------------------------------------------------------ events */

static void midi_in(int st, int d1, int d2)
{
    if (st < 0xF0 && st >= 0x80) {    /* a channel's message: to the track the device picks */
        int base = m2k_ext_midi_ch();
        int ch = e.route >= 1 && e.route <= M2K_TRACKS ? (base + e.route - 1) & 15 : (base + 8) & 15;   /* 8: past the tracks, the one in view */
        st = (st & 0xF0) | ch;
    }
    m2k_midi(st, d1 & 127, d2 & 127);
}

static void drain(void)
{
    uint32_t r = q.r, w = LD(&q.w);
    for (; r != w; r++) {
        int k = q.kind[r % QN], a = q.a[r % QN], b = q.b[r % QN];
        switch (k) {
        case EV_BTN: if (a >= 0 && a < B_COUNT) m2k_button(a, b != 0); break;
        case EV_KEY: if (a >= 0 && a < M2K_KEYS) m2k_key(a, b != 0); break;
        case EV_ENC: if (a >= 0 && a < E_COUNT && b) m2k_encoder(a, b); break;
        case EV_MIDI: midi_in(a & 255, b & 127, (b >> 8) & 127); break;
        case EV_POS: e.pos_milli = b; break;
        case EV_TRANSPORT:
            if (a && !e.playing) { e.want_start = 1; e.want_stop = 0; }
            if (!a && e.playing) { e.want_stop = 1; e.want_start = 0; }
            e.playing = a != 0;
            break;
        case EV_SYNC:
            if ((a != 0) != e.sync) {
                if (e.started) { m2k_midi(0xFC, 0, 0); e.started = 0; }
                e.sync = a != 0;
                apply_settings();
                if (e.sync && e.playing) e.want_start = 1;
            }
            break;
        case EV_ROUTE: e.route = a < 0 ? 0 : a > M2K_TRACKS ? M2K_TRACKS : a; break;
        }
    }
    ST(&q.r, r);
}

/* ------------------------------------------------------------------------------------------------ the clock */

/* each Live sample: the beat phase's 1/24 crossings, counted (MIDI clock is 24 a beat) */
static void clock_sample(float ph)
{
    if (ph < 0) ph = 0;
    if (ph >= 1) ph = ph - floorf(ph);
    int idx = (int)(ph * 24.0f);
    if (idx > 23) idx = 23;
    if (ph == e.last_ph) {
        if (e.still < 0x7FFFFFFFu) e.still++;
        return;
    }
    if (e.still > 2048u) { e.moved_at = 0; e.moved_pulses = 0; }   /* moving again after a stop */
    e.still = 0;
    int d = idx - e.last_idx;
    if (ph < e.last_ph - 0.5f) d += 24;        /* through the beat */
    e.last_ph = ph;
    e.last_idx = idx;
    if (d <= 0 || d > 4) return;               /* a jump (a loop, a relocation): followed from here, nothing played */
    e.moved_pulses += d;
    if (e.started) e.pending += d;
}

/* before each core block: Live's start and stop, then at most one pulse (a catch-up spreads over the blocks) */
static void clock_block(void)
{
    if (!e.sync) { e.want_start = e.want_stop = 0; e.pending = 0; return; }
    if (e.want_stop) {
        e.want_stop = 0;
        if (e.started) m2k_midi(0xFC, 0, 0);
        e.started = 0;
        e.pending = 0;
    }
    if (e.want_start) {
        e.want_start = 0;
        /* the crossings since the phase began to move (if it did just now) are played as a catch-up, and the song
         * position is where Live started: its time now less them, to the nearest step */
        int caught = e.moved_at < (uint32_t)(e.sr * 0.5) ? e.moved_pulses : 0;
        long steps = 0;
        if (e.pos_milli >= 0) {
            /* Live's time when told (a little after it started): the step it started on, nearest */
            double now_pulses = e.pos_milli * 24.0 / 1000.0;
            steps = (long)floor((now_pulses - caught) / 6.0 + 0.5);
            if (steps < 0) steps = 0;
        }
        /* the pulses gone by since that step, from the beat phase now (exact, whenever the song time was read) */
        {
            int behind = ((e.last_idx - (int)((steps * 6) % 24)) % 24 + 24) % 24;
            if (behind > 12) behind -= 24;            /* before it: the read was late and rounded up a step */
            while (behind < 0 && steps > 0) { steps--; behind += 6; }
            if (behind < 0) behind = 0;
            steps += behind / 6;                       /* past the next: on from the step now under way */
            caught = behind % 6;
        }
        steps &= 0x3FFF;
        if (e.started) m2k_midi(0xFC, 0, 0);
        if (steps) {
            m2k_midi(0xF2, (int)(steps & 127), (int)(steps >> 7));
            m2k_midi(0xFB, 0, 0);
        } else
            m2k_midi(0xFA, 0, 0);
        e.started = 1;
        e.pending = 1 + caught;                /* the start's own pulse, then the ones gone by */
        e.pos_milli = -1;
    }
    if (e.started && e.pending > 0) {
        int n = e.pending > 3 ? 2 : 1;         /* (behind: two a block until caught up) */
        for (int i = 0; i < n; i++) m2k_midi(0xF8, 0, 0);
        e.pending -= n;
    }
}

/* ------------------------------------------------------------------------------------------------ publishing */

static void publish_frame(void)
{
    uint32_t ms = (uint32_t)(e.core_n * 1000u / M2K_SR);
    const uint8_t *f = m2k_frame(ms ? ms : 1);
    const uint8_t *l = m2k_leds();
    uint32_t s = fseq + 1;
    memcpy(fb[s & 1], f, E_FB_BYTES);
    memcpy(leds[s & 1], l, E_LEDS);
    ST(&fseq, s);
}

uint32_t e_frame_seq(void) { return LD(&fseq); }

void e_frame_copy(uint8_t *dfb, uint8_t *dleds)
{
    uint32_t s = LD(&fseq);
    memcpy(dfb, fb[s & 1], E_FB_BYTES);
    memcpy(dleds, leds[s & 1], E_LEDS);
}

static void look_at_state(void)
{
    int n = m2k_store_pack();
    if (n <= 0 || n > E_STATE_MAX) return;
    int mx;
    const uint8_t *p = m2k_store_buf(&mx);
    uint32_t h = fnv(p, n);
    if (h == e.last_hash) return;
    if (LD(&st_seq) != LD(&st_taken)) return;   /* the last not taken yet: next time */
    e.last_hash = h;
    memcpy(st_buf, p, (size_t)n);
    st_len = n;
    ST(&st_seq, st_seq + 1);
}

int e_state_take(uint8_t *dst, int max)
{
    uint32_t s = LD(&st_seq);
    if (s == LD(&st_taken)) return 0;
    int n = st_len;
    if (n > max) n = 0;
    else memcpy(dst, st_buf, (size_t)n);
    ST(&st_taken, s);
    return n;
}

void e_state_put(const uint8_t *src, int len)
{
    if (len <= 0 || len > E_STATE_MAX || LD(&rs_flag)) return;
    memcpy(rs_buf, src, (size_t)len);
    rs_len = len;
    ST(&rs_flag, 1);
}

static void restore_if_asked(void)
{
    if (!LD(&rs_flag)) return;
    int mx;
    uint8_t *p = m2k_store_buf(&mx);
    if (rs_len <= mx) {
        memcpy(p, rs_buf, (size_t)rs_len);
        if (m2k_store_unpack(rs_len)) {
            e.started = 0;
            apply_settings();
            e.last_hash = fnv(rs_buf, rs_len);   /* (the settings put back may differ: then it is saved once) */
            if (e.sync && e.playing) e.want_start = 1;
        }
    }
    ST(&rs_flag, 0);
}

/* ------------------------------------------------------------------------------------------------ audio */

static void core_block(void)
{
    drain();
    restore_if_asked();
    clock_block();
    m2k_render(e.blk, M2K_SUB);
    e.core_n += M2K_SUB;
    if (e.next_frame <= M2K_SUB) { publish_frame(); e.next_frame += FRAME_EVERY; }
    e.next_frame -= M2K_SUB;
    if (e.next_pack <= M2K_SUB) { look_at_state(); e.next_pack += PACK_EVERY; }
    e.next_pack -= M2K_SUB;
    e.bpos = 0;
}

static void push_core_frame(void)
{
    if (e.bpos >= M2K_SUB) core_block();
    memmove(e.hist[0], e.hist[1], sizeof e.hist[0] * 3);
    e.hist[3][0] = e.blk[e.bpos * 2] * (1.0f / 32768.0f);
    e.hist[3][1] = e.blk[e.bpos * 2 + 1] * (1.0f / 32768.0f);
    e.bpos++;
}

static inline float hermite(float x0, float x1, float x2, float x3, float t)
{
    float c1 = 0.5f * (x2 - x0);
    float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
    float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
    return ((c3 * t + c2) * t + c1) * t + x1;
}

void e_process(const float *phase, float *out_l, float *out_r, int n)
{
    for (int i = 0; i < n; i++) {
        if (phase) clock_sample(phase[i]);
        if (e.moved_at < 0x7FFFFFFFu) e.moved_at++;
        e.frac += e.ratio;
        while (e.frac >= 1.0) { e.frac -= 1.0; push_core_frame(); }
        float t = (float)e.frac;
        out_l[i] = hermite(e.hist[0][0], e.hist[1][0], e.hist[2][0], e.hist[3][0], t);
        out_r[i] = hermite(e.hist[0][1], e.hist[1][1], e.hist[2][1], e.hist[3][1], t);
    }
}

/* ------------------------------------------------------------------------------------------------ the encoding */

#define ST_MAGIC 0x4D324Bu        /* "M2K" */

int e_state_atoms(int len) { return 3 + (len + 2) / 3; }

void e_state_encode(const uint8_t *src, int len, int32_t *dst)
{
    uint32_t h = fnv(src, len);
    dst[0] = ST_MAGIC;
    dst[1] = len;
    dst[2] = (int32_t)(h & 0xFFFFFFu);
    for (int i = 0, k = 3; i < len; i += 3, k++) {
        int32_t v = src[i] << 16;
        if (i + 1 < len) v |= src[i + 1] << 8;
        if (i + 2 < len) v |= src[i + 2];
        dst[k] = v;
    }
}

int e_state_decode(const int32_t *src, int n, uint8_t *dst, int max)
{
    if (n < 3 || src[0] != ST_MAGIC) return -1;
    int len = src[1];
    if (len <= 0 || len > max || n < e_state_atoms(len)) return -1;
    for (int i = 0, k = 3; i < len; i += 3, k++) {
        uint32_t v = (uint32_t)src[k];
        dst[i] = (uint8_t)(v >> 16);
        if (i + 1 < len) dst[i + 1] = (uint8_t)(v >> 8);
        if (i + 2 < len) dst[i + 2] = (uint8_t)v;
    }
    if ((int32_t)(fnv(dst, len) & 0xFFFFFFu) != src[2]) return -1;
    return len;
}
