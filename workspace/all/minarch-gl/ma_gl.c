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
#include "ma_chain.h" // runShaderPass / s_pass_overlay (the shared overlay stage)

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

// ---------------------------------------------------------------------------
// FBO count: a RING of slots, because our cores render on their own thread.
//
// RA's plain hw-render contract is ONE FBO that the core renders into and that
// the present path samples directly:
//   gl3: gl3_init_hw_render() creates a single hw_render_fbo + hw_render_texture
//   pair (gl3.c:2068-2081), gl3_get_current_framebuffer() returns that FBO
//   verbatim (gl3.c:5610-5616) and the frame samples hw_render_texture
//   (gl3.c:2782). gl2: hw render forces gl->textures = 1 (gl2.c:5368-5372), so
//   hw_render_fbo[(tex_index + 1) % textures] (gl2.c:3214) and the tex_index
//   flip (gl2.c:4163) collapse to index 0 -- render target and sampled texture
//   are the same object.
//
// That is safe only while the core draws on the frontend's thread and in the
// frontend's context: the present then samples the frame strictly after the
// core finished writing it. A core that renders on ANOTHER thread with its own
// (shared) GL context -- ours does, and so do RA's threaded cores -- gets a
// RING instead: RA's gfx/video_thread_hw.c keeps VIDEO_THREAD_HW_RING (3) slots,
// hands the core hw_ring_framebuffer(index), advances the index on every
// published frame, and fences slots between the two threads ("the ring gives
// them what the swapchain gave them unthreaded"). The core renders into slot N
// while the present still samples slot N-1.
//
// An earlier dual-buffered variant here failed because the core kept rendering
// into the slot glsm cached at SETUP (get_current_framebuffer is called once
// there) while the present alternated: the picture came up every other frame
// (N64, "visible but flickering"). It works now because the core re-reads
// get_current_framebuffer() every frame and re-points its mirror FBO at the new
// slot's texture (yaba: yk_adopt_front_fbo / YuiGetFB).
//
// Fences: after presenting a slot we fence it (its read must complete before
// the core may render into it again), and before the core moves on to the next
// slot we wait that slot's fence if it is still in flight -- RA's
// hw_ring_capture/hw_ring_fence_wait split, with the waiting done here on the
// frontend's thread so the core needs no GL fence of its own.
// ---------------------------------------------------------------------------
#define MA_GL_FBO_COUNT 3
static GLuint ma_gl_fbo[MA_GL_FBO_COUNT] = { 0 };
static GLuint ma_gl_fbo_tex[MA_GL_FBO_COUNT] = { 0 };
static GLuint ma_gl_fbo_rb[MA_GL_FBO_COUNT] = { 0 };
static bool ma_gl_fbo_valid = false;
static unsigned ma_gl_fbo_dim_cur = 0; // actual created size (RA tex_w)
// Ring bookkeeping (frontend thread only): `write` is the slot the core is
// filling (what get_current_framebuffer returns), `present` the slot the last
// frame was presented from (what the present path samples).
static unsigned ma_gl_ring_write = 0, ma_gl_ring_present = 0;
static GLsync ma_gl_ring_sync[MA_GL_FBO_COUNT] = { 0 };
static bool ma_gl_ring_inflight[MA_GL_FBO_COUNT] = { false };
static unsigned ma_gl_ring_log = 0;   // bounded ring tracing (first frames)

// ---------------------------------------------------------------------------
// Ring opt-in: rotate only for a core that FOLLOWS the ring.
//
// glsm cores read hw_render.get_current_framebuffer() exactly once, in
// glsm_ctl(GLSM_CTL_STATE_SETUP) -- flycast glsm.c:2759, mupen64plus_next
// glsm.c:3049 -- and cache it as `default_framebuffer` (rglBindFramebuffer even
// redirects a bind of 0 to it).  Such a core renders into that one slot on the
// frontend's thread forever: if the present rotated anyway it would sample
// slots nobody ever wrote, which is exactly the "picture every other frame"
// failure the earlier two-buffer experiment produced on N64.
//
// Our core re-reads it every retro_run (yk_adopt_front_fbo) because its VDP
// thread needs the slot the frontend hands out next.  So instead of guessing
// from a name list, the frontend watches the calls: a request arriving between
// two presents means the core is following the ring; none means it cached the
// FBO and must keep the single-slot behaviour.  Rotation (and the extra slots,
// which cost colour+depth memory) start only after two consecutive presents
// that were followed, and the whole detector resets whenever the FBOs are
// recreated (a context_reset re-runs glsm's SETUP).
// ---------------------------------------------------------------------------
static unsigned ma_gl_ring_reqs = 0;       // get_current_framebuffer calls
static unsigned ma_gl_ring_reqs_mark = 0;  // value at the last present
static unsigned ma_gl_ring_follow = 0;     // consecutive presents that re-read
static unsigned ma_gl_ring_presents = 0;   // presents seen since (re)creation
static unsigned ma_gl_ring_slots = 1;      // slots actually created
static int ma_gl_fbo_depth = 0, ma_gl_fbo_stencil = 0; // what the core asked
static int ma_gl_ring_verdict = 0;         // 0 unknown, 1 single, 2 ring
static void ma_gl_ring_ensure_slots(void); // creates slots 1..N-1 on demand


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

