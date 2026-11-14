/* SPDX-License-Identifier: GPL-3.0-only */
/* The MONO2K core, unchanged, as one translation unit (its own unity build), plus the few views into its statics the
 * Max for Live engine needs. */
#include "m2k_core.c"

int m2k_ext_midi_ch(void) { return midi_ch(); }

/* ------------------------------------------------------------------------------- the panel's knobs, with ends */

/* what each encoder shows as a knob with a start and an end: kind 1 a position 0..range (its value), kind 2 endless
 * (a turn means the next / previous: the catalogue, a held step's tools, the special settings), kind 0 nothing */
enum { XK_NONE, XK_ABS, XK_REL };

static int xk_plain(void)          /* no layer or held step changing what the left encoders do */
{
    return !held(B_GLO) && !(ui.grid && ui.nheld) && !seq_layer() && !cat.open && !step_entry();
}

/* encoder e (E_SELECT .. E_MASTER): its kind, position and range */
void m2k_ext_knob(int e, int *kind, int *pos, int *range)
{
    *kind = XK_NONE; *pos = 0; *range = 0;
    switch (e) {
    case E_SELECT:
        if (!xk_plain() || ui.page == PG_LFO) { *kind = XK_REL; return; }
        *kind = XK_ABS; *pos = ui.trk; *range = M2K_TRACKS - 1; return;
    case E_ALGO:
        if (seq_layer() || (ui.grid && ui.nheld) || cat.open) { *kind = XK_REL; return; }
        *kind = XK_ABS; *pos = m_place(PAT(cur_pat)->machine[ui.trk]); *range = M_COUNT - 1; return;
    case E_PRESET:
        if (!xk_plain() || (ui.page != PG_HOME && ui.page != PG_GLO)) { *kind = XK_REL; return; }
        *kind = XK_ABS; *pos = sq.playing ? sq.queued : cur_pat; *range = M2K_PATTERNS - 1; return;
    case E_MASTER:
        if (ui.grid && ui.nheld) { *kind = XK_REL; return; }
        *kind = XK_ABS; *pos = master_vol; *range = 127; return;
    default: {
        int k = e - E_K1;
        if (k < 0 || k > 3) return;
        int code = knob_at(k);
        if (code == NONE) return;
        const pi_t *pi = pinfo(ui.trk, code);
        if (pi->s4[0] == '-') return;
        if (code >= SETTING(16)) { *kind = XK_REL; return; }       /* (projects, sounds, help: lists that step) */
        int v = knob_value(code), n = kind_choices(pi->kind);
        if (n) { *kind = XK_ABS; *pos = choice_of(pi->kind, v); *range = n - 1; }
        else {
            int lo = kind_min(pi->kind), hi = kind_max(pi->kind);
            *kind = XK_ABS; *pos = CLAMP(v, lo, hi) - lo; *range = hi - lo;
        }
        if (*range <= 0) { *kind = XK_REL; *range = 0; }
        if (*pos > *range) *pos = *range;
        if (*pos < 0) *pos = 0;
        return;
    }
    }
}

/* turn encoder e so its knob lands on position POS: as turns of the right size (each turn x1, never sped up) */
void m2k_ext_knob_to(int e, int pos)
{
    int kind, cur, range;
    m2k_ext_knob(e, &kind, &cur, &range);
    if (kind != XK_ABS) return;
    pos = CLAMP(pos, 0, range);
    int d = pos - cur;
    if (!d) return;
    if (e == E_SELECT || e == E_ALGO || e == E_PRESET) {     /* (these move one a turn, whatever its size) */
        for (int i = 0; i < (d < 0 ? -d : d); i++) m2k_encoder(e, d < 0 ? -1 : 1);
        return;
    }
    if (e >= E_K1 && e <= E_K4) ui.kn_t[e - E_K1] = ui.now;   /* (no time since the last turn: knob_speed takes it as is) */
    m2k_encoder(e, d);
}

int m2k_ext_step(void) { return sq.step; }     /* (tests) */
int m2k_ext_len(void) { return seq_len(); }     /* (tests) */
unsigned m2k_ext_pulses(void) { return mx.pulses; }     /* (tests) */
