#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>

#include <GLES3/gl3.h> // FBO / glBlitFramebuffer (GLES3; Mali libGLESv2 exports them)

#include "ma_internal.h"
#include "ma_gl.h"

// The libretro hardware-render callback as negotiated with the core.
// get_proc_address / get_current_framebuffer are owned by the frontend;
// context_reset / context_destroy are provided by the core.
static struct retro_hw_render_callback hw_render;
static bool hw_render_active = false;

// Frontend hw-render FBO. The core renders into this (glsm caches the id
// returned by get_current_framebuffer at context reset), and we blit it
// centered onto the default framebuffer at present time -- the RetroArch
// convention (core draws into the frontend FBO, frontend presents it).
// Fixed max size so the id never changes after context reset.
#define MA_GL_FBO_MAX_W 1024
#define MA_GL_FBO_MAX_H 1024
static GLuint ma_gl_fbo = 0;
static GLuint ma_gl_fbo_tex = 0;
static GLuint ma_gl_fbo_rb = 0;
static bool ma_gl_fbo_valid = false;

// Called by the core (through glsm) to resolve GL function pointers.
// Our context is a plain SDL GL context, so SDL_GL_GetProcAddress covers
// everything (it wraps eglGetProcAddress on the mali winsys).
static retro_proc_address_t ma_gl_get_proc_address(const char *sym) {
	if (!sym) return NULL;
	return (retro_proc_address_t)(uintptr_t)SDL_GL_GetProcAddress(sym);
}

// Returns the frontend FBO the core should render into. Must be current and
// complete before the core's context_reset runs (glsm reads it in SETUP).
static uintptr_t ma_gl_get_current_framebuffer(void) {
	return ma_gl_fbo_valid ? (uintptr_t)ma_gl_fbo : 0;
}

static bool ma_gl_create_fbo(void) {
	glGenTextures(1, &ma_gl_fbo_tex);
	glBindTexture(GL_TEXTURE_2D, ma_gl_fbo_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, MA_GL_FBO_MAX_W, MA_GL_FBO_MAX_H, 0,
			GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	// flycast glClears depth|stencil every frame -> FBO needs both.
	glGenRenderbuffers(1, &ma_gl_fbo_rb);
	glBindRenderbuffer(GL_RENDERBUFFER, ma_gl_fbo_rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
			MA_GL_FBO_MAX_W, MA_GL_FBO_MAX_H);

	glGenFramebuffers(1, &ma_gl_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, ma_gl_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_TEXTURE_2D, ma_gl_fbo_tex, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
			GL_RENDERBUFFER, ma_gl_fbo_rb);

	ma_gl_fbo_valid = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	if (!ma_gl_fbo_valid)
		LOG_error("minarch: hw-render FBO incomplete\n");
	return ma_gl_fbo_valid;
}

static void ma_gl_destroy_fbo(void) {
	if (ma_gl_fbo) glDeleteFramebuffers(1, &ma_gl_fbo);
	if (ma_gl_fbo_tex) glDeleteTextures(1, &ma_gl_fbo_tex);
	if (ma_gl_fbo_rb) glDeleteRenderbuffers(1, &ma_gl_fbo_rb);
	ma_gl_fbo = ma_gl_fbo_tex = ma_gl_fbo_rb = 0;
	ma_gl_fbo_valid = false;
}

// Frame-rate throttle, same scheme as GFX_flip_fixed_rate (api.c): schedule
// each present at frame_index * 1/fps, usleep the bulk then busy-wait the
// remainder. This is the libretro convention for frontends without vsync
// (minarch's GL swap has swap interval 0). Skipped while fast-forwarding
// (run_frame's limitFF handles that).
static void ma_gl_throttle(double fps) {
	if (fast_forward || fps <= 0.0) return;

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
			// busy-wait remainder for accurate alignment
		}
	} else if (offset > max_lost_frames * frame_duration) {
		// fell behind by more than 2 frames; re-anchor next frame
		frame_index = -1;
		last_fps = 0.0;
	}
}

