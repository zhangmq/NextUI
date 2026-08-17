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

// ---------------------------------------------------------------------------
// Rotation (RETRO_ENVIRONMENT_SET_ROTATION) - RetroArch gl2_renderchain style.
//
// flycast libretro renders vertical (ROT270) games UNROTATED into the FBO
// (640x480 landscape) and asks the frontend to rotate the output 90 degrees
// (libretro.cpp: "actual framebuffer rotation is done by frontend"). We
// present the FBO texture with a passthrough quad whose texture coordinates
// implement the rotation - the display rect is aspect-fit into the device
// using the ROTATED dimensions, mirroring RA's
// video_viewport_get_scaled_aspect2 + mvp rotation.
//
// Why a quad and not glBlitFramebuffer: glBlitFramebuffer cannot rotate 90
// degrees. Why a state reset after the quad: flycast's GLES2 renderer keeps
// its own GL state shadow (glcache, core/rend/gles/glcache.h) that SKIPS real
// gl calls when the cached value matches the requested one, and glsm's
// STATE_BIND only resets program/viewport/textures/attribs/FBO - caps and
// blend/stencil/depth/scissor funcs survive into the next core frame. RA's
// gl2_frame handles this with explicit per-frame state management
// (gl2_renderchain_restore_default_state + glDisable(STENCIL_TEST|BLEND) +
// glBlendFunc/glClearColor); we do the same, tuned to flycast's frame-end
// state (stencil+blend left enabled so glcache's per-draw Enable() skips are
// harmless).
// ---------------------------------------------------------------------------

static unsigned ma_gl_rotation = 0;   // 0-3, from SET_ROTATION (0 = no rotation)
static GLuint ma_gl_present_prog = 0;
static GLuint ma_gl_present_vao = 0;
static GLuint ma_gl_present_vbo = 0;

static const char *ma_gl_present_vs =
	"#version 300 es\n"
	"layout(location = 0) in vec2 aPos;\n"
	"layout(location = 1) in vec2 aTex;\n"
	"out vec2 vTex;\n"
	"void main() { vTex = aTex; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *ma_gl_present_fs =
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec2 vTex;\n"
	"uniform sampler2D uTex;\n"
	"out vec4 fragColor;\n"
	"void main() { fragColor = texture(uTex, vTex); }\n";

static GLuint ma_gl_compile_shader(GLenum type, const char *src) {
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);
	GLint status = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status) {
		char log[512];
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		LOG_error("minarch: present shader compile failed: %s\n", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

// Lazy init of the present quad pipeline (must run with the GL context
// current - MA_GL_video_refresh guarantees that).
static bool ma_gl_present_init(void) {
	GLuint vs = ma_gl_compile_shader(GL_VERTEX_SHADER, ma_gl_present_vs);
	if (!vs) return false;
	GLuint fs = ma_gl_compile_shader(GL_FRAGMENT_SHADER, ma_gl_present_fs);
	if (!fs) { glDeleteShader(vs); return false; }

	ma_gl_present_prog = glCreateProgram();
	glAttachShader(ma_gl_present_prog, vs);
	glAttachShader(ma_gl_present_prog, fs);
	glBindAttribLocation(ma_gl_present_prog, 0, "aPos");
	glBindAttribLocation(ma_gl_present_prog, 1, "aTex");
	glLinkProgram(ma_gl_present_prog);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint status = 0;
	glGetProgramiv(ma_gl_present_prog, GL_LINK_STATUS, &status);
	if (!status) {
		char log[512];
		glGetProgramInfoLog(ma_gl_present_prog, sizeof(log), NULL, log);
		LOG_error("minarch: present program link failed: %s\n", log);
		glDeleteProgram(ma_gl_present_prog);
		ma_gl_present_prog = 0;
		return false;
	}

	// One VAO + one interleaved VBO (x,y,u,v per vertex), re-uploaded each
	// frame (128 bytes - negligible). Own VAO keeps the attrib setup away
	// from the core's VAOs.
	glGenVertexArrays(1, &ma_gl_present_vao);
	glBindVertexArray(ma_gl_present_vao);
	glGenBuffers(1, &ma_gl_present_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, ma_gl_present_vbo);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void*)0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void*)(2 * sizeof(GLfloat)));
	glEnableVertexAttribArray(1);
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	LOG_info("minarch: present quad pipeline ready (prog=%u vao=%u vbo=%u)\n",
		(unsigned)ma_gl_present_prog, (unsigned)ma_gl_present_vao,
		(unsigned)ma_gl_present_vbo);
	return true;
}

