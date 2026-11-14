/* SPDX-License-Identifier: GPL-3.0-only */
/* MONO2K for Max for Live: the engine layer between the MONO2K core and the Max object.
 * No Max code here, so it can be tested on its own (tests/engine_test.c).
 *   - an event queue: presses, turns and MIDI from Max's threads, played into the core on the audio thread
 *   - Live's clock: a beat-phase signal turned into MIDI clock pulses, Live's play and stop into start and stop
 *   - a resampler: the core runs at 44.1 kHz, Live at whatever rate it likes
 *   - the screen, the LEDs and the saved state, published for the Max thread to read
 * The core is a set of statics, so there is one engine per process. */
#ifndef M2K_ENGINE_H
#define M2K_ENGINE_H

#include <stdint.h>

#define E_W 240
#define E_H 240
#define E_FB_BYTES (E_W * E_H / 8)
#define E_LEDS 41                 /* 27 keys, then 14 buttons */
#define E_KNOBS 8                 /* the encoders as knobs: kind, position, range each (m2k_ext_knob) */
#define E_STATE_MAX (900 * 1024)

enum { EV_BTN, EV_KEY, EV_ENC, EV_MIDI, EV_TRANSPORT, EV_POS, EV_SYNC, EV_ROUTE, EV_KNOB, EV_COUNT };

void e_init(double sr);
void e_set_sr(double sr);
/* from any thread except the audio thread; callers must not push from two threads at once (the Max binding serialises
 * with a critical section). a, b: EV_BTN/EV_KEY index, down; EV_ENC encoder, delta; EV_MIDI status, d1 | d2 << 8;
 * EV_KNOB encoder, position (lands its knob there); EV_TRANSPORT playing; EV_POS Live's song time in 1/1000 beats; EV_SYNC on; EV_ROUTE 0 the selected track, 1..6 */
int e_push(int kind, int a, int b);
/* the audio thread: phase is Live's beat phase (0..1 a beat, plugphasor~), or NULL */
void e_process(const float *phase, float *out_l, float *out_r, int n);

/* published for the Max thread */
uint32_t e_frame_seq(void);
void e_frame_copy(uint8_t *fb, uint8_t *leds, int16_t *knobs);   /* the latest screen (1 bit, MSB first, 1 ink), LEDs,
                                                                 * knobs (E_KNOBS x kind, position, range; may be NULL) */
/* the saved state: a new one since the last take? copies it and returns its length, else 0 */
int e_state_take(uint8_t *dst, int max);
/* a saved state to load (from the Live set): played into the core on the audio thread */
void e_state_put(const uint8_t *src, int len);
int e_dropped(void);

/* the encoding the Live set keeps: 3 bytes an integer (exact in a float32), a header of the length and a checksum */
int e_state_atoms(int len);                                  /* the integers it takes */
void e_state_encode(const uint8_t *src, int len, int32_t *dst);
int e_state_decode(const int32_t *src, int n, uint8_t *dst, int max);   /* the length, or -1: not ours or damaged */

#endif
