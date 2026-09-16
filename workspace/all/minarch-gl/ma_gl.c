#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <sys/stat.h>
#include <stdlib.h>

#include <GLES3/gl3.h> // FBO + VAO (GLES3; Mali libGLESv2 exports them)

#include "ma_internal.h"
#include "ma_present.h"
#include "ma_gl.h"

// Screen X/Y raw offsets (Frontend menu): defined in generic_video.c as
// screenx = x - 64 / screeny = y - 64 (range -64..64 px). No header
// declaration exists - extern here for the present-rect computation.
extern int screenx;
extern int screeny;

// Screen Sharpness (Frontend menu): defined in generic_video.c next to
// PLAT_setSharpness. 1 = GL_LINEAR ("LINEAR"), 0 = GL_NEAREST ("NEAREST") -
// the sampler filter for the core FBO texture at present time, mirroring
// the software path's orig_texture sampling in the finalscale pass.
extern int g_sharpness_linear;

// The libretro hardware-render callback as negotiated with the core.
// get_proc_address / get_current_framebuffer are owned by the frontend;
// context_reset / context_destroy are provided by the core.
static struct retro_hw_render_callback hw_render;
static bool hw_render_active = false;

// Hardware-path present accounting (see MA_GL_take_present_stats): how many
// video_cb calls carried new pixels vs. were dupes, and what OUR own draw
// (normalize + chain + effect + overlay + HUD) costs.
static uint64_t mg_draw_us_sum = 0, mg_draw_us_max = 0;
static unsigned mg_new_frames = 0, mg_dupe_frames = 0;

// Frontend hw-render FBO. The core renders into this (glsm caches the id
// returned by get_current_framebuffer at context reset), and we present it
// centered onto the default framebuffer with a textured quad -- the
// RetroArch convention (core draws into the frontend FBO, frontend
// presents it).
// RA gl2: hw-render FBO textures are RARCH_SCALE_BASE(256) * input_scale
// squares (gl2.c:5415), where input_scale = MAX(next_pow2(MAX(max_w,
// max_h)) / 256, 1) (video_driver.c:4602-4604). tex_w = tex_h, and the
// core frame maps onto it with xamt/yamt = frame/tex (can be < 1).
#define RARCH_SCALE_BASE 256u

// Next power of 2 (RA libretro-common retro_math.h next_pow2).
static unsigned ma_gl_next_pow2(unsigned v) {
	v--;
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
	v++;
	return v;
}

// RA hw-render FBO size (video_driver.c:4602-4604 + gl2.c:5415):
// tex_w = tex_h = RARCH_SCALE_BASE * MAX(next_pow2(max_dim)/BASE, 1).
// max_dim = MAX(max_width, max_height) of the core's reported geometry;
// the frontend resizes its FBOs when that changes (runloop.c SET_SYSTEM_AV_INFO).
static unsigned ma_gl_fbo_dim(void) {
	unsigned max_dim = core.max_width > core.max_height
		? core.max_width : core.max_height;
	unsigned scale = ma_gl_next_pow2(max_dim) / RARCH_SCALE_BASE;
	if (scale < 1) scale = 1;
	return RARCH_SCALE_BASE * scale;
}
// Dual-buffered hw-render FBOs, mirroring RetroArch's gl2 renderchain
// (gl2_get_current_framebuffer returns hw_render_fbo[(tex_index+1)%
// textures]; the present samples the completed previous frame). The core
// renders into ma_gl_fbo[ma_gl_fbo_write] and the present samples
// ma_gl_fbo_tex[(ma_gl_fbo_write+1)%2] -- the frame completed last
// present -- so the same texture is never sampled in the same frame it
// was rendered to (RTT-vs-sample hazard on Mali).
#define MA_GL_FBO_COUNT 2
static GLuint ma_gl_fbo[MA_GL_FBO_COUNT] = { 0 };
static GLuint ma_gl_fbo_tex[MA_GL_FBO_COUNT] = { 0 };
static GLuint ma_gl_fbo_rb[MA_GL_FBO_COUNT] = { 0 };
static bool ma_gl_fbo_valid = false;
static unsigned ma_gl_fbo_write = 0; // core render target index
static unsigned ma_gl_fbo_dim_cur = 0; // actual created size (RA tex_w)

// Called by the core (through glsm) to resolve GL function pointers.
// Our context is a plain SDL GL context, so SDL_GL_GetProcAddress covers
// everything (it wraps eglGetProcAddress on the mali winsys).
static retro_proc_address_t ma_gl_get_proc_address(const char *sym) {
	if (!sym) return NULL;
	return (retro_proc_address_t)(uintptr_t)SDL_GL_GetProcAddress(sym);
}

// Returns the frontend FBO the core should render into. Must be current and
// complete before the core's context_reset runs (glsm reads it in SETUP).
// RA semantics (gl2_get_current_framebuffer): the core renders into the
// (tex_index+1) slot and the renderchain flips tex_index to that slot before
// sampling it -- write slot and sampled slot are the SAME texture, rotated
// across frames. So: core renders fbo[(write+1)%2], the present flips write
// to that slot and samples fbo[write].
static uintptr_t ma_gl_get_current_framebuffer(void) {
	return ma_gl_fbo_valid ? (uintptr_t)ma_gl_fbo[(ma_gl_fbo_write + 1) % MA_GL_FBO_COUNT] : 0;
}

// Index of the texture the present path samples: the slot the core rendered
// into THIS frame (= write after the flip at the end of the present).
static GLuint ma_gl_sample_tex(void) {
	return ma_gl_fbo_tex[ma_gl_fbo_write];
}