// Reset every GL state flycast's glcache tracks to values that make its
// cache skips harmless for the next core frame. flycast's GLES2 renderer
// (gles/gldraw.cpp) enables GL_STENCIL_TEST at the start of every draw list
// and never disables it, and typically ends frames with the translucent list
// (GL_BLEND enabled, SrcBlend/DstBlend = SRC_ALPHA/ONE_MINUS_SRC_ALPHA is the
// PVR default), so leaving stencil+blend ENABLED with those funcs means the
// per-draw Enable()/BlendFunc() cache hits are no-ops that match reality.
// Everything else is left in the disabled/default state flycast's frame-start
// code (RenderFrame) re-establishes anyway.
static void ma_gl_reset_core_state(void) {
	glEnable(GL_STENCIL_TEST);
	glEnable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glStencilFunc(GL_ALWAYS, 0, 0);
	glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
	glStencilMask(0xFF);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_TRUE);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glCullFace(GL_BACK);
	glFrontFace(GL_CCW);
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glUseProgram(0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindVertexArray(0);
	glActiveTexture(GL_TEXTURE0);
}

// Present the core's FBO centered with the configured rotation via a quad.
// Texture coordinates implement the rotation (the quad rect itself is always
// axis-aligned, aspect-fit from the ROTATED dimensions). Verified against
// RetroArch's gl2 renderchain with flycast (bottom_left_origin=true ->
// unflipped vertexes/texcoords; mvp = Rz(90*rot) * ortho):
//   rot 0:  u =  cx*tw,  v =  cy*th
//   rot 1:  u =  cy*tw,  v = (1-cx)*th   (90 deg counter-clockwise)
//   rot 2:  u = (1-cx)*tw, v = (1-cy)*th (180)
//   rot 3:  u = (1-cy)*tw, v =  cx*th    (270 CCW = 90 clockwise)
// where (cx,cy) is the corner's normalized position on the display rect and
// (tw,th) = (width,height)/FBO size map only the used region of the fixed
// 1024x1024 FBO texture (content anchored at its GL bottom-left).
static void ma_gl_present_quad(unsigned width, unsigned height) {
	if (!ma_gl_present_prog && !ma_gl_present_init())
		return;

	// Rotated display dims: for 90/270 degree rotation the image's on-screen
	// bounding box is (height,width) swapped. ma_gl_rotation is the SET_ROTATION
	// index (0-3), so odd values mean 90/270 degrees. This matches RetroArch's
	// viewport aspect handling (it fits the core's reported aspect, which for
	// flycast rotated games is 0.75 = height/width of the 640x480 frame).
	unsigned disp_w = width, disp_h = height;
	if (ma_gl_rotation % 2 == 1) { disp_w = height; disp_h = width; }
	// Aspect-fit into the device (no upscale, like the blit path), centered.
	double s = fmin((double)DEVICE_WIDTH / disp_w, (double)DEVICE_HEIGHT / disp_h);
	if (s > 1.0) s = 1.0;
	int dst_w = (int)(disp_w * s);
	int dst_h = (int)(disp_h * s);
	int dst_x = (DEVICE_WIDTH - dst_w) / 2;
	int dst_y = (DEVICE_HEIGHT - dst_h) / 2;

	// NDC positions (GL y is bottom-up; screen y is top-down).
	float cx0 = 2.0f * dst_x / DEVICE_WIDTH - 1.0f;
	float cx1 = 2.0f * (dst_x + dst_w) / DEVICE_WIDTH - 1.0f;
	float cy0 = 1.0f - 2.0f * (dst_y + dst_h) / DEVICE_HEIGHT; // bottom
	float cy1 = 1.0f - 2.0f * dst_y / DEVICE_HEIGHT;           // top

	float tw = (float)width / MA_GL_FBO_MAX_W;
	float th = (float)height / MA_GL_FBO_MAX_H;

	// Per-vertex (x, y, u, v), triangle strip order BL, BR, TL, TR.
	float verts[16] = { 0 };
	switch (ma_gl_rotation % 4) {
	case 1:
		verts[0]=cx0; verts[1]=cy0; verts[2]=0;    verts[3]=th;
		verts[4]=cx1; verts[5]=cy0; verts[6]=0;    verts[7]=0;
		verts[8]=cx0; verts[9]=cy1; verts[10]=tw;  verts[11]=th;
		verts[12]=cx1; verts[13]=cy1; verts[14]=tw; verts[15]=0;
		break;
	case 2:
		verts[0]=cx0; verts[1]=cy0; verts[2]=tw;   verts[3]=th;
		verts[4]=cx1; verts[5]=cy0; verts[6]=0;    verts[7]=th;
		verts[8]=cx0; verts[9]=cy1; verts[10]=tw;  verts[11]=0;
		verts[12]=cx1; verts[13]=cy1; verts[14]=0; verts[15]=0;
		break;
	case 3:
		verts[0]=cx0; verts[1]=cy0; verts[2]=tw;   verts[3]=0;
		verts[4]=cx1; verts[5]=cy0; verts[6]=tw;   verts[7]=th;
		verts[8]=cx0; verts[9]=cy1; verts[10]=0;   verts[11]=0;
		verts[12]=cx1; verts[13]=cy1; verts[14]=0; verts[15]=th;
		break;
	default: // 0
		verts[0]=cx0; verts[1]=cy0; verts[2]=0;    verts[3]=0;
		verts[4]=cx1; verts[5]=cy0; verts[6]=tw;   verts[7]=0;
		verts[8]=cx0; verts[9]=cy1; verts[10]=0;   verts[11]=th;
		verts[12]=cx1; verts[13]=cy1; verts[14]=tw; verts[15]=th;
		break;
	}

	// Draw the quad with a clean pipeline.
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, DEVICE_WIDTH, DEVICE_HEIGHT);
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);

	glUseProgram(ma_gl_present_prog);
	glBindVertexArray(ma_gl_present_vao);
	glBindBuffer(GL_ARRAY_BUFFER, ma_gl_present_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, ma_gl_fbo_tex);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	// Leave the state flycast's glcache expects (see ma_gl_reset_core_state).
	ma_gl_reset_core_state();
}

