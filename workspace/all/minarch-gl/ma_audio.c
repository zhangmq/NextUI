#include <stdbool.h>
#include <SDL2/SDL.h>
#include <msettings.h>
#include "ma_internal.h"
#include "ma_audio.h"

static bool resetAudio = false;

volatile int ma_audio_wrote_frame = 0;

void Audio_onSinkChanged(int device, int watch_event) {
	switch (watch_event) {
	case DIRWATCH_CREATE:      LOG_info("callback reason: DIRWATCH_CREATE\n");      break;
	case DIRWATCH_DELETE:      LOG_info("callback reason: DIRWATCH_DELETE\n");      break;
	case FILEWATCH_MODIFY:     LOG_info("callback reason: FILEWATCH_MODIFY\n");     break;
	case FILEWATCH_DELETE:     LOG_info("callback reason: FILEWATCH_DELETE\n");     break;
	case FILEWATCH_CLOSE_WRITE:LOG_info("callback reason: FILEWATCH_CLOSE_WRITE\n");break;
	}

	resetAudio = true;

	// FIXME: This shouldnt be necessary, alsa should just read .asoundrc for the changed default device.
	if (device == AUDIO_SINK_BLUETOOTH)
		SDL_setenv("AUDIODEV", "bluealsa", 1);
	else
		SDL_setenv("AUDIODEV", "default", 1);
}

void Audio_checkAndResetIfNeeded(void) {
	if (!resetAudio) return;
	resetAudio = false;
	LOG_info("Resetting audio device config! (new state: %s)\n", SDL_getenv("AUDIODEV"));
	SND_resetAudio(core.sample_rate, core.fps);
}

void audio_sample_callback(int16_t left, int16_t right) {
	if (rewinding && !rewind_ctx.audio) return;
	if (!fast_forward || ff_audio) {
		if (use_core_fps || fast_forward) {
			SND_batchSamples_fixed_rate(&(const SND_Frame){left,right}, 1);
		}
		else {
			SND_batchSamples(&(const SND_Frame){left,right}, 1);
		}
	}
}

/* Per-call cost of the audio path (this is inside retro_run; a headless
 * harness has no resampler and no ring, so this is another piece of the
 * frontend-vs-harness gap).  MINARCH_VIDEO_PROFILE=1 logs every 5 s. */
static int audprof_on = -1;
static uint64_t audprof_ns;
static unsigned audprof_n;
static uint32_t audprof_last_ms;
static uint64_t audprof_now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

size_t audio_sample_batch_callback(const int16_t *data, size_t frames) {
	if (audprof_on < 0)
		audprof_on = getenv("MINARCH_VIDEO_PROFILE") ? 1 : 0;
	uint64_t ap_t0 = audprof_on ? audprof_now() : 0;
	if (rewinding && !rewind_ctx.audio) return frames;
	// The sound card is the one real-time master clock for EVERY core, so the
	// write blocks on a full ring buffer (RA RUNLOOP_PACE_AUDIO / blocking
	// write convention): the producer is held to the drain rate the user
	// actually hears. pace_frame() (minarch.c) then adds no timer on top.
	//
	// This used to be hw-render only: software cores dropped frames instead
	// (SND_setBlockOnFull(false) -> api.c break-on-full), which left them
	// paced by whatever the display/sleep path happened to do. One clock,
	// both families.
	SND_setBlockOnFull(true);
	if (!fast_forward || ff_audio) {
		size_t written;
		if (use_core_fps || fast_forward) {
			/* Fixed-rate path: resamples to the core's fps and DROPS when
			 * the ring is full (api.c, "should never happen"). It never
			 * holds the producer, so it is not a pace: leave
			 * ma_audio_wrote_frame alone and let pace_frame's timer be the
			 * clock for this frame (sync_ref = core fps means exactly that
			 * -- the timer is the reference, audio follows it).
			 * Marking this as an audio-paced frame is what left
			 * sync_ref=core unpaced altogether: no block, and the timer
			 * skipped as "already paced". */
			written = SND_batchSamples_fixed_rate((const SND_Frame*)data, frames);
		}
		else {
			/* Blocking write: this frame is held to the sound card's drain
			 * rate, so pace_frame must not add a second, independent sleep
			 * on top of it (RA pace bits). */
			written = SND_batchSamples((const SND_Frame*)data, frames);
			ma_audio_wrote_frame = 1;
		}
		if (audprof_on) {
			uint64_t t = audprof_now();
			audprof_ns += t - ap_t0;
			audprof_n++;
			uint32_t now_ms = (uint32_t)(t / 1000000ull);
			if (!audprof_last_ms) audprof_last_ms = now_ms;
			if (now_ms - audprof_last_ms >= 5000) {
				LOG_info("minarch: audio batch %.2f ms avg over %u calls\n",
						(double)audprof_ns / 1e6 / (audprof_n ? audprof_n : 1), audprof_n);
				fflush(stdout);
				audprof_ns = 0; audprof_n = 0; audprof_last_ms = now_ms;
			}
		}
		return written;
	}
	else return frames;
}

/* RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK (62): once per frame,
 * from the main loop, after core.run().  RetroArch's shape (runloop.c:8029-
 * 8065): snapshot the ring, translate it to "percent occupied", call the core
 * with no audio lock held.  When there is no usable ring (no audio device,
 * teardown in progress) the core is told inactive instead of being fed
 * nonsense -- and it must not be called at all after it unregistered. */
void MA_Audio_buffer_status_tick(void)
{
	unsigned total = 0, free_frames = 0, underruns = 0;
	bool active = false;
	unsigned occupancy = 0;
	bool underrun = false;

	if (!core.audio_buffer_status) return;

	/* Optional latch: keep reporting "underrun" for N frames after the driver
	 * actually ran dry.  A single-frame pulse is enough for a core that skips
	 * one frame, but not for one that needs several consecutive skips to climb
	 * back (Yabadabadoo's auto mode caps consecutive skips at Frameskip
	 * Interval).  Off by default; MINARCH_AUDIO_UNDERRUN_LATCH=<frames>. */
	static int latch_enabled = -1, latch_frames = 0, latch_left = 0;
	if (latch_enabled < 0) {
		const char *v = getenv("MINARCH_AUDIO_UNDERRUN_LATCH");
		latch_frames = (v && v[0]) ? atoi(v) : 0;
		if (latch_frames < 0) latch_frames = 0;
		latch_enabled = 1;
	}

	if (SND_getBufferStats(&total, &free_frames, &underruns)) {
		if (total)
			occupancy = (unsigned)(100 - (free_frames * 100) / total);
		/* RA's low-water mark is 25% occupancy (runloop.c:8045-8060).  The
		 * ring is written under backpressure, so a core that cannot keep up
		 * also shows up in the driver's "silence for want of audio" counter;
		 * report that as an underrun too, so the core skips a frame before
		 * the user hears the gap. */
		int fresh = (underruns != core.audio_underruns_last);
		if (fresh) latch_left = latch_frames;
		else if (latch_left > 0) latch_left--;
		underrun = (occupancy < 25) || fresh || (latch_left > 0);
		active = true;
	}
	core.audio_underruns_last = underruns;
	core.audio_buffer_status(active, occupancy, underrun);

	/* Optional instrumentation (MINARCH_AUDIO_STATUS_LOG=1): what the core is
	 * being told, and what the ring really looks like, once every 5 s.  This
	 * is how the 25% low-water mark gets checked on a device rather than
	 * argued about: frequencies of active/underrun and the occupancy range. */
	{
		static int log_enabled = -1;
		if (log_enabled < 0) {
			const char *v = getenv("MINARCH_AUDIO_STATUS_LOG");
			log_enabled = (v && v[0] && v[0] != '0');
		}
		if (log_enabled) {
			static unsigned n, occ_sum, occ_min = 999, occ_max, und_n, act_n;
			static Uint32 last;
			static unsigned ur_last_seen;
			Uint32 now = SDL_GetTicks();
			n++; act_n += active ? 1 : 0; und_n += underrun ? 1 : 0;
			occ_sum += occupancy;
			if (occupancy < occ_min) occ_min = occupancy;
			if (occupancy > occ_max) occ_max = occupancy;
			if (!last) last = now;
			if (now - last >= 5000) {
				unsigned new_und = underruns - ur_last_seen;
				ur_last_seen = underruns;
				LOG_info("minarch: audio status %us: %u calls, active %u, underrun-flag %u, "
						"occupancy avg %u min %u max %u, %u new underrun periods\n",
						(now - last) / 1000, n, act_n, und_n,
						n ? occ_sum / n : 0, occ_min == 999 ? 0 : occ_min, occ_max, new_und);
				fflush(stdout);
				n = act_n = und_n = occ_sum = occ_max = 0; occ_min = 999; last = now;
			}
		}
	}
}