static bool ma_gl_create_fbo(void) {
	unsigned dim = ma_gl_fbo_dim();
	if (dim < 1) dim = 1;
	for (unsigned i = 0; i < MA_GL_FBO_COUNT; i++)
	{
		glGenTextures(1, &ma_gl_fbo_tex[i]);
		glBindTexture(GL_TEXTURE_2D, ma_gl_fbo_tex[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, dim, dim, 0,
				GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		// flycast glClears depth|stencil every frame -> FBO needs both.
		glGenRenderbuffers(1, &ma_gl_fbo_rb[i]);
		glBindRenderbuffer(GL_RENDERBUFFER, ma_gl_fbo_rb[i]);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
				dim, dim);

		glGenFramebuffers(1, &ma_gl_fbo[i]);
		glBindFramebuffer(GL_FRAMEBUFFER, ma_gl_fbo[i]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				GL_TEXTURE_2D, ma_gl_fbo_tex[i], 0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
				GL_RENDERBUFFER, ma_gl_fbo_rb[i]);

		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			ma_gl_fbo_valid = false;
	}
	ma_gl_fbo_valid = true;
	ma_gl_fbo_dim_cur = dim;

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	if (!ma_gl_fbo_valid)
		LOG_error("minarch: hw-render FBO incomplete\n");
	ma_gl_fbo_write = 0;
	return ma_gl_fbo_valid;
}

static void ma_gl_destroy_fbo(void) {
	for (unsigned i = 0; i < MA_GL_FBO_COUNT; i++)
	{
		if (ma_gl_fbo[i]) glDeleteFramebuffers(1, &ma_gl_fbo[i]);
		if (ma_gl_fbo_tex[i]) glDeleteTextures(1, &ma_gl_fbo_tex[i]);
		if (ma_gl_fbo_rb[i]) glDeleteRenderbuffers(1, &ma_gl_fbo_rb[i]);
		ma_gl_fbo[i] = ma_gl_fbo_tex[i] = ma_gl_fbo_rb[i] = 0;
	}
	ma_gl_fbo_valid = false;
	ma_gl_fbo_write = 0;
	ma_gl_fbo_dim_cur = 0;
}

// RA semantics (SET_SYSTEM_AV_INFO, runloop.c: no_video_reinit unless
// max_width/max_height unchanged): when the core reports a larger max
// geometry the hw-render FBOs must be rebuilt at the new RA size. Only the
// frontend GL objects change; the core's context_reset is invoked again so
// glsm re-reads get_current_framebuffer (RA re-inits the whole driver and
// re-runs the core's context_reset the same way).
void MA_GL_update_fbo_size(void) {
	if (!ma_gl_fbo_valid) return;
	unsigned dim = ma_gl_fbo_dim();
	if (dim == ma_gl_fbo_dim_cur) return;

	SDL_Window *win = PLAT_getGLWindow();
	SDL_GLContext ctx = PLAT_getGLContext();
	if (!win || !ctx) return;
	SDL_GL_MakeCurrent(win, ctx);

	LOG_info("minarch: hw-render FBO %ux%u -> %ux%u (av_info max %ux%u, RA next_pow2)\n",
		ma_gl_fbo_dim_cur, ma_gl_fbo_dim_cur, dim, dim,
		core.max_width, core.max_height);
	ma_gl_destroy_fbo();
	if (!ma_gl_create_fbo()) return;
	if (hw_render.context_reset)
		hw_render.context_reset();
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
// Why a quad for everything: glBlitFramebuffer cannot rotate 90 degrees,
// and its scaling filter kernel is implementation-defined while a sampler
// is spec-defined - one quad path keeps rotation, filtering, offsets and
// state management in a single place. Why a state reset after the quad:
// flycast's GLES2 renderer keeps
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
static GLuint ma_gl_present_vbo = 0;
static GLint  ma_gl_present_mvp_loc = -1;
static GLint  ma_gl_present_tex_loc = -1; // sampler uniform (RA set_params: glUniform1i)

static const char *ma_gl_present_vs =
	"#version 300 es\n"
	"layout(location = 0) in vec2 aPos;\n"
	"layout(location = 1) in vec2 aTex;\n"
	"uniform mat4 uMvp;\n"
	"out vec2 vTex;\n"
	"void main() { vTex = aTex; gl_Position = uMvp * vec4(aPos, 0.0, 1.0); }\n";

// RA gl2 GLSL stock fragment shader (gfx/drivers/gl_shaders/modern_opaque.
// glsl.frag.h): "Must enforce alpha = 1.0 or 32-bit games can potentially
// go black" -- the hw-render FBO is RGBA; cores (flycast) may leave the
// alpha channel at 0 and a passthrough that forwards it samples black.
static const char *ma_gl_present_fs =
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec2 vTex;\n"
	"uniform sampler2D uTex;\n"
	"out vec4 fragColor;\n"
	"void main() { fragColor = vec4(texture(uTex, vTex).rgb, 1.0); }\n";

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
	ma_gl_present_mvp_loc = glGetUniformLocation(ma_gl_present_prog, "uMvp");
	// RA gl_glsl_find_uniforms_frame: sampler uniform explicitly bound to
	// unit 0 in set_params (glUniform1i(uni->texture, texunit)) -- never
	// rely on the GLSL default.
	ma_gl_present_tex_loc = glGetUniformLocation(ma_gl_present_prog, "uTex");

	// RA gl_glsl_set_coords/set_attribs model (shader_glsl.c:701-731, 1709):
	// one VBO, re-uploaded every draw, attributes size=2/stride=0 on the
	// DEFAULT VAO (VAO 0 == RA's global attrib state) -- no private VAO.
	glGenBuffers(1, &ma_gl_present_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, ma_gl_present_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	LOG_info("minarch: present quad pipeline ready (prog=%u vbo=%u)\n",
		(unsigned)ma_gl_present_prog, (unsigned)ma_gl_present_vbo);
	return true;
}

// ---------------------------------------------------------------------------
// Frontend Screen Scaling -> target-side viewport (RetroArch GLES model).
//
// PORTING FIDELITY: RetroArch's GLES driver computes the destination with a
// pure target-side viewport model -- the keep-aspect/stretch branches of
// gl2_set_viewport (gfx/drivers/gl.c, ra-ref/gl19.c:384-478) plus
// video_viewport_get_scaled_integer for the integer branch (video_driver.c,
// ra-ref/retroarch19.c:32567-32639) -- and never touches the SOURCE rect:
// the whole core frame is mapped into the computed screen rect. Frontend
// mode mapping onto that model:
//   NATIVE        -> video_scale_integer: integer scale of the core base
//                    geometry, centered.
//   ASPECT        -> keep_aspect, desired = core.aspect_ratio (RA "Core
//                    provided", retroarch19.c:32111-32125).
//   ASPECT_SCREEN -> keep_aspect, desired = source (display) frame ratio.
//   FULLSCREEN    -> !keep_aspect: stretch to the whole screen (gl19.c:452).
//   CROPPED       -> video_scale_integer with CEILING scale: the same RA
//                    Integer Scale option as NATIVE (retroarch19.c:32567),
//                    only the stepping direction differs -- ceiling covers
//                    the screen and the symmetric overflow is clipped by the
//                    viewport. HDMI falls back to NATIVE (minarch rule,
//                    ma_video.c:451).
// We do NOT call the software functions (setRectToAspectRatio dereferences
// the software-only vid.blit state machine); the viewport math below is
// taken verbatim from the RA GLES viewport code referenced above.
//
// width/height: the frame's DISPLAY geometry -- for rotated games pass the
// width/height-swapped dims (quad path), mirroring RA's rotation handling.
// The result is the screen-space rect (it may extend past the screen; the
// viewport clips the overflow). The minarch-specific Screen X/Y offsets are
// NOT part of this rect: the present path applies them to the screen-space
// rect (see ma_gl_present_quad).
// ---------------------------------------------------------------------------
// RA-style Screen Scaling -> present rect (viewport model), shared by BOTH
// present paths (the software path calls it from PLAT_GL_Swap, the hw path
// from ma_gl_present_quad). See AGENTS.md「呈现/shader 链架构」.
void PLAT_compute_present_rect(int width, int height,
		int *out_x, int *out_y, int *out_w, int *out_h) {
	int scaling = screen_scaling;
	if (scaling == SCALE_CROPPED && DEVICE_WIDTH == HDMI_WIDTH)
		scaling = SCALE_NATIVE; // minarch rule: no crop on HDMI

	int dst_x = 0, dst_y = 0, dst_w = DEVICE_WIDTH, dst_h = DEVICE_HEIGHT;
	// RA video_viewport_get_scaled_aspect2: the viewport rect is computed
	// on the PHYSICAL device frame (vp->full_width/height); on rotation
	// the window is NOT swapped, only the desired aspect reciprocates
	// (video_driver_get_core_aspect, video_driver.c:2580-2583). The MVP
	// carries the rotation. width/height here are the ROTATED dims
	// (disp_w/height) only to identify rotation direction (same rule as
	// RA: rotation % 2).
	float device_aspect = (float)DEVICE_WIDTH / DEVICE_HEIGHT;
	// core.aspect_ratio comes straight from the core's av_info/geometry
	// (ma_core.c / ma_environment.c SET_SYSTEM_AV_INFO), which for a
	// rotated core (flycast ikaruga) is ALREADY the rotated aspect
	// (setGameGeometry: if rotate_screen aspect = 1/aspect). So desired
	// needs NO extra reciprocal here -- RA only reciprocates because it
	// stores the unrotated aspect and applies core_requested_rotation on
	// top. minarch has no such split: what the core reports is what we
	// fit.
	double desired = (scaling == SCALE_ASPECT_SCREEN)
		? (double)width / height
		: (core.aspect_ratio > 0 ? core.aspect_ratio : (double)width / height);

	if (scaling == SCALE_NATIVE || scaling == SCALE_CROPPED)
	{
		// video_viewport_get_scaled_integer (retroarch19.c:32567): integer
		// scale of the core base geometry. minarch does not cache
		// base_width/base_height, but flycast's reported base (640x480)
		// equals the presented frame and the software scaler uses the frame
		// size as its integer base too, so base == frame here. base_w is the
		// square-pixel correction base_h * aspect (retroarch19.c:32610;
		// aspect == core-reported ratio, which for flycast matches the frame;
		// for rotated games the passed dims are already swapped, so the base
		// follows the rotation the way RA swaps base_height, :32598).
		unsigned base_h = (height > 0) ? height : 1;
		unsigned base_w = (unsigned)roundf(base_h * (float)desired);
		if (DEVICE_WIDTH >= (int)base_w && DEVICE_HEIGHT >= (int)base_h)
		{
			unsigned max_scale = MIN(DEVICE_WIDTH / (int)base_w,
					DEVICE_HEIGHT / (int)base_h);
			if (scaling == SCALE_CROPPED)
				// Ceiling variant of RA's integer scale (video_scale_integer):
				// cover the screen; the overflow is clipped by the viewport
				// when rendering.
				max_scale = MIN(CEIL_DIV(DEVICE_WIDTH, (int)base_w),
						CEIL_DIV(DEVICE_HEIGHT, (int)base_h));
			dst_w = (int)(base_w * max_scale);
			dst_h = (int)(base_h * max_scale);
			// Centered (RA: vp->x = padding_x / 2, retroarch19.c:32637).
			dst_x = (DEVICE_WIDTH - dst_w) / 2;
			dst_y = (DEVICE_HEIGHT - dst_h) / 2;
		}
		else
		{
			// Source larger than the screen: the software path keeps it 1:1
			// and lets the screen crop it ("forced crop"); the viewport clips
			// the overflow here the same way.
			dst_w = width;
			dst_h = height;
			dst_x = (DEVICE_WIDTH - dst_w) / 2;
			dst_y = (DEVICE_HEIGHT - dst_h) / 2;
		}
	}
	else if (scaling == SCALE_FULLSCREEN)
	{
		// gl2_set_viewport, !keep_aspect branch (gl19.c:452-457): stretch to
		// the whole screen.
		dst_x = 0;
		dst_y = 0;
		dst_w = DEVICE_WIDTH;
		dst_h = DEVICE_HEIGHT;
	}
	else
	{
	// SCALE_ASPECT / SCALE_ASPECT_SCREEN only. RA keep_aspect:
	// video_viewport_get_scaled_aspect2(vp, full_width, full_height,
	// y_down=false, device_aspect, desired_aspect) on the PHYSICAL device
	// frame -- the window is NOT swapped on rotation and the rect stays in
	// physical coordinates; rotation only flips the desired aspect
	// (video_driver_get_core_aspect) and the MVP. minarch: desired already
	// flipped above for rotation%2.
	//
	// This fit must NOT run for NATIVE / CROPPED / FULLSCREEN: it used to be
	// an unconditional block that overwrote those rects, so Fullscreen
	// silently pillarboxed (desired != device) and Native/Cropped lost their
	// integer scale (e.g. GB 160x144 wanted 3x = 480x432, got a non-integer
	// 533-wide fit). RA keeps the three modes mutually exclusive.
		// vp_bias_x/y defaults are 0.5; y_down=false leaves bias_y as is,
		// so (0.5-delta)*(bias*2) == (0.5-delta).
		float delta;
		if (fabsf(device_aspect - (float)desired) < 0.0001f)
		{
			/* Screen and desired aspect ratios are numerically equal
			 * (gl19.c:426-432): full screen. */
		}
		else if (device_aspect > (float)desired)
		{
			/* Screen wider than desired -> pillarbox (gl19.c:433-437). */
			delta = ((float)desired / device_aspect - 1.0f) / 2.0f + 0.5f;
			dst_x = (int)roundf(DEVICE_WIDTH * (0.5f - delta));
			dst_w = (int)roundf(2.0f * DEVICE_WIDTH * delta);
		}
		else
		{
			/* Screen taller than desired -> letterbox (gl19.c:439-443). */
			delta = (device_aspect / (float)desired - 1.0f) / 2.0f + 0.5f;
			dst_y = (int)roundf(DEVICE_HEIGHT * (0.5f - delta));
			dst_h = (int)roundf(2.0f * DEVICE_HEIGHT * delta);
		}
	}

	// Screen X/Y offsets are NOT applied here: they are frontend-specific
	// (no RA equivalent) and applied by the present callers
	// (ma_gl_present_quad for hw cores, PLAT_GL_Swap for software cores) to
	// the SCREEN-space rect.

	*out_x = dst_x;
	*out_y = dst_y;
	*out_w = dst_w;
	*out_h = dst_h;
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
//
// This is the ONLY present path (landscape included). Rotation is the
// original reason a quad exists (glBlitFramebuffer cannot rotate 90 deg);
// folding landscape in unifies the rest: the sampler filter is the
// spec-defined texture filter (a scaling blit's LINEAR kernel is
// implementation-defined), and sharpness, offsets and core-state reset
// apply in one place. Performance is equivalent: a scaling blit on Mali is
// internally a sampling draw, same as this quad.

// RA gl_glsl_set_coords/set_attribs (shader_glsl.c:701-731, 1709-1800):
// upload the [vertex 2f x4][texcoord 2f x4] stream into the shared VBO and
// set both attributes (size=2, stride=0) on the DEFAULT VAO (VAO 0 = RA's
// global attrib state) right before the draw. Caller must have the present
// program current; attrib locations are 0=aPos/1=aTex (glBindAttribLocation).
static void ma_gl_present_draw(const GLfloat *coords) {
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, ma_gl_present_vbo);
	glBufferData(GL_ARRAY_BUFFER, 16 * sizeof(GLfloat), coords, GL_STREAM_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const GLvoid*)0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0,
			(const GLvoid*)(8 * sizeof(GLfloat)));
	glEnableVertexAttribArray(1);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// Draw a passthrough quad for the Screen Effect / Overlay textures. Unlike
// the FBO texture (content anchored at its GL bottom-left, v up), these RGBA
// surfaces have row 0 at the TOP, so v runs top-down on screen; UVs span the
// whole texture. The caller must have blending enabled (alpha compositing)
// and the present program + VAO/VBO already bound.
// Alpha-preserving passthrough program for the frontend's RGBA overlay
// surfaces (Screen Effect, Overlay, hw-render debug HUD).  The present
// program's fragment shader is RA's modern_opaque and forces alpha = 1 to keep
// 32-bit game textures from going black, and its vertex shader applies uMvp to
// already-clip-space vertices -- so overlay PNGs must NOT be drawn with it:
// their transparency would be lost (an opaque rectangle over the game) and the
// leftover uMvp would displace the quad.  Same contract as the software path's
// s_pass_overlay (factory overlay.glsl): plain texture passthrough, alpha kept,
// SRC_ALPHA blending.
static const char *ma_gl_overlay_vs =
	"#version 300 es\n"
	"layout(location = 0) in vec2 aPos;\n"
	"layout(location = 1) in vec2 aTex;\n"
	"out vec2 vTex;\n"
	"void main() { vTex = aTex; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
static const char *ma_gl_overlay_fs =
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec2 vTex;\n"
	"uniform sampler2D uTex;\n"
	"out vec4 fragColor;\n"
	"void main() { fragColor = texture(uTex, vTex); }\n";
static GLuint ma_gl_overlay_prog = 0;
static GLint  ma_gl_overlay_tex_loc = -1;

// Lazy program init (the GL context is current at every call site).  Draws
// through the shared present VBO/attrib layout (locations 0/1) that
// ma_gl_present_draw sets up.
static void ma_gl_overlay_use(void) {
	if (!ma_gl_overlay_prog) {
		GLuint vs = ma_gl_compile_shader(GL_VERTEX_SHADER, ma_gl_overlay_vs);
		GLuint fs = ma_gl_compile_shader(GL_FRAGMENT_SHADER, ma_gl_overlay_fs);
		if (vs && fs) {
			ma_gl_overlay_prog = glCreateProgram();
			glAttachShader(ma_gl_overlay_prog, vs);
			glAttachShader(ma_gl_overlay_prog, fs);
			glBindAttribLocation(ma_gl_overlay_prog, 0, "aPos");
			glBindAttribLocation(ma_gl_overlay_prog, 1, "aTex");
			glLinkProgram(ma_gl_overlay_prog);
			ma_gl_overlay_tex_loc = glGetUniformLocation(ma_gl_overlay_prog, "uTex");
		}
		if (vs) glDeleteShader(vs);
		if (fs) glDeleteShader(fs);
		if (!ma_gl_overlay_prog)
			LOG_error("minarch: overlay shader link failed\n");
	}
	glUseProgram(ma_gl_overlay_prog);
	if (ma_gl_overlay_tex_loc >= 0) glUniform1i(ma_gl_overlay_tex_loc, 0);
}

static void ma_gl_draw_overlay_quad(int x, int y, int w, int h, GLuint tex) {
	float cx0 = 2.0f * x / DEVICE_WIDTH - 1.0f;
	float cx1 = 2.0f * (x + w) / DEVICE_WIDTH - 1.0f;
	float cy0 = 1.0f - 2.0f * (y + h) / DEVICE_HEIGHT; // bottom
	float cy1 = 1.0f - 2.0f * y / DEVICE_HEIGHT;       // top

	// RA set_coords buffer layout: [vx..vy.. u.. v..]; NDC positions (old
	// layout kept for the overlay: viewport is fullscreen here), row 0 of
	// the surface (image top) maps to v = 0 (screen top).
	float coords[16] = {
		cx0, cy0,  cx1, cy0,  cx0, cy1,  cx1, cy1,   // vertex
		0.f, 1.f,  1.f, 1.f,  0.f, 0.f,  1.f, 0.f,   // texcoord
	};
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex);
	// Own program, sampler and blend state (GL discipline: never inherit a
	// binding).  The filter matches what the software path sets on these
	// textures (generic_video.c upload block): NEAREST.
	ma_gl_overlay_use();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	ma_gl_present_draw(coords);
}

// ===== Shader-chain source normalization (P2, NextUI contract) =====
// The chain's pass shaders derive their pattern from TextureSize/InputSize,
// and the Source/Texture Type contract only holds when the pass texture IS the
// pass content. The hw-render FBO is a fixed pow2 texture with the frame in
// its bottom-left region, so re-render that region into an exact-size texture
// first. SET_ROTATION is baked into the sample coordinates here: the factory
// system shaders (SYSTEM/*/shaders/default.glsl, noshader.glsl) hardcode
// gl_Position = vec4(VertexCoord, 0, 1), so the chain has no MVP left to
// rotate with (RA rotates in the final pass's projection instead).
//
// The quad is fullscreen; only the UVs rotate -- the same sampling table the
// no-shader game quad uses (derived from RA's projection: rot 1 = 90 deg CCW,
// rot 3 = 270 CCW). The output keeps the FBO's render convention (v = 1 =
// image top) so the no-flip final pass presents it upright.
//
// Returns 1 when the chain texture is ready, 0 when it cannot be built.
static GLuint ma_gl_chain_tex = 0, ma_gl_chain_fbo = 0;
static int ma_gl_chain_w = 0, ma_gl_chain_h = 0;

static int ma_gl_normalize_source(unsigned width, unsigned height) {
	if (!width || !height)
		return 0;
	if (!ma_gl_present_prog && !ma_gl_present_init())
		return 0;

	// Chain dims = ROTATED frame dims (swap for 90/270).
	int cw = width, ch = height;
	if (ma_gl_rotation % 2 == 1) { cw = height; ch = width; }

	if (!ma_gl_chain_tex || ma_gl_chain_w != cw || ma_gl_chain_h != ch) {
		if (ma_gl_chain_tex) glDeleteTextures(1, &ma_gl_chain_tex);
		if (ma_gl_chain_fbo) glDeleteFramebuffers(1, &ma_gl_chain_fbo);
		glGenTextures(1, &ma_gl_chain_tex);
		glBindTexture(GL_TEXTURE_2D, ma_gl_chain_tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, cw, ch, 0,
				GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glGenFramebuffers(1, &ma_gl_chain_fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, ma_gl_chain_fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				GL_TEXTURE_2D, ma_gl_chain_tex, 0);
		ma_gl_chain_w = cw;
		ma_gl_chain_h = ch;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, ma_gl_chain_fbo);
	glViewport(0, 0, cw, ch);

	// State resets BEFORE the clear: flycast can leave GL_SCISSOR_TEST enabled
	// with its own render window at frame end, and both the clear and the quad
	// would be clipped to it (partially uninitialized normalize output would
	// then flow through the chain).
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_DITHER);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(ma_gl_present_prog);
	if (ma_gl_present_mvp_loc >= 0) {
		// Plain ortho (0..1, 0..1): the unit quad fills the chain texture.
		float ortho[16];
		PLAT_compute_present_mvp(0, ortho);
		glUniformMatrix4fv(ma_gl_present_mvp_loc, 1, GL_FALSE, ortho);
	}
	if (ma_gl_present_tex_loc >= 0)
		glUniform1i(ma_gl_present_tex_loc, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, ma_gl_sample_tex());

	// Content region inside the fixed pow2 FBO.
	float tw = (float)width / ma_gl_fbo_dim_cur;
	float th = (float)height / ma_gl_fbo_dim_cur;

	// RA set_coords layout: [vertex 2f x4][texcoord 2f x4]; unit quad in
	// BL, BR, TL, TR order + the rotation-permuted content UVs.
	float coords[16] = {
		0.f, 0.f,  1.f, 0.f,  0.f, 1.f,  1.f, 1.f,
		0.f, 0.f,  tw,  0.f,  0.f, th,   tw,  th,
	};
	switch (ma_gl_rotation % 4) {
	case 1:
		coords[8]  = 0.f; coords[9]  = th;  coords[10] = 0.f; coords[11] = 0.f;
		coords[12] = tw;  coords[13] = th;  coords[14] = tw;  coords[15] = 0.f;
		break;
	case 2:
		coords[8]  = tw;  coords[9]  = th;  coords[10] = 0.f; coords[11] = th;
		coords[12] = tw;  coords[13] = 0.f; coords[14] = 0.f; coords[15] = 0.f;
		break;
	case 3:
		coords[8]  = tw;  coords[9]  = 0.f; coords[10] = tw;  coords[11] = th;
		coords[12] = 0.f; coords[13] = 0.f; coords[14] = 0.f; coords[15] = th;
		break;
	default: // 0
		break;
	}
	ma_gl_present_draw(coords);
	return 1;
}

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
	// Screen Scaling (Frontend menu) applies to the ROTATED geometry, via
	// the shared target-side viewport model (RA GLES, see
	// PLAT_compute_present_rect). The rect may
	// extend past the screen (NATIVE upscales/CROPPED covers) - the viewport
	// clips it; upscaling a rotated frame is filtered by the texture sampler.
	int dst_x = 0, dst_y = 0, dst_w = 0, dst_h = 0;
	PLAT_compute_present_rect((int)disp_w, (int)disp_h,
			&dst_x, &dst_y, &dst_w, &dst_h);
	// Screen X/Y offsets (frontend-specific, no RA equivalent): applied to
	// the SCREEN-space rect. NOTE the y sign: dst_y goes through the NDC
	// conversion below (cy = 1 - 2y/H, which treats dst_y as screen-y-down
	// and flips it), so the offset must be SUBTRACTED here for +screeny to
	// move the picture up on screen -- matching the software path's +y-up
	// behavior for every orientation. The offsets must NOT be applied in
	// the pre-rotation (content) space: that swaps x/y on rotated games.
	dst_x += screenx;
	dst_y -= screeny;

	// RA gl2 final pass geometry (gl2.c vertexes/vertexes_flipped +
	// gl2_set_projection): unit quad in 0..1, MVP = ortho(0..1,0..1)
	// (rotated by 90*rotation), texcoords map the FBO content region and do
	// NOT rotate. The viewport rect (dst*) maps the unit quad onto the
	// screen. bottom_left_origin=true -> vertex array v bottom-up like RA.
	// (Screen X/Y are already folded into dst_* above -- applying them a
	// second time here doubled every offset.)
	float tw = (float)width / ma_gl_fbo_dim_cur;
	float th = (float)height / ma_gl_fbo_dim_cur;

	// RA buffer layout (set_coords): [vx0..3, vy0..3, u0..3, v0..3].
	float coords[16] = { 0 };
	// Unit vertices, BL, BR, TL, TR (RA vertexes, bottom_left_origin).
	coords[0] = 0.f; coords[1] = 0.f;
	coords[2] = 1.f; coords[3] = 0.f;
	coords[4] = 0.f; coords[5] = 1.f;
	coords[6] = 1.f; coords[7] = 1.f;
	// Texcoords: content region, RA fbo_tex_coords (xamt/yamt).
	coords[8]  = 0.f;  coords[9]  = 0.f;
	coords[10] = tw;   coords[11] = 0.f;
	coords[12] = 0.f;  coords[13] = th;
	coords[14] = tw;   coords[15] = th;

	// MVP: mvp = rot(90*rotation) * ortho(0,1,0,1,-1,1), exactly RA
	// gl2_set_projection (gl2.c:1470-1499), shared with the shader-chain
	// final pass (single source of truth).
	float ortho[16];
	PLAT_compute_present_mvp(ma_gl_rotation, ortho);

	// Draw the quad with a clean pipeline. GL_FRAMEBUFFER (READ + DRAW)
	// binds to 0 here, which also pins the menu-capture contract: the
	// in-game menu's GFX_GL_screenCapture (glReadPixels) reads the READ
	// binding and must see the PRESENTED frame, not the 1024x1024 render
	// FBO (that would give the 640x480 content anchored bottom-left, game
	// shifted left with an empty band). This does not touch bindings;
	// glsm's next STATE_BIND restores the core's FBO
	// (default_framebuffer == ma_gl_fbo) at the start of retro_run.
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	// RA gl2_set_viewport: viewport = the aspect-fit pixel rect (unit quad +
	// ortho MVP map into it). Fullscreen only implicitly for full coverage.
	glViewport(dst_x, dst_y, dst_w, dst_h);
	// State resets BEFORE the clear (scissor: flycast can leave its own
	// render window enabled at frame end). RA
	// gl2_renderchain_restore_default_state (GLES branch, gl2.c:2468):
	// glDisable(DEPTH_TEST|CULL_FACE|DITHER); the remaining resets below
	// mirror gl2.c:4217-4222 (scissor/stencil/blend + blend funcs + clear
	// color).
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_DITHER);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBlendEquation(GL_FUNC_ADD);
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);

	if (GFX_shaders_active()) {
		// Shader chain (Frontend -> Shaders) on the SAME NextUI-contract
		// chain the software path uses: the normalize stage turns the pow2
		// FBO's content region into an exact-size texture (rotation baked),
		// so every pass sees texture == content and the Source/Texture Type
		// numbers are faithful. Rotation is already baked in, so the chain
		// gets rotation 0 and the no-flip final pass (factory noshader.glsl;
		// the FBO content is render convention, v = 1 = image top). The
		// source texture filter is the chain's first-pass filter (the
		// software rule where the chain overrides Screen Sharpness).
		if (ma_gl_normalize_source(width, height)) {
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, ma_gl_chain_tex);
			// GFX_first_shader_filter returns the GL constant directly
			// (GL_LINEAR / GL_NEAREST; 0 = no chain, not reachable here).
			int src_filter = GFX_first_shader_filter();
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, src_filter);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, src_filter);
			GFX_run_shader_pipeline(ma_gl_chain_tex, ma_gl_chain_tex,
					ma_gl_chain_w, ma_gl_chain_h, 1,
					dst_x, dst_y, dst_w, dst_h,
					(float)ma_gl_chain_w, (float)ma_gl_chain_h,
					0, 1);
		}
	} else {
		glUseProgram(ma_gl_present_prog);
		if (ma_gl_present_mvp_loc >= 0) {
			// RA gl2_set_projection mvp (rot 0: ortho(0..1,0..1)).
			glUniformMatrix4fv(ma_gl_present_mvp_loc, 1, GL_FALSE, ortho);
		}
		// RA set_params: sampler bound to unit 1 explicitly (texunit starts at 1).
		if (ma_gl_present_tex_loc >= 0)
			glUniform1i(ma_gl_present_tex_loc, 1);
		glActiveTexture(GL_TEXTURE1);
		// Dual-buffer present (RA gl2 semantics): sample the COMPLETED slot,
		// never the one the core rendered into this frame. RA set_params
		// binds the orig texture on unit 1 (texunit starts at 1) and sets
		// the sampler uniform to it.
		glBindTexture(GL_TEXTURE_2D, ma_gl_sample_tex());
		// Screen Sharpness: re-asserted every present (2 param calls,
		// trivial) so the sampler state stays authoritative regardless of
		// anything the core's glcache touched between frames.
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
				g_sharpness_linear ? GL_LINEAR : GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
				g_sharpness_linear ? GL_LINEAR : GL_NEAREST);
		ma_gl_present_draw(coords);
	}

	// Frontend Screen Effect + Overlay (software-path features mirrored
	// here, same draw order: game -> effect -> overlay). The effect
	// anchors at the game rect with its own size; the overlay is
	// fullscreen. Both are RGBA with alpha, so blending is on for these
	// two draws. The effect PNG density follows the integer scale the
	// software scaler would report, derived here from the present rect.
	int fx_scale_w = dst_w / (width ? (int)width : 1);
	int fx_scale_h = dst_h / (height ? (int)height : 1);
	int fx_scale = (fx_scale_w <= fx_scale_h) ? fx_scale_w : fx_scale_h;
	if (fx_scale < 1) fx_scale = 1;
	// The debug HUD prints the same renderer fields the software path fills in
	// its frame callback (src dims/scale, dst rect); hw cores have no
	// selectScaler step, so describe this present here.
	{
		int cw = GFX_shaders_active() ? ma_gl_chain_w : (int)width;
		int ch = GFX_shaders_active() ? ma_gl_chain_h : (int)height;
		int sc = (cw > 0) ? dst_w / cw : 1;
		renderer.src_w = cw;
		renderer.src_h = ch;
		renderer.src_x = 0;
		renderer.src_y = 0;
		renderer.scale = (sc < 1) ? 1 : sc;
		renderer.dst_x = dst_x;
		renderer.dst_y = dst_y;
		renderer.dst_w = dst_w;
		renderer.dst_h = dst_h;
	}
	GFX_setEffectScale(fx_scale);
	GFX_prepare_overlay_textures();
	// The shader chain runs on its own program (VAO 0 global attribs) and
	// leaves the viewport at the final pass's rect, and may leave TEXTURE1
	// bound; re-establish the present program's state (fullscreen viewport,
	// no scissor) before overlaying. ma_gl_present_draw re-uploads the quad
	// and attributes on the default VAO.
	glUseProgram(ma_gl_present_prog);
	glViewport(0, 0, DEVICE_WIDTH, DEVICE_HEIGHT);
	glDisable(GL_SCISSOR_TEST);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	int fx_w = 0, fx_h = 0, ov_w = 0, ov_h = 0;
	GLuint fx_tex = GFX_effect_texture(&fx_w, &fx_h);
	GLuint ov_tex = GFX_overlay_texture(&ov_w, &ov_h);
	if (fx_tex || ov_tex) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		if (fx_tex && fx_w > 0 && fx_h > 0)
			ma_gl_draw_overlay_quad(dst_x, dst_y, fx_w, fx_h, fx_tex);
		if (ov_tex && ov_w > 0 && ov_h > 0)
			ma_gl_draw_overlay_quad(0, 0, DEVICE_WIDTH, DEVICE_HEIGHT, ov_tex);
	}
}

