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
