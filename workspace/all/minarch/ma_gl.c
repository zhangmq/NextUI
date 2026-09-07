#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>

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
	float device_aspect = (float)DEVICE_WIDTH / DEVICE_HEIGHT;
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
	else // SCALE_ASPECT / SCALE_ASPECT_SCREEN: gl2_set_viewport keep_aspect
	{
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

	// Draw the quad with a clean pipeline. GL_FRAMEBUFFER (READ + DRAW)
	// binds to 0 here, which also pins the menu-capture contract: the
	// in-game menu's GFX_GL_screenCapture (glReadPixels) reads the READ
	// binding and must see the PRESENTED frame, not the 1024x1024 render
	// FBO (that would give the 640x480 content anchored bottom-left, game
	// shifted left with an empty band). ma_gl_reset_core_state does not
	// touch bindings; glsm's next STATE_BIND restores the core's FBO
	// (default_framebuffer == ma_gl_fbo) at the start of retro_run.
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
	// Screen Sharpness: re-asserted every present (2 param calls, trivial)
	// so the sampler state stays authoritative regardless of anything the
	// core's glcache touched between frames.
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
			g_sharpness_linear ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
			g_sharpness_linear ? GL_LINEAR : GL_NEAREST);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	// Leave the state flycast's glcache expects (see ma_gl_reset_core_state).
	ma_gl_reset_core_state();
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

	// Single present path for every orientation (landscape included): the
	// core renders into the frontend FBO and we present it as a textured
	// quad -- see ma_gl_present_quad. Landscape previously used
	// glBlitFramebuffer; it was folded in so rotation, scaling, sharpness,
	// offsets and core-state management each live in exactly one place.
	ma_gl_present_quad(width, height);
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
