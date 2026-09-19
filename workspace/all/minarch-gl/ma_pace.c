// See ma_pace.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <SDL2/SDL.h>

#include "ma_internal.h"
#include "ma_pace.h"
#include "ma_present.h"
#include "ma_gl.h"

// See ma_pace.h.  Pacing is a run-loop decision -- not
// a GL, SDL or core one. It composes the RA runloop pace bits (runloop.c
// RUNLOOP_PACE_*):
//
//   PACE_AUDIO  the blocking audio write (audio_sample_batch_callback) already
//               held retro_run to the sound card's drain rate. That is the one
//               real-time master clock -- the only reference that cannot drift
//               against what the user hears. Nothing is added on top of it.
//   PACE_TIMER  fallback for frames that wrote no audio at all (silent scenes,
//               muted cores, cores without an audio device): schedule the
//               present at frame_index * 1/fps on an absolute timeline, sleep
//               the bulk and busy-wait the remainder. Re-anchor when more than
//               two frames are lost.
//
// Skipped while fast-forwarding: limitFF (ma_runframe.c) owns that cadence, and
// while rewinding the loop is driven by the rewind cadence.
//
// This is deliberately the ONLY sleep/pacing site in the loop. Both the old
// software-path timer (GFX_flip_fixed_rate, called from inside the video
// callback) and the old hw-only copy in ma_gl.c were the same schedule with
// different guards; keeping two of them meant the two core families were paced
// by different clocks and only one of them respected audio backpressure.
void MA_pace_frame(void) {
	if (fast_forward || rewinding) return;
	double fps = core.fps;
	if (fps <= 0.0) return;

	if (ma_audio_wrote_frame)
		return; // this frame is already paced by the sound card
	if (MA_present_blocked())
		return; // ...or by the display (RA PACE_VSYNC)

	static int64_t frame_index = -1;
	static int64_t first_frame_start_time = 0;
	static double last_fps = 0.0;

	int64_t perf_freq = SDL_GetPerformanceFrequency();
	int64_t now = SDL_GetPerformanceCounter();

	if (++frame_index == 0 || fps != last_fps) {
		frame_index = 0;
		first_frame_start_time = now;
		last_fps = fps;
	}

	int64_t frame_duration = perf_freq / fps;
	int64_t time_of_frame = first_frame_start_time + frame_index * frame_duration;
	int64_t offset = now - time_of_frame;
	const int64_t max_lost_frames = 2;

	if (offset < 0) {
		useconds_t time_to_sleep_us = (useconds_t)((time_of_frame - now) * 1e6 / perf_freq);
		const useconds_t min_waiting_time = 2000;
		if (time_to_sleep_us > min_waiting_time)
			usleep(time_to_sleep_us - min_waiting_time);
		while (SDL_GetPerformanceCounter() < time_of_frame) {
			// busy-wait the remainder for accurate alignment
		}
	} else if (offset > max_lost_frames * frame_duration) {
		// fell behind by more than 2 frames: re-anchor the timeline
		frame_index = -1;
		last_fps = 0.0;
	}
}

// Frame-time distribution (MINARCH_FRAME_LOG / /tmp/minarch_frame_log), off unless asked for.
// environment. The on-screen HUD only shows a windowed average, which hides
// the distinction that matters when a device stutters: is a slow frame a rare
// phase-beat (one 33ms frame among 16.7ms ones -- audio clock beating against
// vsync) or a systematic overrun (every frame 33ms -- the frame simply does not
// fit)? We cannot answer that from inside the loop with one number, and the HUD
// itself perturbs the measurement, so this logs a bucket histogram instead and
// costs nothing when disabled.
// Monotonic timestamp the optimizer may not fold.
//
// SDL_GetPerformanceCounter() is an ordinary function whose body LTO can see,
// so with -O3/-flto the back end proved two consecutive reads in the run loop
// identical and CSE'd them: the delta came out 0.0 on EVERY frame, the
// histogram's "ms <= 0" guard rejected every sample, and the 5s window was
// never printed. That is the long-standing "N64 (mupen hw path) telemetry
// never emits, and adding a LOG_info in the gate makes it reappear" TODO --
// it was never about the hw path, it is this measurement read being folded.
// noinline + a memory clobber keeps the read opaque; the cost is one call per
// loop iteration, only while the telemetry flag is set.
static __attribute__((noinline)) uint64_t ma_tick_now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	__asm__ __volatile__("" ::: "memory");
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#define MA_TICK_FREQ 1000000000.0