bool MA_GL_set_hw_render(struct retro_hw_render_callback *cb) {
	if (!cb) return false;

	switch (cb->context_type) {
		case RETRO_HW_CONTEXT_OPENGLES2:
		case RETRO_HW_CONTEXT_OPENGLES3:
		case RETRO_HW_CONTEXT_OPENGLES_VERSION:
			// Supported: the SDL context is GLES 3.2, backward compatible
			// with any GLES2/3 request.
			break;
		default:
			LOG_info("minarch: HW render context type %d not supported, refusing\n", cb->context_type);
			return false;
	}

	// Make sure our GL context is current so FBO creation works.
	SDL_Window *win = PLAT_getGLWindow();
	SDL_GLContext ctx = PLAT_getGLContext();
	if (win && ctx) SDL_GL_MakeCurrent(win, ctx);

	// Frontend FBO must exist before the core's context_reset (glsm reads
	// get_current_framebuffer during SETUP and caches the id).
	if (!ma_gl_fbo_valid && !ma_gl_create_fbo())
		return false;

	// Stash the core's callbacks before overwriting the fields we own.
	hw_render = *cb;

	// Frontend-owned fields.
	hw_render.get_proc_address      = ma_gl_get_proc_address;
	hw_render.get_current_framebuffer = ma_gl_get_current_framebuffer;
	hw_render.cache_context         = false;

	// Give the core back the filled-in struct.
	*cb = hw_render;

	hw_render_active = true;
	LOG_info("minarch: GLES hardware render enabled (context_type=%d, reset=%p, destroy=%p, fbo=%u)\n",
		hw_render.context_type, (void*)hw_render.context_reset, (void*)hw_render.context_destroy,
		(unsigned)ma_gl_fbo);

	// Context reset must happen *before* the core's retro_load_game returns:
	// flycast (ThreadedRendering) starts its emu thread inside load_game (after
	// setting AV info), and that thread begins rendering immediately. If we
	// defer context_reset until after load_game, the emu thread renders with
	// uninitialized glsm symbols -> SIGSEGV @ (nil). Our SDL GL context is
	// already current here (UI init), so it's safe to reset right away.
	MA_GL_context_reset();
	return true;
}

bool MA_GL_is_active(void) {
	return hw_render_active;
}

void MA_GL_context_reset(void) {
	if (!hw_render_active) return;
	if (!hw_render.context_reset) return;

	// Make sure our GL context is current before the core (re)creates its
	// GL resources. The UI normally leaves it current, but the core may
	// have switched contexts during load.
	SDL_Window *win = PLAT_getGLWindow();
	SDL_GLContext ctx = PLAT_getGLContext();
	if (win && ctx) SDL_GL_MakeCurrent(win, ctx);

	LOG_info("minarch: calling core context_reset\n");
	hw_render.context_reset();
	LOG_info("minarch: core context_reset returned\n");
}

void MA_GL_context_destroy(void) {
	if (!hw_render_active) return;
	if (hw_render.context_destroy) {
		LOG_info("minarch: calling core context_destroy\n");
		hw_render.context_destroy();
	}
	hw_render_active = false;
	memset(&hw_render, 0, sizeof(hw_render));
	ma_gl_destroy_fbo();
}

void MA_GL_video_refresh(const void *data, unsigned width, unsigned height, size_t pitch) {
	if (!hw_render_active) return;
	if (data != (const void*)RETRO_HW_FRAME_BUFFER_VALID) return;
	if (!ma_gl_fbo_valid) return;

	SDL_Window *win = PLAT_getGLWindow();
	SDL_GLContext ctx = PLAT_getGLContext();
	if (!win || !ctx) return;

	// Align the present to the core's frame rate (RetroArch convention for
	// frontends without vsync). Do it before MakeCurrent/Swap.
	ma_gl_throttle(core.fps);

	SDL_GL_MakeCurrent(win, ctx);

	// Present the core's FBO centered onto the default framebuffer
	// (RetroArch: FBO quad; nextui convention: center via (device-src)/2).
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, DEVICE_WIDTH, DEVICE_HEIGHT);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);

	int dst_w = (int)width;
	int dst_h = (int)height;
	if (dst_w > DEVICE_WIDTH || dst_h > DEVICE_HEIGHT) {
		double s = fmin((double)DEVICE_WIDTH / dst_w, (double)DEVICE_HEIGHT / dst_h);
		dst_w = (int)(dst_w * s);
		dst_h = (int)(dst_h * s);
	}
	int dst_x = (DEVICE_WIDTH - dst_w) / 2;
	int dst_y = (DEVICE_HEIGHT - dst_h) / 2;

	glBindFramebuffer(GL_READ_FRAMEBUFFER, ma_gl_fbo);
	glBlitFramebuffer(0, 0, (int)width, (int)height,
			dst_x, dst_y, dst_x + dst_w, dst_y + dst_h,
			GL_COLOR_BUFFER_BIT, GL_LINEAR);

	SDL_GL_SwapWindow(win);
}
