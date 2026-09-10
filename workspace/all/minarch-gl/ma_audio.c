#include <stdbool.h>
#include <SDL2/SDL.h>
#include <msettings.h>
#include "ma_internal.h"
#include "ma_audio.h"
#include "ma_gl.h"

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
	// hw-render cores (flycast) run their emulator on a separate thread at
	// full speed; blocking on a full audio ring buffer gives them
	// backpressure so emulation is throttled to the sound card rate
	// (RetroArch convention). Software cores never wait (they're already
	// throttled by vsync and the buffer doesn't overflow).
	SND_setBlockOnFull(MA_GL_is_active());
	if (!fast_forward || ff_audio) {
		size_t written;
		if (use_core_fps || fast_forward) {
			written = SND_batchSamples_fixed_rate((const SND_Frame*)data, frames);
		}
		else {
			written = SND_batchSamples((const SND_Frame*)data, frames);
		}
		/* A frame that reached the ring buffer is paced by audio
		 * backpressure (RA RUNLOOP_PACE_AUDIO); the main-loop timer must
		 * not add a second sleep on top of it. */
		ma_audio_wrote_frame = 1;
		return written;
	}
	else return frames;
}
