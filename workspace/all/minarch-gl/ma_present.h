// ma_present -- the frontend's whole presentation policy, in one place.
//
// Design rule for the rewrite (workspace/tmp/minarch-gl-rewrite/PLAN.md,
// priority c > a > b): the software path (a core frame uploaded into a
// texture) and the hardware path (a core-rendered FBO) must both end in the
// SAME call -- present this texture -- so present policy, the vsync/async
// choice and its telemetry exist exactly once.
//
// Nothing here knows about cores, shaders or minarch's option tables: it only
// owns the window, the EGL handles and the swap.
#ifndef MA_PRESENT_H
#define MA_PRESENT_H

#include <SDL2/SDL.h>
#include <stdint.h>

// Presentation policy (hidden config key minarch_present, env MINARCH_PRESENT).
// MA_PRESENT_VSYNC is the default and the only mode whose output is known to be
// latched at a refresh boundary; MA_PRESENT_ASYNC swaps with raw
// eglSwapBuffers, which on the H700 driver never waits and ignores the swap
// interval (measured ~8us vs a full 16.1ms block at 60fps).
#define MA_PRESENT_VSYNC 0
#define MA_PRESENT_ASYNC 1

// Captures the display/window/surface handles.  Call once, with the window's
// GL context current (i.e. straight after the context is created), because EGL
// reports the draw surface of the CALLING thread.
void MA_present_init(SDL_Window *window);

// "vsync" | "async"; anything else is ignored (and logged).  Env
// MINARCH_PRESENT takes effect at init; the config layer may call this later to
// override it, so a per-core cfg can still choose.
void MA_present_set_mode(const char *name);
int  MA_present_mode(void);

// Put the frame on the display.  The only swap in the frontend.
void MA_present_frame(void);

// Read-and-clear: did the present block this frame?  Consumed by the pacing
// code exactly like ma_audio_wrote_frame (RetroArch PACE_VSYNC).
int MA_present_blocked(void);

// Telemetry (inert unless the caller logs it): swap-call cost and the
// present-to-present gap.  Taken and reset together.
void MA_present_take_stats(uint64_t *swap_sum_us, uint64_t *swap_max_us, unsigned *swaps,
		unsigned *over_1ms, unsigned *over_half_period,
		unsigned *gaps, uint64_t *gap_min_us,
		unsigned *gap_lt5ms, unsigned *gap_lt12ms, unsigned *gap_lt20ms, unsigned *gap_ge20ms);

#endif
