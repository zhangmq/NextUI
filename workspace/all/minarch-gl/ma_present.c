// See ma_present.h.  One swap for the whole frontend, one place that knows the
// present policy, one place that measures it.
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>

#include "ma_present.h"

// Minimal EGL declarations: the toolchain's EGL headers predate the constants
// used here and would drag in a second set of typedefs.
typedef void *EGLDisplay; typedef void *EGLSurface;
typedef unsigned int EGLBoolean;
#define EGL_DRAW 0x3059

static int ma_mode = MA_PRESENT_VSYNC;
static SDL_Window *ma_window = NULL;
static void *ma_display = NULL;
static void *ma_surface = NULL;
static EGLBoolean (*ma_eglSwapBuffers)(EGLDisplay, EGLSurface) = NULL;
static uint64_t ma_freq = 0;

// Telemetry, see ma_present.h.
static uint64_t st_swap_sum = 0, st_swap_max = 0, st_gap_last = 0, st_gap_min = 0;
static unsigned st_swaps = 0, st_over_1ms = 0, st_over_half = 0;
static unsigned st_gaps = 0, st_gap_lt5 = 0, st_gap_lt12 = 0, st_gap_lt20 = 0, st_gap_ge20 = 0;
static volatile int st_blocked = 0;

int MA_present_mode(void) { return ma_mode; }

void MA_present_set_mode(const char *name) {
	if (!name) return;
	if      (!strcasecmp(name, "vsync")) ma_mode = MA_PRESENT_VSYNC;
	else if (!strcasecmp(name, "async")) ma_mode = MA_PRESENT_ASYNC;
	else SDL_Log("minarch: unknown present mode '%s'; keeping %s", name,
			ma_mode == MA_PRESENT_ASYNC ? "async" : "vsync");
}

void MA_present_init(SDL_Window *window) {
	ma_window = window;
	void *(*get_display)(void) = (void *)SDL_GL_GetProcAddress("eglGetCurrentDisplay");
	void *(*get_surface)(int)  = (void *)SDL_GL_GetProcAddress("eglGetCurrentSurface");
	ma_eglSwapBuffers = (void *)SDL_GL_GetProcAddress("eglSwapBuffers");
	if (get_display) ma_display = get_display();
	// The caller must already have the window context current: EGL reports the
	// draw surface of the calling thread.
	if (get_surface) ma_surface = get_surface(EGL_DRAW);
	ma_freq = SDL_GetPerformanceFrequency();

	// env beats the config file (the launcher's environment carries it); the
	// config layer may call MA_present_set_mode later to override.
	const char *env = getenv("MINARCH_PRESENT");
	if (env && *env) MA_present_set_mode(env);

	SDL_Log("minarch: present %s (window=%p display=%p surface=%p eglSwapBuffers=%p)",
			ma_mode == MA_PRESENT_ASYNC ? "async/raw-eglSwapBuffers" : "vsync/SDL_GL_SwapWindow",
			(void *)ma_window, ma_display, ma_surface, (void *)ma_eglSwapBuffers);
}

void MA_present_frame(void) {
	if (!ma_window) return;
	uint64_t t0 = SDL_GetPerformanceCounter();

	if (ma_mode == MA_PRESENT_ASYNC && ma_eglSwapBuffers && ma_display && ma_surface)
		ma_eglSwapBuffers(ma_display, ma_surface);
	else
		SDL_GL_SwapWindow(ma_window);

	uint64_t now = SDL_GetPerformanceCounter();
	uint64_t us = (now - t0) * 1000000ull / (ma_freq ? ma_freq : 1);
	st_swap_sum += us;
	st_swaps++;
	if (us > st_swap_max) st_swap_max = us;
	if (us > 1000) st_over_1ms++;
	if (us > 8300) st_over_half++;   // half a 60Hz period: only a display-side
	                                 // block or a GPU settle can take this long
	// Pace composition (RetroArch runloop PACE_VSYNC): a present that took a
	// measurable slice of the frame means the display paced this frame, so the
	// main loop must not add its timer on top.
	if (us > 1000) st_blocked = 1;

	if (st_gap_last) {
		uint64_t gap = (now - st_gap_last) * 1000000ull / (ma_freq ? ma_freq : 1);
		if (!st_gap_min || gap < st_gap_min) st_gap_min = gap;
		if      (gap < 5000)  st_gap_lt5++;
		else if (gap < 12000) st_gap_lt12++;
		else if (gap < 20000) st_gap_lt20++;
		else                  st_gap_ge20++;
		st_gaps++;
	}
	st_gap_last = now;
}

// Read-and-clear: whether the present blocked this frame (consumed by the
// pacing code, like ma_audio_wrote_frame).
int MA_present_blocked(void) { int b = st_blocked; st_blocked = 0; return b; }

void MA_present_take_stats(uint64_t *swap_sum_us, uint64_t *swap_max_us, unsigned *swaps,
		unsigned *over_1ms, unsigned *over_half_period,
		unsigned *gaps, uint64_t *gap_min_us,
		unsigned *gap_lt5ms, unsigned *gap_lt12ms, unsigned *gap_lt20ms, unsigned *gap_ge20ms) {
	if (swap_sum_us) *swap_sum_us = st_swap_sum;
	if (swap_max_us) *swap_max_us = st_swap_max;
	if (swaps) *swaps = st_swaps;
	if (over_1ms) *over_1ms = st_over_1ms;
	if (over_half_period) *over_half_period = st_over_half;
	if (gaps) *gaps = st_gaps;
	if (gap_min_us) *gap_min_us = st_gap_min;
	if (gap_lt5ms) *gap_lt5ms = st_gap_lt5;
	if (gap_lt12ms) *gap_lt12ms = st_gap_lt12;
	if (gap_lt20ms) *gap_lt20ms = st_gap_lt20;
	if (gap_ge20ms) *gap_ge20ms = st_gap_ge20;
	st_swap_sum = 0; st_swap_max = 0; st_swaps = 0; st_over_1ms = 0; st_over_half = 0;
	st_gaps = 0; st_gap_min = 0; st_gap_lt5 = 0; st_gap_lt12 = 0; st_gap_lt20 = 0; st_gap_ge20 = 0;
}