// RA hw-render FBO size (video_driver.c:4602-4604 + gl2.c:5415 + gl3.c:3239):
// tex_w = tex_h = RARCH_SCALE_BASE * MAX(next_pow2(max_dim)/BASE, 1).
// max_dim = MAX(max_width, max_height) of the core's reported geometry.
// RA builds the FBO from this once per video-driver init, immediately before
// hwr->context_reset() (drivers_init -> video_driver_init_internal, then
// retroarch.c:1647-1648), which is what MA_GL_set_hw_render() below does.
static unsigned ma_gl_fbo_dim(void) {
	unsigned max_dim = core.max_width > core.max_height
		? core.max_width : core.max_height;
	unsigned scale = ma_gl_next_pow2(max_dim) / RARCH_SCALE_BASE;
	if (scale < 1) scale = 1;
	return RARCH_SCALE_BASE * scale;
}
// Called by the core (through glsm) to resolve GL function pointers.
// Our context is a plain SDL GL context, so SDL_GL_GetProcAddress covers
// everything (it wraps eglGetProcAddress on the mali winsys).
static retro_proc_address_t ma_gl_get_proc_address(const char *sym) {
	if (!sym) return NULL;
	return (retro_proc_address_t)(uintptr_t)SDL_GL_GetProcAddress(sym);
}

// Returns the frontend FBO the core should render into: the ring's WRITE slot.
// glsm caches whatever this returns at SETUP as `default_framebuffer`; a core
// that renders on another thread must re-read it every frame (ours does), which
// is exactly what makes the ring possible where the old dual-buffer attempt
// failed -- and the call counter below is how the ring decides whether this
// core does.  Called on the frontend's thread.
static uintptr_t ma_gl_get_current_framebuffer(void) {
	if (!ma_gl_fbo_valid) return 0;
	ma_gl_ring_reqs++;
	return (uintptr_t)ma_gl_fbo[ma_gl_ring_write];
}

// The texture the present path samples: the colour attachment of the slot the
// last published frame went into -- NOT the slot the core is filling now.
// Bound after glBindFramebuffer(GL_FRAMEBUFFER, 0) so this is not a
// render-to-sampled-texture feedback loop.
static GLuint ma_gl_sample_tex(void) {
	return ma_gl_fbo_tex[ma_gl_ring_present];
}

// ---------------------------------------------------------------------------
// Ring fences (frontend thread, frontend context).
//
// After a slot has been presented its texture is still being read by the GPU;
// the core must not draw into that slot again until the read finished.  A
// GLsync object per slot carries that: signal right after the present, wait
// before the slot is handed out again.  RA's hw_ring_capture/hw_ring_fence_wait
// do the same across its two threads (gl2.c: "place a fence after the core's
// rendering and flush, so the frame on the other thread can wait it").
// ---------------------------------------------------------------------------
typedef GLsync (*ma_gl_fencesync_fn)(GLenum, GLbitfield);
typedef GLenum (*ma_gl_clientwait_fn)(GLsync, GLbitfield, GLuint64);
typedef GLenum (*ma_gl_fencesync_wait_fn)(GLsync, GLbitfield, GLuint64);
typedef void   (*ma_gl_deletesync_fn)(GLsync);
static ma_gl_fencesync_fn      ma_gl_FenceSync;
static ma_gl_clientwait_fn     ma_gl_ClientWaitSync;
static ma_gl_fencesync_wait_fn ma_gl_WaitSync;
static ma_gl_deletesync_fn     ma_gl_DeleteSync;
static int ma_gl_sync_resolved = 0;

