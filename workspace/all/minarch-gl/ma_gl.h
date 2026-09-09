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

// Main-loop frame pacing for the hw-render path (call once per runloop
// iteration; does nothing when no hw-render core is active).
void MA_GL_frame_throttle(void);

// Handles RETRO_ENVIRONMENT_SET_ROTATION (0-3, multiples of 90 degrees).
// RetroArch convention: the core renders the image already oriented for the
// given rotation and the frontend must rotate its output to display it
// upright. Only meaningful while a GLES hw-render core is active (software
// cores like fbneo output their own orientation and are untouched).
void MA_GL_set_rotation(unsigned rotation);

// Re-make the hw-render GL context current (no-op when no GLES hw-render
// core is active). Call after any frontend activity that may have switched
// to another GL context (e.g. the SDL_Renderer used by the in-game menu)
// so the next retro_run executes the core's GL work in the right context.
void MA_GL_make_current(void);

// RA semantics (SET_SYSTEM_AV_INFO): the hw-render FBO size follows the
// core's reported geometry max dimensions (tex_w = next_pow2(max_dim)).
// Call when av_info.geometry.max_width/max_height change; rebuilds the
// frontend FBOs and re-runs the core's context_reset if the size grew.
void MA_GL_update_fbo_size(void);