void MA_GL_set_rotation(unsigned rotation) {
	ma_gl_rotation = rotation % 4;
	LOG_info("minarch: SET_ROTATION %u -> present rotates %u deg (%s path)\n",
		rotation, ma_gl_rotation * 90,
		ma_gl_rotation == 0 ? "blit" : "quad");
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

	if (ma_gl_rotation != 0) {
		// Vertical/rotated game (flycast ROT270 etc.): the core rendered
		// unrotated into the FBO and asked us to rotate the output.
		// RetroArch-style quad present with per-frame state management.
		ma_gl_present_quad(width, height);
		SDL_GL_SwapWindow(win);
		return;
	}

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

	// Leave the read binding on the default framebuffer so the in-game
	// menu's GFX_GL_screenCapture (glReadPixels) grabs what is actually on
	// screen (the presented, centered frame) instead of the 1024x1024 render
	// FBO - reading that would return the 640x480 content anchored at its
	// bottom-left, i.e. the game shifted left with an empty band on the right.
	// Safe for flycast: glsm's next STATE_BIND restores the core's FBO
	// binding (default_framebuffer == ma_gl_fbo) at the start of retro_run.
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

	SDL_GL_SwapWindow(win);
}

// Re-make our GL context current after any frontend UI activity that may have
// switched to another context (the SDL_Renderer used by the in-game menu owns
// a SEPARATE GLES2 context and leaves it current). Without this, the first
// retro_run after the menu runs flycast's glsm STATE_BIND + RenderFrame with
// the renderer's context current: its FBO/texture ids then refer to phantom
// objects in that context, so the frame renders nowhere (frozen frame) and
// the GLCache/glsm shadow state gets polluted, which surfaces as rendering
// corruption after closing the menu. The game present path (MA_GL_video_refresh)
// already makes this context current every frame, so this only matters for the
// window between Menu_loop and the next retro_run.
void MA_GL_make_current(void) {
	if (!hw_render_active) return;
	SDL_Window *win = PLAT_getGLWindow();
	SDL_GLContext ctx = PLAT_getGLContext();
	if (win && ctx) SDL_GL_MakeCurrent(win, ctx);
}
