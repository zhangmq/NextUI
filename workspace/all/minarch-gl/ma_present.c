// See ma_present.h.  One swap for the whole frontend, one place that measures
// what presentation costs.
#include <SDL2/SDL.h>

#include "ma_present.h"

static SDL_Window *ma_window = NULL;
static uint64_t ma_freq = 0;

static uint64_t st_swap_sum = 0, st_swap_max = 0, st_gap_last = 0, st_gap_min = 0;
static unsigned st_swaps = 0, st_over_1ms = 0, st_over_half = 0;
static unsigned st_gaps = 0, st_gap_lt5 = 0, st_gap_lt12 = 0, st_gap_lt20 = 0, st_gap_ge20 = 0;
static volatile int st_blocked = 0;

void MA_present_init(SDL_Window *window) {
	ma_window = window;
	ma_freq = SDL_GetPerformanceFrequency();
	SDL_Log("minarch: present via SDL_GL_SwapWindow (window=%p)", (void *)ma_window);
}


void MA_present_frame(void) {
	if (!ma_window) return;
	// One-shot proof that present is reachable at all: the first regression
	// this hook had was an unimplemented init, where the swap silently did
	// nothing and the screen never changed.
	static int announced = 0;
	if (!announced) {
		announced = 1;
		SDL_Log("minarch: first present");
	}

	uint64_t t0 = SDL_GetPerformanceCounter();
	SDL_GL_SwapWindow(ma_window);
	uint64_t now = SDL_GetPerformanceCounter();

	uint64_t us = (now - t0) * 1000000ull / (ma_freq ? ma_freq : 1);
	st_swap_sum += us;
	st_swaps++;
	if (us > st_swap_max) st_swap_max = us;
	if (us > 1000) st_over_1ms++;
	if (us > 8300) st_over_half++;   // half a 60Hz period: only a display-side
	                                 // block or a GPU settle can take this long
	// Pace composition (RetroArch runloop PACE_VSYNC).
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
