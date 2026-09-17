#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

// minarch GLES hardware-render (glsm) frontend.
// Implements the frontend side of the libretro hardware-render contract for
// GLES cores (flycast et al.) on top of the existing SDL GL context:
//   - SET_HW_RENDER (GLES2/3/ES_VERSION): fills get_proc_address /
//     get_current_framebuffer and stashes the core's context_reset /
//     context_destroy callbacks.
//   - MA_GL_context_reset(): called by minarch once the GL context is
//     current and the game is loaded, so the core can init its GL state.
//   - MA_GL_video_refresh(): called from the video refresh callback when
//     data == RETRO_HW_FRAME_BUFFER_VALID, i.e. the frame is a GL
//     framebuffer rather than a pixel buffer.
//   - MA_GL_context_destroy(): called on exit to let the core free GL
//     resources while the context is still alive.

// Handle RETRO_ENVIRONMENT_SET_HW_RENDER. Returns true if the requested
// context type is supported (GLES only), false otherwise (so cores can
// fall back). Fills the callback fields the frontend owns.
bool MA_GL_set_hw_render(struct retro_hw_render_callback *cb);

// True once a core has successfully negotiated a GLES hardware-render context.
bool MA_GL_is_active(void);

// Core's context_reset callback, called by the frontend when the GL context
// is current and ready for the core (after retro_load_game).
void MA_GL_context_reset(void);

// Core's context_destroy callback, called by the frontend while the GL
// context is still alive during teardown.
void MA_GL_context_destroy(void);

// Called from the video refresh callback when data == RETRO_HW_FRAME_BUFFER_VALID.
// Presents the GL framebuffer the core rendered into.
void MA_GL_video_refresh(const void *data, unsigned width, unsigned height, size_t pitch);

// Re-draw the last presented hw-render frame (game + chain + overlays) into the
// currently bound framebuffer. Used by PLAT_GL_screenCapture to capture the
// presented frame deterministically instead of reading the post-swap window.
void MA_GL_present_draw(void);

// Take (and reset) the hardware-path present counters: new vs dupe frames and
// the cost of our own draw.  See the definitions in ma_gl.c.
void MA_GL_take_present_stats(unsigned* new_frames, unsigned* dupes,
		uint64_t* draw_sum_us, uint64_t* draw_max_us);

// Frame pacing deliberately has no entry point in this module: it lives in
// minarch.c (pace_frame), the run loop that owns both the audio-flag reset and
// the core.fps it schedules against.

// Handles RETRO_ENVIRONMENT_SET_ROTATION (0-3, multiples of 90 degrees).
// RetroArch convention: the core renders the image already oriented for the
// given rotation and the frontend must rotate its output to display it
// upright. Only meaningful while a GLES hw-render core is active (software
// cores like fbneo output their own orientation and are untouched).
void MA_GL_set_rotation(unsigned rotation);

// Re-make the hw-render GL context current (no-op when no GLES hw-render
// core is active). Call after frontend UI activity (Menu_loop) so the next
// retro_run executes the core's GL work with this frontend's one context
// current -- the cores cache GL object wrappers, so a wrong-current bind
// renders into phantom objects. See the definition for the full rationale.
void MA_GL_make_current(void);

// RA semantics (SET_SYSTEM_AV_INFO): the hw-render FBO size follows the
// core's reported geometry max dimensions (tex_w = next_pow2(max_dim)).
// Call when av_info.geometry.max_width/max_height change; rebuilds the
// frontend FBOs and re-runs the core's context_reset if the size grew.
void MA_GL_update_fbo_size(void);