#ifndef GL_SYNC_GPU_COMMANDS_COMPLETE
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#endif
#ifndef GL_SYNC_FLUSH_COMMANDS_BIT
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#endif
#ifndef GL_ALREADY_SIGNALED
#define GL_ALREADY_SIGNALED 0x911A
#endif
#ifndef GL_TIMEOUT_EXPIRED
#define GL_TIMEOUT_EXPIRED 0x911B
#endif
#ifndef GL_CONDITION_SATISFIED
#define GL_CONDITION_SATISFIED 0x911C
#endif
#ifndef GL_WAIT_FAILED
#define GL_WAIT_FAILED 0x911D
#endif

static void ma_gl_sync_init(void) {
	if (ma_gl_sync_resolved) return;
	ma_gl_sync_resolved = 1;
	ma_gl_FenceSync      = (ma_gl_fencesync_fn)(uintptr_t)SDL_GL_GetProcAddress("glFenceSync");
	ma_gl_ClientWaitSync = (ma_gl_clientwait_fn)(uintptr_t)SDL_GL_GetProcAddress("glClientWaitSync");
	ma_gl_WaitSync       = (ma_gl_fencesync_wait_fn)(uintptr_t)SDL_GL_GetProcAddress("glWaitSync");
	ma_gl_DeleteSync     = (ma_gl_deletesync_fn)(uintptr_t)SDL_GL_GetProcAddress("glDeleteSync");
	LOG_info("minarch: hw-render ring fences: fence=%p clientwait=%p wait=%p delete=%p\n",
			(void *)ma_gl_FenceSync, (void *)ma_gl_ClientWaitSync,
			(void *)ma_gl_WaitSync, (void *)ma_gl_DeleteSync);
}

// The core just published a frame: it is in the WRITE slot, and that is what
// the present path must sample (before this, `present` still named the previous
// frame's slot).
static void ma_gl_ring_publish(void) {
	if (!ma_gl_fbo_valid) return;
	ma_gl_ring_present = ma_gl_ring_write;
}

// Called after the present of the current frame (frontend thread + context).
// First decide whether this core follows the ring at all (see the detector
// above); only then, and only for a following core:
//  1. fence the slot we just presented -- its read must finish before the core
//     may render into it again,
//  2. advance the write slot, so the core's next frame goes somewhere else and
//     can start while the GPU is still reading this one,
//  3. wait the new write slot's fence (armed when it was presented two frames
//     ago; normally long done).
// A caching core keeps its slot: no fence and no rotation, i.e. exactly the
// behaviour this frontend had before the ring existed.
static void ma_gl_ring_after_present(void) {
	unsigned presented;

	if (!ma_gl_fbo_valid) return;
	ma_gl_sync_init();
	presented = ma_gl_ring_present;
	ma_gl_ring_presents++;

	if (ma_gl_ring_reqs != ma_gl_ring_reqs_mark) {
		ma_gl_ring_reqs_mark = ma_gl_ring_reqs;
		if (ma_gl_ring_follow < 3) ma_gl_ring_follow++;
	} else {
		ma_gl_ring_follow = 0;
		/* the first present is ambiguous (glsm's SETUP read may be the only
		 * one so far): judge from the second one on. */
		if (ma_gl_ring_verdict == 0 && ma_gl_ring_presents > 1) {
			ma_gl_ring_verdict = 1;
			LOG_info("minarch: core caches the hw-render FBO (glsm SETUP) -- "
					"single slot, no ring rotation\n");
		}
	}

	if (MA_GL_FBO_COUNT < 2 || ma_gl_ring_follow < 2)
		return;   /* single-slot behaviour: the core renders where it presents */

	if (ma_gl_ring_verdict != 2) {
		ma_gl_ring_verdict = 2;
		ma_gl_ring_ensure_slots();
		LOG_info("minarch: core follows the hw-render ring -- rotating %u slots\n",
				(unsigned)MA_GL_FBO_COUNT);
	}

	if (ma_gl_FenceSync) {
		ma_gl_ring_sync[presented] = ma_gl_FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		ma_gl_ring_inflight[presented] = (ma_gl_ring_sync[presented] != NULL);
	}

	ma_gl_ring_write = (ma_gl_ring_write + 1) % MA_GL_FBO_COUNT;
	if (ma_gl_ring_log < 24)
	{
		LOG_info("minarch: ring present=%u next-write=%u fbo=%u tex=%u\n",
				presented, ma_gl_ring_write,
				(unsigned)ma_gl_fbo[presented], (unsigned)ma_gl_fbo_tex[presented]);
		ma_gl_ring_log++;
	}

	if (ma_gl_ring_inflight[ma_gl_ring_write]) {
		if (ma_gl_ClientWaitSync && ma_gl_ring_sync[ma_gl_ring_write]) {
			GLenum r = ma_gl_ClientWaitSync(ma_gl_ring_sync[ma_gl_ring_write], 0, 100000000ull);
			if (r == GL_TIMEOUT_EXPIRED)
				LOG_error("minarch: ring fence wait timed out on slot %u\n", ma_gl_ring_write);
		}
		if (ma_gl_DeleteSync && ma_gl_ring_sync[ma_gl_ring_write])
			ma_gl_DeleteSync(ma_gl_ring_sync[ma_gl_ring_write]);
		ma_gl_ring_sync[ma_gl_ring_write] = NULL;
		ma_gl_ring_inflight[ma_gl_ring_write] = false;
	}
}