void MA_GL_set_rotation(unsigned rotation) {
	ma_gl_rotation = rotation % 4;
	LOG_info("minarch: SET_ROTATION %u -> present rotates %u deg (quad path)\n",
		rotation, ma_gl_rotation * 90);
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

	// RA semantics: the hw-render FBO size follows the core's reported av_info
	// geometry max (gl2 tex_w = next_pow2(max_dim)); RA resolves av_info before
	// its driver builds the FBOs. minarch builds them inside SET_HW_RENDER
	// (during load_game), so resolve the geometry here -- otherwise the first
	// FBO is built from max=0 (256^2) and must be rebuilt mid-game, which
	// re-runs the core's context_reset while its render thread is live.
	if (core.get_system_av_info) {
		struct retro_system_av_info av = {};
		core.get_system_av_info(&av);
		core.max_width = av.geometry.max_width;
		core.max_height = av.geometry.max_height;
	}

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
	LOG_info("minarch: GLES hardware render enabled (context_type=%d, reset=%p, destroy=%p, fbo[%u]=%u/%u)\n",
		hw_render.context_type, (void*)hw_render.context_reset, (void*)hw_render.context_destroy,
		ma_gl_fbo_write, (unsigned)ma_gl_fbo[0], (unsigned)ma_gl_fbo[1]);

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

// ---------------------------------------------------------------------------
// hw-render debug HUD (flycast etc.).  hw-render cores never deliver a CPU
// frame, and minarch's original HUD (ma_video.c PLAT_draw_debug_hud) stamps
// its bitmap text into exactly such a frame -- so the SAME function runs here,
// onto a frontend RGBA surface, and the result is composited as one more
// overlay quad through ma_gl_draw_overlay_quad.  No second HUD implementation,
// no TTF panel and no seqlock handoff.
//
// It is rasterized on the core's video thread, which is where the software
// path runs the same function (upstream calls drawDebugHud from the video
// callback), so the threading model and the globals it reads (perf, renderer)
// are unchanged.  Half device resolution on a fullscreen quad reproduces the
// ~2x apparent text size the software path gets from its integer upscale.
// ---------------------------------------------------------------------------
static uint32_t *ma_gl_hud_surface = NULL;
static int ma_gl_hud_w = 0, ma_gl_hud_h = 0;
static GLuint ma_gl_hud_tex = 0;
static int ma_gl_hud_tex_sized = 0;

static void ma_gl_draw_debug_hud(void) {
	if (!show_debug || SDL_GetTicks() < 5000) return;

	int w = DEVICE_WIDTH / 2, h = DEVICE_HEIGHT / 2;
	if (w <= 0 || h <= 0) return;
	if (!ma_gl_hud_surface || ma_gl_hud_w != w || ma_gl_hud_h != h) {
		free(ma_gl_hud_surface);
		ma_gl_hud_surface = calloc((size_t)w * (size_t)h, 4);
		ma_gl_hud_w = w;
		ma_gl_hud_h = h;
		ma_gl_hud_tex_sized = 0;
	}
	if (!ma_gl_hud_surface) return;

	// Transparent background: the HUD's black text boxes and white glyphs are
	// opaque, everything else blends through to the game frame.
	memset(ma_gl_hud_surface, 0, (size_t)w * (size_t)h * 4);
	PLAT_draw_debug_hud(ma_gl_hud_surface, (unsigned)w, (unsigned)h,
			(size_t)w * 4, RETRO_PIXEL_FORMAT_XRGB8888);

	if (!ma_gl_hud_tex) {
		glGenTextures(1, &ma_gl_hud_tex);
		if (!ma_gl_hud_tex) return;
	}
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, ma_gl_hud_tex);
	if (!ma_gl_hud_tex_sized) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
				GL_UNSIGNED_BYTE, ma_gl_hud_surface);
		ma_gl_hud_tex_sized = 1;
	} else {
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA,
				GL_UNSIGNED_BYTE, ma_gl_hud_surface);
	}

	// Own draw state, then the shared overlay quad (which brings its own
	// program, filter and blend function).
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, DEVICE_WIDTH, DEVICE_HEIGHT);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	ma_gl_draw_overlay_quad(0, 0, DEVICE_WIDTH, DEVICE_HEIGHT, ma_gl_hud_tex);
}