void MA_telemetry_tick(void) {
	// env var for a shell that has one, or just drop the marker file (no
	// device script has to be edited to start a measurement run)
	static int enabled = -1;
	if (enabled < 0) {
		const char *v = getenv("MINARCH_FRAME_LOG");
		enabled = (v && v[0] && v[0] != '0') || exists("/tmp/minarch_frame_log");
	}
	if (!enabled) return;


	enum { NB = 7 };
	static uint32_t buckets[NB];
	static uint32_t frames = 0;
	static double   sum_ms = 0.0;
	static double   worst_ms = 0.0;
	static uint64_t last_counter = 0;
	static uint32_t last_log_ms = 0;

	uint64_t now = ma_tick_now();
	if (!last_counter) { last_counter = now; last_log_ms = SDL_GetTicks(); return; }
	double ms = (double)(now - last_counter) * 1000.0 / MA_TICK_FREQ;
	last_counter = now;
	if (ms <= 0.0 || ms > 1000.0) return; // 1s+ means we were not measuring

	int b;
	if      (ms < 17.0) b = 0; // fits a 60Hz frame
	else if (ms < 19.0) b = 1; // a hair over
	else if (ms < 25.0) b = 2; // clearly late but not a full drop
	else if (ms < 35.0) b = 3; // one dropped frame (2 vsync periods)
	else if (ms < 50.0) b = 4;
	else if (ms < 70.0) b = 5;
	else                b = 6;
	buckets[b]++;
	frames++;
	sum_ms += ms;
	if (ms > worst_ms) worst_ms = ms;

	uint32_t now_ms = SDL_GetTicks();
	if (now_ms - last_log_ms >= 5000) {
		double elapsed = (double)(now_ms - last_log_ms);
		LOG_info("minarch: frames over %.1fs: %u total, %.1f fps, %.2f ms avg, %.1f ms worst; "
				"buckets <17/<19/<25/<35/<50/<70/>=70ms = %u/%u/%u/%u/%u/%u/%u\n",
				elapsed / 1000.0, frames, frames * 1000.0 / elapsed, sum_ms / (frames ? frames : 1), worst_ms,
				buckets[0], buckets[1], buckets[2], buckets[3], buckets[4], buckets[5], buckets[6]);
		{
			uint64_t s = 0, mx = 0, gmin = 0;
			unsigned n = 0, o1 = 0, oh = 0, gn = 0, g5 = 0, g12 = 0, g20 = 0, gge = 0;
			MA_present_take_stats(&s, &mx, &n, &o1, &oh, &gn, &gmin, &g5, &g12, &g20, &gge);
			if (n)
				LOG_info("minarch: present over the same window: %u swaps, %.0f us avg, "
						"%llu us max, %u over 1ms, %u over 8.3ms\n",
						n, (double)s / (double)n, (unsigned long long)mx, o1, oh);
			if (gn)
				LOG_info("minarch: present gaps: %u gaps, min %llu us; <5/<12/<20/>=20ms = %u/%u/%u/%u\n",
						gn, (unsigned long long)gmin, g5, g12, g20, gge);
		}
		{
			unsigned nf = 0, dp = 0; uint64_t ds = 0, dm = 0;
			MA_GL_take_present_stats(&nf, &dp, &ds, &dm);
			if (nf || dp)
			{
				unsigned fa = 0, fal = 0, fb = 0, ft = 0;
				MA_GL_take_ring_fence_stats(&fa, &fal, &fb, &ft);
				if (fa)
					LOG_info("minarch: hw video_cb: %u new, %u dupe; our draw %.0f us avg, "
							"%llu us max; ring fences %u armed, %u already-signalled, "
							"%u blocked, %u timeout\n",
							nf, dp, (double)ds / (double)(nf + dp), (unsigned long long)dm,
							fa, fal, fb, ft);
				else
					LOG_info("minarch: hw video_cb: %u new, %u dupe; our draw %.0f us avg, %llu us max\n",
							nf, dp, (double)ds / (double)(nf + dp), (unsigned long long)dm);
			}
		}
		memset(buckets, 0, sizeof(buckets));
		frames = 0; sum_ms = 0.0; worst_ms = 0.0;
		last_log_ms = now_ms;
	}
}

