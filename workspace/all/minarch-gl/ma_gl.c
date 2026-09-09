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
static GLuint ma_gl_sample_fbo(void) {
	return ma_gl_fbo[ma_gl_fbo_write];
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
static void ma_gl_compute_present_rect(int width, int height,
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
	// RA keep-aspect: video_viewport_get_scaled_aspect2(vp, full_width,
	// full_height, y_down=false, device_aspect, desired_aspect) on the
	// PHYSICAL device frame -- the window is NOT swapped on rotation and
	// the rect stays in physical coordinates; rotation only flips the
	// desired aspect (video_driver_get_core_aspect) and the MVP.
	// minarch: desired already flipped above for rotation%2.
	{
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
	// (no RA equivalent) and applied by the single present caller
	// (ma_gl_present_quad) to the SCREEN-space rect, with the y sign
	// adjusted for its NDC conversion so +screeny moves the picture up.

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
	// Same filter the software path sets on these textures (generic_video.c
	// upload block): NEAREST.
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	ma_gl_present_draw(coords);
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
	// ma_gl_compute_present_rect). The rect may
	// extend past the screen (NATIVE upscales/CROPPED covers) - the viewport
	// clips it; upscaling a rotated frame is filtered by the texture sampler.
	int dst_x = 0, dst_y = 0, dst_w = 0, dst_h = 0;
	ma_gl_compute_present_rect((int)disp_w, (int)disp_h,
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
	// Screen X/Y offsets: applied to the pixel rect (dst_y is screen-y-down;
	// RA has no offset concept, this mirrors the software path's +y-up).
	dst_x += screenx;
	dst_y -= screeny;

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
		// Shader chain (Frontend -> Shaders): RA gl2 semantics -- pass 0
		// samples the hw-render FBO texture DIRECTLY (its content region is
		// the frame dims inside the pow2 FBO), the chain passes cascade
		// through pow2 FBOs in the core's orientation, and the final pass
		// applies SET_ROTATION in its MVP (gl2_renderchain_render). The
		// source texture filter is the chain's first-pass filter (the
		// software rule where the chain overrides Screen Sharpness).
		GLuint src_tex = ma_gl_sample_tex();
		glBindTexture(GL_TEXTURE_2D, src_tex);
		// GFX_first_shader_filter returns the GL constant directly
		// (GL_LINEAR / GL_NEAREST; 0 = no chain, not reachable here).
		int src_filter = GFX_first_shader_filter();
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, src_filter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, src_filter);
		GFX_run_shader_pipeline(src_tex, src_tex,
				width, height, 1, dst_x, dst_y, dst_w, dst_h,
				(float)ma_gl_fbo_dim_cur, (float)ma_gl_fbo_dim_cur,
				ma_gl_rotation);
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

// RA runloop pacing for the hw-render path: the frame budget is applied on
// the main loop (the thread that drives retro_run), never inside the core's
// video callback (which runs on the core's render thread -- sleeping there
// throttles the core itself). The video callback presents without sleeping;
// the main loop calls this once per frame.
void MA_GL_frame_throttle(void) {
	if (!hw_render_active) return;
	ma_gl_throttle(core.fps);
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

	SDL_Window *win = PLAT_getGLWindow();
	SDL_GLContext ctx = PLAT_getGLContext();
	if (!win || !ctx) return;

	SDL_GL_MakeCurrent(win, ctx);

	const char *db = getenv("MINARCH_DIRECT_BLIT");
	const char *qs = getenv("MINARCH_QUAD_STUB");
	if (db && db[0] == '1') {
		// Reset the state flycast leaves behind (scissor in particular),
		// bind the core FBO as READ source and blit to the default fb.
		glDisable(GL_SCISSOR_TEST);
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_CULL_FACE);
		glDisable(GL_STENCIL_TEST);
		glDisable(GL_BLEND);
		// Dual-buffer present: blit the COMPLETED slot.
		glBindFramebuffer(GL_READ_FRAMEBUFFER, ma_gl_sample_fbo());
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		glViewport(0, 0, DEVICE_WIDTH, DEVICE_HEIGHT);
		// Clear the draw fb first so letterbox bars are black, not garbage.
		glClearColor(0.f, 0.f, 0.f, 1.f);
		glClear(GL_COLOR_BUFFER_BIT);
		// Source rect = the content region of the 1024^2 FBO (bottom-left
		// anchored, flycast renders with bottom_left_origin=true).
		unsigned src_w = width, src_h = height;
		if (src_w > ma_gl_fbo_dim_cur) src_w = ma_gl_fbo_dim_cur;
		if (src_h > ma_gl_fbo_dim_cur) src_h = ma_gl_fbo_dim_cur;
		glBlitFramebuffer(0, 0, (GLint)src_w, (GLint)src_h,
			0, 0, DEVICE_WIDTH, DEVICE_HEIGHT,
			GL_COLOR_BUFFER_BIT, GL_NEAREST);
		SDL_GL_SwapWindow(win);
	} else if (qs && qs[0] == '1') {
		// QUAD-STUB: one step past direct-blit -- present the core FBO
		// TEXTURE through the present program's fullscreen quad instead of
		// blitting the attachment. Same full-screen stretch (no aspect,
		// rotation, offsets, shader chain). If this is black while the blit
		// shows the game, the break is between "FBO attachment" and "FBO
		// texture sampled by a quad"; if it shows the game too, the quad
		// path itself is fine and the break is further down the original
		// pipeline (normalize / chain / effect / overlay).
		if (!ma_gl_present_prog && !ma_gl_present_init())
			return;

		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glViewport(0, 0, DEVICE_WIDTH, DEVICE_HEIGHT);
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_CULL_FACE);
		glDisable(GL_STENCIL_TEST);
		glDisable(GL_SCISSOR_TEST);
		glDisable(GL_BLEND);
		glClearColor(0.f, 0.f, 0.f, 1.f);
		glClear(GL_COLOR_BUFFER_BIT);

		glUseProgram(ma_gl_present_prog);
		// Full-screen quad (RA unit vertexes + ortho MVP), UVs map the
		// content region of the RA-sized FBO.
		float tw = (float)width / ma_gl_fbo_dim_cur;
		float th = (float)height / ma_gl_fbo_dim_cur;
		float coords[16] = {
			0.f, 0.f,  1.f, 0.f,  0.f, 1.f,  1.f, 1.f,   // vertex
			0.f, 0.f,  tw,  0.f,  0.f, th,   tw,  th,    // texcoord
		};
		if (ma_gl_present_mvp_loc >= 0) {
			static const float ortho[16] = {
				2.f, 0.f, 0.f, 0.f,
				0.f, 2.f, 0.f, 0.f,
				0.f, 0.f, 1.f, 0.f,
				-1.f, -1.f, 0.f, 1.f,
			};
			glUniformMatrix4fv(ma_gl_present_mvp_loc, 1, GL_FALSE, ortho);
		}
		// Sample the slot the core just rendered into (dual-buffer write slot
		// after the VALID flip above).
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, ma_gl_sample_tex());
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		ma_gl_present_draw(coords);
		SDL_GL_SwapWindow(win);
	} else {
		// Single present path for every orientation (landscape included): the
		// core renders into the frontend FBO and we present it as a textured
		// quad -- see ma_gl_present_quad. Landscape previously used
		// glBlitFramebuffer; it was folded in so rotation, scaling, sharpness,
		// offsets and core-state management each live in exactly one place.
		ma_gl_present_quad(width, height);
		SDL_GL_SwapWindow(win);
	}
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