// Ring reset: used whenever the FBOs are (re)created, so stale fences and slot
// indices never outlive the objects they refer to -- and so the follow detector
// starts over (a context_reset re-runs glsm's SETUP).
static void ma_gl_ring_reset(void) {
	for (unsigned i = 0; i < MA_GL_FBO_COUNT; i++) {
		if (ma_gl_ring_sync[i] && ma_gl_DeleteSync)
			ma_gl_DeleteSync(ma_gl_ring_sync[i]);
		ma_gl_ring_sync[i] = NULL;
		ma_gl_ring_inflight[i] = false;
	}
	ma_gl_ring_write = 0;
	ma_gl_ring_present = 0;
	ma_gl_ring_slots = 1;
	ma_gl_ring_reqs_mark = ma_gl_ring_reqs;
	ma_gl_ring_follow = 0;
	ma_gl_ring_presents = 0;
	ma_gl_ring_verdict = 0;
}

// One ring slot: colour texture + optional depth/stencil, both sized to the
// driver's power-of-two dim.  Returns true when the FBO is complete.
static bool ma_gl_create_slot(unsigned i, unsigned dim, int depth, int stencil) {
	glGenTextures(1, &ma_gl_fbo_tex[i]);
	glBindTexture(GL_TEXTURE_2D, ma_gl_fbo_tex[i]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, dim, dim, 0,
			GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	if (depth) {
		glGenRenderbuffers(1, &ma_gl_fbo_rb[i]);
		glBindRenderbuffer(GL_RENDERBUFFER, ma_gl_fbo_rb[i]);
		glRenderbufferStorage(GL_RENDERBUFFER,
				stencil ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT16,
				dim, dim);
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
	}

	glGenFramebuffers(1, &ma_gl_fbo[i]);
	glBindFramebuffer(GL_FRAMEBUFFER, ma_gl_fbo[i]);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_TEXTURE_2D, ma_gl_fbo_tex[i], 0);
	if (depth)
		glFramebufferRenderbuffer(GL_FRAMEBUFFER,
				stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
				GL_RENDERBUFFER, ma_gl_fbo_rb[i]);

	return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

// Slots 1..N-1 exist only for a core that follows the ring (see the detector
// above): a caching core never needs them, and each one costs a colour texture
// plus a depth/stencil renderbuffer at the driver's dimension.  Created here,
// on the frontend's thread with the frontend's context current, right after the
// present that proved the core follows.
static void ma_gl_ring_ensure_slots(void) {
	if (ma_gl_ring_slots >= MA_GL_FBO_COUNT) return;
	if (MA_GL_FBO_COUNT < 2) return;
	for (unsigned i = 1; i < MA_GL_FBO_COUNT; i++) {
		if (!ma_gl_create_slot(i, ma_gl_fbo_dim_cur, ma_gl_fbo_depth, ma_gl_fbo_stencil)) {
			ma_gl_fbo_valid = false;
			LOG_error("minarch: ring slot %u incomplete\n", i);
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glBindTexture(GL_TEXTURE_2D, 0);
			glBindRenderbuffer(GL_RENDERBUFFER, 0);
			return;
		}
	}
	ma_gl_ring_slots = MA_GL_FBO_COUNT;
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
}

// The depth/stencil attachment follows the CORE'S REQUEST, exactly like RA
// (gl3.c:2086-2099): no depth at all when hwr->depth is 0, a depth-only
// DEPTH_COMPONENT16 + GL_DEPTH_ATTACHMENT when the core asked for depth but
// not stencil, and DEPTH24_STENCIL8 + GL_DEPTH_STENCIL_ATTACHMENT otherwise.
// Attaching both unconditionally desyncs glsm's framebuffer bookkeeping: it
// records the attachment it sees on GL_DEPTH_ATTACHMENT (glsm.c:3063) and
// re-attaches it later on restore.
static bool ma_gl_create_fbo(int depth, int stencil) {
	unsigned dim = ma_gl_fbo_dim();
	if (dim < 1) dim = 1;
	ma_gl_fbo_depth = depth;
	ma_gl_fbo_stencil = stencil;
	ma_gl_fbo_valid = ma_gl_create_slot(0, dim, depth, stencil);
	ma_gl_fbo_dim_cur = dim;
	// Fresh objects: drop any fence from the old slots (their names are gone),
	// start the rotation over, and re-run the follow detector.
	ma_gl_ring_reset();

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	if (!ma_gl_fbo_valid)
		LOG_error("minarch: hw-render FBO incomplete\n");
	else
		LOG_info("minarch: hw-render FBO ready (%ux%u, up to %u ring slots on demand)\n",
				dim, dim, (unsigned)MA_GL_FBO_COUNT);
	return ma_gl_fbo_valid;
}

static void ma_gl_destroy_fbo(void) {
	ma_gl_ring_reset();
	for (unsigned i = 0; i < MA_GL_FBO_COUNT; i++)
	{
		if (ma_gl_fbo[i]) glDeleteFramebuffers(1, &ma_gl_fbo[i]);
		if (ma_gl_fbo_tex[i]) glDeleteTextures(1, &ma_gl_fbo_tex[i]);
		if (ma_gl_fbo_rb[i]) glDeleteRenderbuffers(1, &ma_gl_fbo_rb[i]);
		ma_gl_fbo[i] = ma_gl_fbo_tex[i] = ma_gl_fbo_rb[i] = 0;
	}
	ma_gl_fbo_valid = false;
	ma_gl_fbo_dim_cur = 0;
}

// Follows a growing core max geometry (RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO).
// RA answers that call with a full video-driver reinit when max_width or
// max_height changed -- runloop.c:2772-2847 sets DRIVER_VIDEO_MASK and issues
// CMD_EVENT_REINIT, i.e. context_destroy (core GL teardown) -> the driver's GL
// objects are rebuilt at the new size -> context_reset. minarch's only hw
// GL objects are these FBOs, so the same effect is: rebuild the FBO at the new
// RA size, then re-run the core's context_reset so glsm re-reads
// get_current_framebuffer.
//
// The FBO is NOT reused in place: the core caches the FBO name, so the new
// name has to reach it. mupen's glsm makes the re-reset the only workable
// carrier: with window_first already > 0, GLSM_CTL_STATE_CONTEXT_RESET runs
// glsm_state_setup() (glsm.c:3365-3375) and that is what re-reads
// default_framebuffer (glsm.c:3049); flycast calls GLSM_CTL_STATE_SETUP from
// its own context_reset unconditionally (libretro.cpp:1154-1163). Calling the
// core's context_destroy first would reset mupen's window_first to 0
// (glsm.c:3281-3282) so the reset would take the "first reset" branch and skip
// glsm_state_setup() entirely, leaving the core on the deleted FBO name.
//
// Not reached for the validated cores: both GLES cores report their final max
// geometry before SET_HW_RENDER -- mupen has a static 640x480
// (libretro.c:145-146) and flycast computes it from config::RenderResolution in
// update_variables(true) (libretro.cpp:1818, 1022) before
// set_opengl_hw_render (libretro.cpp:1921) -- so the FBO is built correct from
// the start and this only fires if a core grows its max mid-game.
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
	if (!ma_gl_create_fbo(hw_render.depth, hw_render.stencil)) return;
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
// No software scaler helper is involved (the CPU rect/scale functions the
// upstream software path used are gone); the viewport math below is taken
// verbatim from the RA GLES viewport code referenced above.
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
	// RA: aspect <= 0 means "core did not provide one"; the fallback is the
	// BASE geometry ratio (video_driver.c:2574-2577, and the libretro
	// contract: "If zero or less, an aspect ratio of base_width /
	// base_height is assumed") -- not the frame size.
	double desired = (scaling == SCALE_ASPECT_SCREEN)
		? (double)width / height
		: (core.aspect_ratio > 0 ? core.aspect_ratio
				: (core.base_width && core.base_height
					? (double)core.base_width / core.base_height
					: (double)width / height));

	if (scaling == SCALE_NATIVE || scaling == SCALE_CROPPED)
	{
		// video_viewport_get_scaled_integer (retroarch19.c:32567): integer
		// scale of the core *frame* size (RA uses frame_cache_width/height
		// with a base-geometry fallback when <= 4, video_driver.c:2869-2880),
		// so the frame height is the integer base here -- core.base_* only
		// backs the aspect fallback above. base_w is the square-pixel
		// correction base_h * aspect (retroarch19.c:32610).
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

// RGBA overlay surfaces (Screen Effect, Overlay, Notification, hw-render debug
// HUD) are composited by the ONE shared stage PLAT_composite_overlays
// (generic_video.c), through the factory overlay pass. There is deliberately no
// second overlay program here: the present program cannot carry them (its
// fragment shader is RA modern_opaque and forces alpha = 1; its vertex shader
// applies uMvp to already-clip-space vertices), and a private copy would drift
// from the software path's composite -- the previous ma_gl_overlay_prog did
// exactly that and never drew notifications at all.

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

// Last presented frame size, retained so the present draw can be replayed into
// an offscreen target by PLAT_GL_screenCapture (see MA_GL_present_draw).
static unsigned ma_gl_present_w = 0, ma_gl_present_h = 0;



static void ma_gl_present_quad(unsigned width, unsigned height);

// Re-draw the last presented frame (game + chain + overlays) into whatever
// framebuffer is bound -- the default one, or the capture target while
// PLAT_GL_screenCapture replays it.
void MA_GL_present_draw(void) {
	if (!ma_gl_present_w || !ma_gl_present_h) return;
	ma_gl_present_quad(ma_gl_present_w, ma_gl_present_h);
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
	// the SCREEN-space rect.  Both present paths feed this rect straight into
	// a GL viewport (glViewport here, runShaderPass -> glViewport on the
	// software path), i.e. y-up, so the SAME sign gives the same direction on
	// screen -- and that direction is the software path's, which is the
	// reference behaviour by decision (2026-09-17): +screeny moves the picture
	// UP.  (Stock minarch put the rect through SDL_RenderCopy, whose y grows
	// downwards, so its +screeny moved the picture down: a deliberate
	// divergence from stock, recorded here because the two are easy to
	// confuse.  The offsets must NOT be applied in the pre-rotation (content)
	// space: that swaps x/y on rotated games.)
	dst_x += screenx;
	dst_y += screeny;

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

	// Draw into the present target: the default framebuffer live, or the
	// offscreen capture target while PLAT_GL_screenCapture replays the present
	// (runShaderPass binds the same target for its final pass).  Captures read
	// that replay -- never the window, because glReadPixels on the default
	// framebuffer returns zeros on this driver.  This does not touch the core's
	// FBO; glsm's next STATE_BIND restores it (default_framebuffer == ma_gl_fbo)
	// at the start of retro_run.
	// Default framebuffer, or the offscreen capture target during a replay.
	glBindFramebuffer(GL_FRAMEBUFFER, PLAT_chain_present_target());
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
		// RA set_params binds the orig texture on unit 1 (texunit starts at 1)
		// and points the sampler uniform at it.  This frontend owns ONE
		// hw-render FBO (MA_GL_FBO_COUNT 1, no per-frame slot rotation), so
		// "orig" is simply that FBO's texture: there is no second slot.
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
	// Overlay composite: the ONE shared stage (generic_video.c
	// PLAT_composite_overlays), the same call the software present makes --
	// Screen Effect -> Overlay -> Notification, all through the factory
	// overlay pass. ma_gl.c used to carry a second implementation of this
	// (ma_gl_overlay_prog/ma_gl_draw_overlay_quad) which also silently never
	// drew notifications for hw-render cores (PLAT_GL_Swap is software-only).
	//
	// The chain runs on its own program (default VAO 0 global attribs) and
	// leaves the viewport at the final pass's rect, and may leave TEXTURE1
	// bound; reset those before overlaying. The shared stage re-establishes
	// its own program, VBO/attribs, filter and blend per pass.
	glUseProgram(ma_gl_present_prog);
	glViewport(0, 0, DEVICE_WIDTH, DEVICE_HEIGHT);
	glDisable(GL_SCISSOR_TEST);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	PLAT_composite_overlays(dst_x, dst_y,
			(GFX_shaders_active() && ma_gl_chain_tex)
				? ma_gl_chain_tex : ma_gl_sample_tex());
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

	// RA builds its hw-render FBO from the av_info geometry max
	// (video_driver.c:4602-4604 -> gl2.c:5415 / gl3.c:3239), and RA has that
	// av_info in hand before it creates the driver because it resolves content
	// av_info first. Our FBO is created here, inside SET_HW_RENDER (which both
	// GLES cores emit from within retro_load_game), so resolve the geometry
	// now: at this point both cores already report their final max
	// (mupen: static 640x480, libretro.c:145-146; flycast: set from
	// config::RenderResolution in update_variables(true), libretro.cpp:1818 +
	// 1022, before set_opengl_hw_render at :1921). RA's rule for a core that
	// reports a larger max later is a full video-driver reinit -- see
	// MA_GL_update_fbo_size().
	if (core.get_system_av_info) {
		struct retro_system_av_info av = {};
		core.get_system_av_info(&av);
		core.max_width = av.geometry.max_width;
		core.max_height = av.geometry.max_height;
	}

	// RA creates the hw-render FBO in video_driver_init_internal() and calls
	// hwr->context_reset() immediately after it (retroarch.c:1647-1648). The
	// core's glsm reads get_current_framebuffer() while handling that reset
	// (SETUP caches it: mupen glsm.c:3049, flycast glsm.c:2759), so the FBO
	// must exist first.
	// Stash the core's request before anything else: the FBO's depth/stencil
	// attachments are part of it.
	hw_render = *cb;

	if (!ma_gl_fbo_valid && !ma_gl_create_fbo(hw_render.depth, hw_render.stencil))
		return false;

	// Frontend-owned fields (RA overwrites exactly these: get_proc_address and
	// get_current_framebuffer). cache_context is the CORE's request
	// (libretro.h:2874-2878: "the frontend will go very far to avoid resetting
	// context"), and RA never rewrites it -- we do not either: this frontend
	// never resets the context after load, so honouring it is accurate.
	hw_render.get_proc_address      = ma_gl_get_proc_address;
	hw_render.get_current_framebuffer = ma_gl_get_current_framebuffer;

	// Give the core back the filled-in struct.
	*cb = hw_render;

	hw_render_active = true;
	LOG_info("minarch: GLES hardware render enabled (context_type=%d, reset=%p, destroy=%p, fbo=%u tex=%u dim=%u depth=%d stencil=%d bottom_left=%d cache_context=%d)\n",
		hw_render.context_type, (void*)hw_render.context_reset, (void*)hw_render.context_destroy,
		(unsigned)ma_gl_fbo[0], (unsigned)ma_gl_fbo_tex[0], ma_gl_fbo_dim_cur,
		(int)hw_render.depth, (int)hw_render.stencil,
		(int)hw_render.bottom_left_origin, (int)hw_render.cache_context);

	// Do NOT run the core's context_reset from here. Both GLES cores negotiate
	// hw render from *inside* retro_load_game (flycast: shell/libretro
	// set_opengl_hw_render; mupen64plus_next: libretro-common
	// glsm_state_ctx_init -> SET_HW_RENDER), and RA's context_reset is not
	// reachable from there: it is called by drivers_init() after
	// video_driver_init_internal() (retroarch.c:1647-1648), and drivers_init
	// runs after retro_load_game has returned. mupen64plus_next makes the
	// difference observable: its glsm treats a reset with window_first > 0 as a
	// window change and runs retroChangeWindow() (glsm.c:3365-3375), which
	// tears down GLideN64's drawer before RomOpen has created it (SIGSEGV in
	// Context::deleteFramebuffer). The one reset is issued after load_game
	// returns, from Core_load(); see ma_core.c.
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
	// Ring: a new frame is published into the WRITE slot; the present samples
	// that slot, then the ring advances so the core's next frame lands in a
	// different slot.  A dupe publishes nothing, so it re-presents the slot
	// the last real frame came from (see ma_gl_ring_publish/_after_present).
	if (data == VALID) {
		last_w = width; last_h = height;
		ma_gl_ring_publish();
	}
	if (last_w) { width = last_w; height = last_h; }
	// RA gl3.c:4773-4776: a zero reported size is fixed up to 1 so the
	// viewport math never divides by zero.
	if (!width) width = 1;
	if (!height) height = 1;

	// RA video_driver_frame semantics: EVERY video_cb runs the driver frame,
	// dupes included (video_driver.c: render_frame is only cleared by the
	// fast-forward frameskip branch), so a dupe still presents.  It must also
	// REDRAW, not just swap: with two window back buffers a bare swap flips to
	// the buffer two presents old, which is the frame-timing flicker seen when
	// the core sends dupes (frameskip on).  Redrawing re-fills both buffers
	// with the same finished slot.
	if (data == NULL) {
		SDL_Window *dwin = PLAT_getGLWindow();
		SDL_GLContext dctx = PLAT_getGLContext();
		mg_dupe_frames++;
		if (dwin && dctx) {
			SDL_GL_MakeCurrent(dwin, dctx);
			if (show_debug) GFX_frame_stats_display_only(core.fps);
			if (ma_gl_present_w && ma_gl_present_h)
				ma_gl_present_quad(ma_gl_present_w, ma_gl_present_h);
			MA_present_frame();
		}
		// RA counts every video_cb, dupes included (video_driver.c:6011),
		// because the shader chain's FrameCount is a frame counter.
		frame_count++;
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
	// The one hw-render present draw: normalize -> shader chain -> overlays.
	ma_gl_present_w = width;
	ma_gl_present_h = height;
	// The debug HUD is NOT drawn here any more: it is one layer of the shared
	// overlay composite (generic_video.c PLAT_composite_overlays), which
	// ma_gl_present_quad already ran, so the hw-render HUD is produced by the
	// same code, at the same point, as the software path's.
	ma_gl_present_quad(width, height);

	{
		uint64_t us = (SDL_GetPerformanceCounter() - mg_t0) * 1000000ull / (mg_freq ? mg_freq : 1);
		mg_draw_us_sum += us;
		if (us > mg_draw_us_max) mg_draw_us_max = us;
		// Only VALID frames reach this point: NULL returned above (dupe
		// branch) and every other pointer returned at the top.
		mg_new_frames++;
	}

	// Present here, in the video frame callback -- RetroArch's shape
	// (video_driver.c: every video_cb runs the driver frame, which flips).
	// The alternative tried earlier, one swap per main-loop iteration, was
	// measured indistinguishable in the swap statistics on both hw cores:
	// doa2m 0 blocking swaps either way, N64 4.1-4.2 ms average and 68-71 of
	// ~250 swaps over 8.3 ms either way.  The blocking is display-paced (a
	// 50 fps core against a 60 Hz panel waits, a 31 fps core does not) and
	// ma_pace.c already consumes that same signal as RA's PACE_VSYNC, so the
	// divergence had no measured benefit to pay for it.
	if (show_debug) GFX_frame_stats_display_only(core.fps);
	MA_present_frame();
	// The presented slot is now being read: fence it, then hand the core the
	// next ring slot (see ma_gl_ring_after_present).
	ma_gl_ring_after_present();
	// Same counter as the software path's (PLAT_GL_Swap): every presented
	// frame advances it, so FrameCount-driven shaders animate on hw cores too.
	frame_count++;
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

// Re-make our GL context current after frontend UI activity, so the next
// retro_run's glsm STATE_BIND + core render start from a known-current
// context: flycast/GLideN64 cache GL object wrappers (GLCache / Context), and
// a core that binds with the wrong context current renders into phantom
// objects (frozen frame) while polluting its shadow state (rendering
// corruption after the menu). The frontend owns exactly ONE GL context
// (generic_video.c:625); the original need came from the deleted
// SDL_Renderer's separate context, and the call is kept as the explicit
// ordering point (one MakeCurrent, idempotent). The game present path
// (MA_GL_video_refresh) makes it current every frame anyway, so this only
// matters for the window between Menu_loop and the next retro_run.
void MA_GL_make_current(void) {
	if (!hw_render_active) return;
	SDL_Window *win = PLAT_getGLWindow();
	SDL_GLContext ctx = PLAT_getGLContext();
	if (win && ctx) SDL_GL_MakeCurrent(win, ctx);
}