void MA_GL_video_refresh(const void *data, unsigned width, unsigned height, size_t pitch) {
	if (!hw_render_active) return;
	// RetroArch semantics (gfx_video_driver.c video_driver_frame): the driver
	// frame callback runs on EVERY video_cb -- including dupes. For hw-render
	// cores a dupe (data == NULL) means "no new core pixels", but the GL
	// frame already lives in the frontend FBO, so we still present + swap it.
	// Skipping the swap on dupes (the old behavior) lets the SDL-renderer
	// presenters (notifications, indicators) cover the screen with a black
	// frame every time the core sends a dupe stretch -- which the 2026
	// flycast does a lot (threaded rendering queue ramp-up: 12+ frames).
	const void *VALID = (const void*)RETRO_HW_FRAME_BUFFER_VALID;
	if (data != VALID && data != NULL) return;
	if (!ma_gl_fbo_valid) return;

	// RA frame_cache semantics: dupes reuse the dimensions of the last real
	// frame (the core reports its internal framebufferWidth -- e.g. the
	// 853x480 startup estimate -- which is not the presented frame size).
	static unsigned last_w = 0, last_h = 0;
	// Dual-buffer rotation (RA gl2 semantics, gl2.c:4163-4164): the core
	// rendered into fbo[(write+1)%2] this frame; flip write to that slot so
	// the present samples what the core just drew. A dupe (data == NULL)
	// does NOT flip: the core produced no new frame, so we re-present the
	// same slot (RA keeps tex_index stable on dupes and re-samples it).
	if (data == VALID) {
		last_w = width; last_h = height;
		ma_gl_fbo_write = (ma_gl_fbo_write + 1) % MA_GL_FBO_COUNT;
	}
	if (last_w) { width = last_w; height = last_h; }

	// RA video_driver_frame semantics: a dupe (data == NULL) carries no new
	// pixels, so the driver frame is not re-run -- RA skips that frame whole.
	// We still swap, because this port composites notifications/indicators
	// through the SDL renderer on the same window (RA composites its widgets
	// into the GL frame), and the swap keeps the game frame on top of the
	// renderer's flip.  Measured dupe rate in play: 0 (flycast bursts them
	// only while its render queue ramps up).
	if (data == NULL) {
		SDL_Window *dwin = PLAT_getGLWindow();
		SDL_GLContext dctx = PLAT_getGLContext();
		mg_dupe_frames++;
		if (dwin && dctx) { SDL_GL_MakeCurrent(dwin, dctx); MA_present_frame(); }
		return;
	}

	SDL_Window *win = PLAT_getGLWindow();
	SDL_GLContext ctx = PLAT_getGLContext();
	if (!win || !ctx) return;

	SDL_GL_MakeCurrent(win, ctx);

	// Single present path for every orientation (landscape included): the
	// core renders into the frontend FBO and we present it as a textured
	// quad -- see ma_gl_present_quad. Rotation, scaling, sharpness, offsets
	// and core-state management each live in exactly one place.
	static uint64_t mg_freq = 0;
	if (!mg_freq) mg_freq = SDL_GetPerformanceFrequency();
	uint64_t mg_t0 = SDL_GetPerformanceCounter();
	ma_gl_present_quad(width, height);
	// Composite the HUD in the same place RA draws its widgets: end of the
	// driver frame.
	ma_gl_draw_debug_hud();
	{
		uint64_t us = (SDL_GetPerformanceCounter() - mg_t0) * 1000000ull / (mg_freq ? mg_freq : 1);
		mg_draw_us_sum += us;
		if (us > mg_draw_us_max) mg_draw_us_max = us;
		if (data == VALID) mg_new_frames++; else mg_dupe_frames++;
	}
	// The swap is deliberately NOT here: see MA_GL_present_from_loop.
}

