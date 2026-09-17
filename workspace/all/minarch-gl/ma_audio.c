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

size_t audio_sample_batch_callback(const int16_t *data, size_t frames) {
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
		return written;
	}
	else return frames;
}
