// ma_present -- the frontend's whole presentation policy, in one place.
//
// Design rule for the rewrite (workspace/tmp/minarch-gl-rewrite/PLAN.md,
// priority c > a > b): the software path (a core frame uploaded into a texture)
// and the hardware path (a core-rendered FBO) must both end in the SAME call --
// present this frame -- so present policy and its measurement exist once.
//
// Only SDL: no api.h, no frontend headers, no core knowledge.
//
// There is deliberately ONE present mode.  The vsync/async experiment from the
// archive branch is not carried over: measured on the device, the SDL swap only
// blocks when the producer arrives ahead of the refresh (a slow frame swaps in
// ~0.17ms), raw eglSwapBuffers never blocks but also ignores the swap interval,
// and the A/B showed no frame-rate difference while the mailbox it was meant to
// justify was archived -- it addressed no measured bottleneck.
#ifndef MA_PRESENT_H
#define MA_PRESENT_H

#include <SDL2/SDL.h>
#include <stdint.h>

// Captures the window.  Call once with its GL context current, i.e. straight
// after the context is created.
void MA_present_init(SDL_Window *window);

// Put the frame on the display.  The only swap in the frontend.
void MA_present_frame(void);

// Read-and-clear: did the present block this frame?  Consumed by the pacing
// code exactly like ma_audio_wrote_frame (RetroArch runloop PACE_VSYNC): a
// present that blocked means the display paced this frame, so the timer must
// not be added on top.
int MA_present_blocked(void);

// Telemetry, inert unless the caller logs it: swap-call cost and the
// present-to-present gap distribution.  Taken and reset together.
void MA_present_take_stats(uint64_t *swap_sum_us, uint64_t *swap_max_us, unsigned *swaps,
		unsigned *over_1ms, unsigned *over_half_period,
		unsigned *gaps, uint64_t *gap_min_us,
		unsigned *gap_lt5ms, unsigned *gap_lt12ms, unsigned *gap_lt20ms, unsigned *gap_ge20ms);

#endif
