/* SPDX-License-Identifier: GPL-3.0-only */
/* The MONO2K core, unchanged, as one translation unit (its own unity build), plus the few views into its statics the
 * Max for Live engine needs. */
#include "m2k_core.c"

int m2k_ext_midi_ch(void) { return midi_ch(); }
int m2k_ext_step(void) { return sq.step; }     /* (tests) */
int m2k_ext_len(void) { return seq_len(); }     /* (tests) */
unsigned m2k_ext_pulses(void) { return mx.pulses; }     /* (tests) */