// Hardware-path present accounting: how many video_cb calls actually carried
// new pixels vs. were dupes, and what OUR draw (normalize + chain + effect +
// overlay + HUD) costs.  A dupe still runs the whole present here, so if
// flycast's dupe bursts are what makes the swap wait, this count shows it.
void MA_GL_take_present_stats(unsigned* new_frames, unsigned* dupes,
		uint64_t* draw_sum_us, uint64_t* draw_max_us) {
	if (new_frames) *new_frames = mg_new_frames;
	if (dupes) *dupes = mg_dupe_frames;
	if (draw_sum_us) *draw_sum_us = mg_draw_us_sum;
	if (draw_max_us) *draw_max_us = mg_draw_us_max;
	mg_new_frames = 0; mg_dupe_frames = 0; mg_draw_us_sum = 0; mg_draw_us_max = 0;
}

// Present once per main-loop iteration, on the loop's own thread.  The hardware
// present used to swap from inside the video callback, which for a threaded
// renderer (flycast) means swapping on the core's render thread: its frames
// arrive in bursts, so the later swaps of a burst waited for the display to
// release a buffer (measured in doa2m: 55 of 146 swaps >1ms, 36 >8.3ms, max
// 10.1ms).  The software path has always presented on the loop thread, where
// the frame budget is already paced, and never blocks (measured in ddp3: max
// 0.43ms, zero over 1ms).  Drawing stays in the callback; only the swap moved.
void MA_GL_present_from_loop(void) {
	if (!hw_render_active) return;
	SDL_Window *win = PLAT_getGLWindow();
	SDL_GLContext ctx = PLAT_getGLContext();
	if (!win || !ctx) return;
	SDL_GL_MakeCurrent(win, ctx);
	// Debug HUD statistics for the hw path: minarch's original sampler with
	// current_fps left alone (SND_batchSamples resamples core audio by it and
	// must keep tracking the audio clock, not this loop).  Sampled here
	// because this is the hw path's once-per-frame loop point.
	if (show_debug) GFX_frame_stats_display_only(core.fps);
	MA_present_frame();
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
