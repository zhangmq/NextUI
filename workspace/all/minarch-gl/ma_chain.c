// See ma_chain.h.  Shader chain state and execution.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL2/SDL.h>
#include <GLES3/gl3.h>

#include "ma_chain.h"
#include "ma_internal.h"

int reloadShaderTextures = 1;
int shaderResetRequested = 0;
int s_final_vp_w = 0;
int s_final_vp_h = 0;


ShaderProgram s_shader_default = {0};
ShaderProgram s_shader_overlay = {0};
ShaderProgram s_noshader = {0};



ShaderProgram shader_programs[MAXSHADERS];
ShaderPass shaders[MAXSHADERS];

// memcpy these in initShaders()
const ShaderProgram blank_shader_program = {
	.shader_p = 0, .filename = "stock.glsl"
};
const ShaderPass blank_shader_pass = { .program = NULL,
	.alpha = 0, .target_texture = 0, .target_updated = 1
};

ShaderPass s_pass_finalscale = { .program = &s_shader_default,
	.filter = GL_NEAREST,
	.alpha = 0, .target_texture = 0, .target_updated = 1
};

ShaderPass s_pass_effect = { .program = &s_shader_overlay,
	.alpha = 1, .target_texture = 0, .target_updated = 1
};

ShaderPass s_pass_overlay = { .program = &s_shader_overlay,
	.alpha = 1, .target_texture = 0, .target_updated = 1
};

ShaderPass s_pass_notif = { .program = &s_shader_overlay,
	.alpha = 1, .target_texture = 0, .target_updated = 1
};

// Frontend UI (menu) surfaces: the menu draws with straight-alpha SDL blits
// onto a transparent surface, so the composited pixels are PREMULTIPLIED
// (dst = src.rgb*src.a over nothing). The SDL renderer presented them with a
// premultiplied blend mode for the same reason; this pass keeps that exact
// semantics while drawing through the shared GL overlay program.
ShaderPass s_pass_ui = { .program = &s_shader_overlay,
	.alpha = 1, .premult = 1, .target_texture = 0, .target_updated = 1
};


int nrofshaders = 0; // choose between 1 and 3 pipelines, > pipelines = more cpu usage, but more shader options and shader upscaling stuff
// No-flip passthrough pass for the hw-render pipeline's final scale (the
// chain textures are render-convention; see PLAT_run_shader_pipeline).
ShaderPass s_noshader_pass = { .program = &s_noshader,
	.filter = GL_NEAREST, .alpha = 0, .target_texture = 0, .target_updated = 1
};

///////////////////////////////


int orig_w = 0;
int orig_h = 0;
int origtex_w = 0;
int origtex_h = 0;

// Next power of two (RA libretro-common retro_math.h next_pow2). The chain
// pass FBOs are pow2-sized with the pass content in the bottom-left region,
// exactly like RA's fbo_rect width/height (gl2.c:4125-4131).
static unsigned gl_next_pow2(unsigned v) {
	v--;
	v |= v >> 1; v |= v >> 2; v |= v >> 4;
	v |= v >> 8; v |= v >> 16;
	return v + 1;
}

// RA gl2_set_projection (gl2.c:1470-1499): mvp = Rz(90*rotation) * ortho,
// ortho = matrix_4x4_ortho(0,1,0,1,-1,1). Column-major data
// (data[4*col+row], glUniformMatrix4fv GL_FALSE):
//   ortho: col0=(2,0,0,0) col1=(0,2,0,0) col2=(0,0,-1,0) col3=(-1,-1,0,1)
//   rot:   col0=(cos,sin,0,0) col1=(-sin,cos,0,0)
// matrix_4x4_multiply(mvp, rot, ortho): mvp.data[4c+r] =
//   sum_k rot.data[4k+r] * ortho.data[4c+k]. rotation 0 -> ortho itself.
void PLAT_compute_present_mvp(int rotation, float out[16]) {
	float ortho[16] = {
		2.f, 0.f, 0.f, 0.f,
		0.f, 2.f, 0.f, 0.f,
		0.f, 0.f, -1.f, 0.f,
		-1.f, -1.f, 0.f, 1.f,
	};
	float radians = 3.14159265f * (90.f * rotation) / 180.f;
	float cosine = cosf(radians), sine = sinf(radians);
	float rot[16] = {
		cosine,  sine, 0.f, 0.f,
		-sine,   cosine, 0.f, 0.f,
		0.f,     0.f,    1.f, 0.f,
		0.f,     0.f,    0.f, 1.f,
	};
	for (int row = 0; row < 4; row++) {
		for (int col = 0; col < 4; col++) {
			float s = 0.f;
			for (int k = 0; k < 4; k++)
				s += rot[k*4+row] * ortho[col*4+k];
			out[col*4+row] = s;
		}
	}
}

// GL pixel-store state for FRONTEND CPU uploads (overlay/effect/notification
// PNGs, the hw-render debug HUD surface).
//
// The frontend uploads its RGBA surfaces with glTexImage2D/glTexSubImage2D from
// a tightly packed buffer, but the unpack state is GLOBAL GL state and the core
// owns it while it runs: glsm/GLideN64 set GL_UNPACK_ROW_LENGTH for their own
// texture streams and glsm's per-frame STATE_BIND does not reset it (it resets
// program/FBO/viewport/texture/attribs only). A leftover row length makes the
// frontend's upload read each row with the wrong stride, so the image arrives
// skewed -- observed on the N64 hw path as a 1024x768 overlay PNG appearing as
// two vertical bands instead of four quadrants.
//
// RA never relies on residual unpack state either: it sets GL_UNPACK_ROW_LENGTH
// per upload from the real pitch (gl3.c:4134-4152, gl2.c:2547/2603). Ours are
// tightly packed, so row length 0 (the default) is the correct value.
void PLAT_gl_unpack_reset(void) {
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

// See ma_chain.h. Static, module-private state: the present draw consults it
// every time it would otherwise bind the default framebuffer.
static GLuint s_present_target_fbo = 0;
void PLAT_chain_set_present_target(GLuint fbo) { s_present_target_fbo = fbo; }
GLuint PLAT_chain_present_target(void) { return s_present_target_fbo; }

// RA chain-pass projection: mvp_no_rot (gl2.c:1482-1488) —
// matrix_4x4_ortho(0,1,0,1,-1,1), column-major.
static const float gl_chain_mvp[16] = {
	2.f, 0.f, 0.f, 0.f,
	0.f, 2.f, 0.f, 0.f,
	0.f, 0.f, -1.f, 0.f,
	-1.f, -1.f, 0.f, 1.f,
};

void runShaderPass(ShaderPass * shader_pass, GLuint src_texture,
				   GLuint orig_texture_src, GLuint * target_texture, int next_filter,
                   int x, int y, int dst_width, int dst_height,
				   const float mvp[16], int unit_quad, int exact_fbo) {

	// RA gl_glsl_use/set_coords/set_attribs model (shader_glsl.c): NO VAO.
	// The pipeline draws with the default VAO 0 (= RA's global attrib state
	// on GLES2) and rebuilds the vertex/texcoord stream + attrib pointers
	// on every pass. Attribs enabled for a program are disabled again when
	// the program switches (gl_glsl_reset_attrib), so no array from one
	// program/VAO leaks into another draw.
	static GLuint static_VBO = 0;
	static GLint pass_enabled_locs[8] = { 0 };
	static int pass_enabled_cnt = 0;
	static GLfloat texelSize[2] = {-1.0f, -1.0f};
	static GLuint fbo = 0;
	static GLint max_tex_size = 0;
	static int logged_bad_size = 0;
	GLenum pre_err;

	if (!shader_pass) return;

	ShaderProgram * shader_program = shader_pass->program;

	if (!shader_program || !shader_program->shader_p) {
		shader_program = &s_noshader;
	}

	const GLuint shader_program_handle = shader_program->shader_p;

	while ((pre_err = glGetError()) != GL_NO_ERROR) {
		(void)pre_err;
	}

	if (max_tex_size == 0) {
		glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex_size);
		if (max_tex_size <= 0) {
			max_tex_size = 2048;
		}
	}

	if (dst_width <= 0 || dst_height <= 0 || dst_width > max_tex_size || dst_height > max_tex_size) {
		if (!logged_bad_size) {
			LOG_error("Shader pass invalid target size: %dx%d (max %d)\n", dst_width, dst_height, max_tex_size);
			logged_bad_size = 1;
		}
		return;
	}

	if (shaderResetRequested) {
		// Force rebuild of GL objects and cached state
		for (int e = 0; e < pass_enabled_cnt; e++)
			glDisableVertexAttribArray(pass_enabled_locs[e]);
		pass_enabled_cnt = 0;
		if (static_VBO) { glDeleteBuffers(1, &static_VBO); static_VBO = 0; }
		texelSize[0] = texelSize[1] = -1.0f;
		fbo = 0;
	}

	// RA gl_glsl_set_params (shader_glsl.c:1409-1428): this pass's source
	// is described by srcw/srch (content size -> InputSize) and texw/texh
	// (source texture size -> TextureSize); the pass viewport is the
	// target (OutputSize). Sampler coords cover the source's CONTENT
	// region: RA fbo_tex_coords xamt/yamt = img_width/width, img_height/
	// height (gl2.c:1555-1558, 1618-1622).
	//
	// exact_fbo (software path, NextUI convention): the pass texture IS the
	// pass content (NPOT, no pow2 padding), so the sampler coords always
	// span the whole texture (uv = 1) and the reported texw/texh are the
	// logical Source/Texture Type values. Note texelSize keeps deriving from
	// the REPORTED texw/texh in both modes: that is exactly what upstream
	// NextUI feeds its shaders (generic_video.c runShaderPass: 1.0f/texw).
	float uv_w = 1.0f, uv_h = 1.0f;
	if (!exact_fbo) {
		uv_w = (shader_pass->texw > 0) ? (float)shader_pass->srcw / shader_pass->texw : 1.0f;
		uv_h = (shader_pass->texh > 0) ? (float)shader_pass->srch / shader_pass->texh : 1.0f;
		if (uv_w > 1.0f) uv_w = 1.0f;
		if (uv_h > 1.0f) uv_h = 1.0f;
	}

	texelSize[0] = 1.0f / shader_pass->texw;
	texelSize[1] = 1.0f / shader_pass->texh;

	// The program must be re-established from the ACTUAL GL binding, never from
	// a module-local cache. This pipeline does not own the context exclusively:
	// ma_gl.c binds its own present program around the chain
	// (ma_gl_present_quad re-binds ma_gl_present_prog before compositing the
	// overlays, and ma_gl_normalize_source binds it for the normalize quad), and
	// the hw-render debug HUD / overlay draws go through this same function.
	// A cached `last_program` therefore goes stale, and the overlay then runs
	// under the PRESENT program -- whose fragment shader forces alpha = 1 and
	// whose vertex shader applies the present MVP -- so the overlay is drawn at
	// the wrong scale/position with its transparency lost. Exactly the failure
	// mode AGENTS.md records for residual GL state.
	// Other per-pass cached state (texelSize, fbo) does not depend on foreign
	// bindings, but the program does; querying it costs one glGetIntegerv.
	{
		GLint current_program = 0;
		glGetIntegerv(GL_CURRENT_PROGRAM, &current_program);
		if ((GLuint)current_program != shader_program_handle) {
			// RA gl_glsl_use (shader_glsl.c:1802-1814): before switching
			// programs, disable the attrib arrays the previous program enabled
			// (gl_glsl_reset_attrib). Leftover enabled arrays with pointers into
			// this pipeline's VBO would otherwise be read by later draws that
			// bind the same VAO 0 / global attrib slots.
			for (int e = 0; e < pass_enabled_cnt; e++)
				glDisableVertexAttribArray(pass_enabled_locs[e]);
			pass_enabled_cnt = 0;
			glUseProgram(shader_program_handle);
		}
	}

	// RA gl_glsl_set_coords/set_attribs (shader_glsl.c:1709-1800, 701-731):
	// draw on the default VAO 0 (the GLES3 name for RA's global attrib
	// state) with one VBO. Stream layout: [texcoord 2f/vertex x4][vertex
	// 2f/vertex x4]; each attrib is size=2, stride=0, set right before the
	// draw and ARRAY_BUFFER unbinds immediately after (gl2.c / RA never
	// rely on a residual binding; binding VAO 0 here also stops pointers
	// from being captured into whichever VAO another draw left bound).
	if (static_VBO == 0)
		glGenBuffers(1, &static_VBO);
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, static_VBO);

	GLfloat stream[16];
	if (unit_quad) {
		// RA chain/final geometry: unit quad (gl2.c fbo_vertexes, 0..1)
		// + source content-region texcoords (xamt/yamt); bottom_left_origin
		// keeps the FBO content bottom-up (v=0 = texture row 0). Same
		// values RA puts in coords->tex_coord / coords->vertex.
		const GLfloat tex[8]  = { 0.f, 0.f, uv_w, 0.f, 0.f, uv_h, uv_w, uv_h };
		const GLfloat vert[8] = { 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f };
		memcpy(stream, tex, sizeof(tex));
		memcpy(stream + 8, vert, sizeof(vert));
	} else {
		// NextUI-only effect/overlay/notification passes (no RA
		// equivalent): fullscreen clip-space quad + full texture, exactly
		// the pre-alignment behavior.
		const GLfloat tex[8]  = { 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f, 0.f };
		const GLfloat vert[8] = { -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f, -1.f };
		memcpy(stream, tex, sizeof(tex));
		memcpy(stream + 8, vert, sizeof(vert));
	}
	glBufferData(GL_ARRAY_BUFFER, sizeof(stream), stream, GL_STREAM_DRAW);

	// Attribute locations are per-program, queried by name on every pass
	// (RA re-runs gl_glsl_set_coords per draw).
	GLint posAttrib = glGetAttribLocation(shader_program_handle, "VertexCoord");
	GLint texAttrib = glGetAttribLocation(shader_program_handle, "TexCoord");
	if (posAttrib >= 0 && pass_enabled_cnt < 8) {
		glVertexAttribPointer(posAttrib, 2, GL_FLOAT, GL_FALSE, 0,
				(const GLvoid*)(8 * sizeof(GLfloat)));
		glEnableVertexAttribArray(posAttrib);
		pass_enabled_locs[pass_enabled_cnt++] = posAttrib;
	}
	if (texAttrib >= 0 && pass_enabled_cnt < 8) {
		glVertexAttribPointer(texAttrib, 2, GL_FLOAT, GL_FALSE, 0, (const GLvoid*)0);
		glEnableVertexAttribArray(texAttrib);
		pass_enabled_locs[pass_enabled_cnt++] = texAttrib;
	}
	// RetroArch unbinds ARRAY_BUFFER right after capturing the pointers, so
	// a later bind by another draw cannot affect this pipeline's state.
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	// RA gl_glsl_set_params runs on EVERY pass (gl2_renderchain_render
	// calls set_params per pass): the size uniforms change between passes,
	// so a program re-used across passes must still get fresh values.
	if (shader_program->u_FrameDirection >= 0) glUniform1i(shader_program->u_FrameDirection, 1);
	if (shader_program->u_FrameCount >= 0) glUniform1i(shader_program->u_FrameCount, frame_count);
	if (shader_program->u_OutputSize >= 0) glUniform2f(shader_program->u_OutputSize, dst_width, dst_height);
	if (shader_program->u_TextureSize >= 0) glUniform2f(shader_program->u_TextureSize, shader_pass->texw, shader_pass->texh);
	if (shader_program->u_InputSize >= 0) glUniform2f(shader_program->u_InputSize, shader_pass->srcw, shader_pass->srch);
	if (shader_program->u_OrigTextureSize >= 0) glUniform2f(shader_program->u_OrigTextureSize, origtex_w, origtex_h);
	if (shader_program->u_OrigInputSize >= 0) glUniform2f(shader_program->u_OrigInputSize, orig_w, orig_h);
	if (shader_program->u_FinalViewportSize >= 0)
		glUniform2f(shader_program->u_FinalViewportSize,
				(float)(s_final_vp_w > 0 ? s_final_vp_w : dst_width),
				(float)(s_final_vp_h > 0 ? s_final_vp_h : dst_height));
	for (int i = 0; i < shader_program->num_pragmas; ++i) {
		glUniform1f(shader_program->pragmas[i].uniformLocation, shader_program->pragmas[i].value);
	}
	// A pass without an explicit MVP (NextUI's clip-space quads: the system
	// shaders hardcode gl_Position = vec4(VertexCoord, 0, 1) while community
	// shaders multiply by MVPMatrix) gets the IDENTITY matrix, exactly like
	// upstream NextUI. Leaving the uniform untouched kept it at 0, which
	// collapsed every vertex to the origin (invisible overlay / effect /
	// notification passes).
	if (shader_program->u_MVP >= 0) {
		static const float identity_mvp[16] = {
			1.f, 0.f, 0.f, 0.f,
			0.f, 1.f, 0.f, 0.f,
			0.f, 0.f, 1.f, 0.f,
			0.f, 0.f, 0.f, 1.f,
		};
		glUniformMatrix4fv(shader_program->u_MVP, 1, GL_FALSE, mvp ? mvp : identity_mvp);
	}

	if (target_texture) {
		if (*target_texture != 0 && !glIsTexture(*target_texture)) {
			*target_texture = 0;
			shader_pass->target_updated = 1;
		}
		unsigned tw, th;
		int need_alloc;
		if (exact_fbo) {
			// NextUI: the pass texture is exactly the pass content (NPOT,
			// texture == content). Recreated whenever that size changes --
			// there is no pow2 slack to absorb a smaller pass.
			tw = (unsigned)dst_width;
			th = (unsigned)dst_height;
			need_alloc = (*target_texture==0 || shader_pass->target_updated || reloadShaderTextures
					|| (int)tw != shader_pass->target_w
					|| (int)th != shader_pass->target_h);
		}
		else {
			// RA gl2_frame resize handling (gl2.c:4099-4157): pass FBO textures
			// are recreated whenever the pass content outgrows the current pow2
			// allocation -- checked every frame, not only on explicit reloads,
			// because the core's reported frame size can change mid-stream (the
			// 853x480 startup estimate -> 640x238 -> 640x480 transitions). A
			// 640x238 frame reallocates to pow2 1024x256; the following
			// 640x480 frame would then render past the texture without this
			// grow check.
			tw = gl_next_pow2(dst_width);
			th = gl_next_pow2(dst_height);
			need_alloc = (*target_texture==0 || shader_pass->target_updated || reloadShaderTextures
					|| tw > (unsigned)shader_pass->target_w
					|| th > (unsigned)shader_pass->target_h);
		}
		if (need_alloc) {
			if(*target_texture==0)
				glGenTextures(1, target_texture);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, *target_texture);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, next_filter);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, next_filter);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			// exact_fbo: the texture is exactly the pass size (NextUI).
			// Otherwise RA pass FBOs are pow2-sized (fbo_rect width/height =
			// next_pow2 of the content, gl2.c:4125-4131); the content
			// occupies the bottom-left [0..dst] region, like the
			// hw-render source FBO.
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
					tw, th,
					0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			shader_pass->target_w = (int)tw;
			shader_pass->target_h = (int)th;
			shader_pass->target_updated = 0;
		}
		if (fbo == 0) {
			glGenFramebuffers(1, &fbo);
		}

        // Always bind before attaching to avoid stale state after swaps
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);

		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *target_texture, 0);

		GLenum  err = glGetError();
		if (err != GL_NO_ERROR) {
			LOG_error("Framebuffer error: %d\n", err);
			LOG_info("Failed to bind framebuffer with texture %u\n", *target_texture);
		}

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            LOG_error("Framebuffer incomplete: 0x%X\n", status);
        }

		// RA clears each pass FBO before drawing into it
		// (gl2_renderchain_render, gl2.c:1581; pass0: gl2_frame:4249).
		glClear(GL_COLOR_BUFFER_BIT);

    } else {
        // No pass target of its own (final present pass, effect/overlay/UI
        // composite): the default framebuffer, or the offscreen capture target
        // while PLAT_GL_screenCapture replays the present.
        glBindFramebuffer(GL_FRAMEBUFFER, PLAT_chain_present_target());
    }

	if(shader_pass->alpha==1) {
		glEnable(GL_BLEND);
		// premult: source RGB already carries its alpha (frontend UI surfaces,
		// the SDL-renderer texture blend modes did the same).
		glBlendFunc(shader_pass->premult ? GL_ONE : GL_SRC_ALPHA,
				GL_ONE_MINUS_SRC_ALPHA);
	} else {
		glDisable(GL_BLEND);
	}

	// RA binds the pass source texture on every pass (gl2.c:1573, 1647) —
	// no cross-pass caching: a texture handle can be recycled by a
	// reload/rebuild between frames, which would otherwise leave the unit
	// bound to a deleted object.
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, src_texture);
	glViewport(x, y, dst_width, dst_height);

	if (shader_program->u_Texture >= 0) glUniform1i(shader_program->u_Texture, 0);

	if (shader_program->u_OrigTexture >= 0) {
		glUniform1i(shader_program->u_OrigTexture, 1);
		glActiveTexture(GL_TEXTURE0+1);
		glBindTexture(GL_TEXTURE_2D, orig_texture_src);
		glActiveTexture(GL_TEXTURE0);
	}
	
	if (shader_program->u_texelSize >= 0) {
		glUniform2fv(shader_program->u_texelSize, 1, texelSize);
	}
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// Run the configured shader chain (0..MAXSHADERS passes) followed by the
// finalscale pass into dst_rect. Shared by BOTH present paths: the software
// path passes its uploaded frame texture (also used as the OrigTexture
// uniform source), the hw-render path passes its pow2 hw-render FBO texture
// directly (RA gl2 semantics -- no separate normalize step). Consumes
// reloadShaderTextures (chain metadata and texture params are rebuilt when
// set). No software-only state (vid.blit) is referenced: frame dimensions
// arrive as parameters.
void PLAT_run_shader_pipeline(GLuint src_texture, GLuint orig_texture_src,
		int frame_w, int frame_h, int final_noflip,
		int dst_x, int dst_y, int dst_w, int dst_h,
		float src_tex_w, float src_tex_h, int rotation, int nextui_mode) {
	int last_w = frame_w, last_h = frame_h;
	float last_tex_w = src_tex_w, last_tex_h = src_tex_h;

	// RA gl_glsl_set_params' "original frame" (params.info): the pipeline
	// source, with its full texture size (tex_size) and content size
	// (input_size). hw path: the pow2 hw-render FBO (1024 x 1024, content
	// bottom-left [0..frame]); software path: the exact-size uploaded
	// frame texture (tex == content).
	orig_w = frame_w; orig_h = frame_h;
	origtex_w = (int)src_tex_w; origtex_h = (int)src_tex_h;
	// FinalViewportSize uniform: the present rect, constant for all passes.
	s_final_vp_w = dst_w;
	s_final_vp_h = dst_h;

	for (int i = 0; i < nrofshaders; i++) {
		int src_w = last_w;
		int src_h = last_h;
		int pass_dst_w = src_w * shaders[i].scale;
		int pass_dst_h = src_h * shaders[i].scale;

		if (shaders[i].scale == 9) {
			pass_dst_w = dst_w;
			pass_dst_h = dst_h;
		}

		// RA gl2_renderchain_render per-pass params (gl2.c:1555-1609):
		// srcw/srch/texw/texh describe THIS pass's source (InputSize /
		// TextureSize uniforms); the pass viewport (OutputSize) is the
		// target dims. The sampler coords sample the source content
		// region (xamt = srcw/texw), like RA fbo_tex_coords.
		//
		// nextui_mode (software path): the numbers reported to the shader
		// follow the pass's own Source Type / Texture Type options --
		// source (0) = the core frame, relative (1) = this pass's real
		// input, screen (2) = the present rect (upstream generic_video.c
		// GFX_blitRenderer). They are recomputed every pass, not only when
		// the shader is reloaded, so a menu change applies immediately
		// (upstream refreshed them only inside `if (reloadShaderTextures)`,
		// which is why changing Texture Type appeared to do nothing).
		if (nextui_mode) {
			int srctype = shaders[i].srctype, scaletype = shaders[i].scaletype;
			shaders[i].srcw = (srctype == 0) ? frame_w : (srctype == 2) ? dst_w : src_w;
			shaders[i].srch = (srctype == 0) ? frame_h : (srctype == 2) ? dst_h : src_h;
			shaders[i].texw = (scaletype == 0) ? frame_w : (scaletype == 2) ? dst_w : src_w;
			shaders[i].texh = (scaletype == 0) ? frame_h : (scaletype == 2) ? dst_h : src_h;
		}
		else {
			shaders[i].srcw = src_w;
			shaders[i].srch = src_h;
			shaders[i].texw = (int)last_tex_w;
			shaders[i].texh = (int)last_tex_h;
		}

		static int shaderinfocount = 0;
		static int shaderinfoscreen = 0;
		if (shaderinfocount > 600 && shaderinfoscreen == i) {
			currentshaderpass = i + 1;
			currentshadertexw = shaders[i].texw;
			currentshadertexh = shaders[i].texh;
			currentshadersrcw = shaders[i].srcw;
			currentshadersrch = shaders[i].srch;
			currentshaderdstw = pass_dst_w;
			currentshaderdsth = pass_dst_h;
			shaderinfocount = 0;
			shaderinfoscreen++;
			if (shaderinfoscreen >= nrofshaders)
				shaderinfoscreen = 0;
		}
		shaderinfocount++;

		runShaderPass(
			&shaders[i],
			(i == 0) ? src_texture : shaders[i - 1].target_texture,
			orig_texture_src,
			&shaders[i].target_texture,
			(i == nrofshaders - 1) ? s_pass_finalscale.filter : shaders[i+1].filter,
			0, 0, pass_dst_w, pass_dst_h,
			nextui_mode ? NULL : gl_chain_mvp,
			nextui_mode ? 0 : 1,
			nextui_mode);

		last_w = pass_dst_w;
		last_h = pass_dst_h;
		// NextUI pass FBOs are exactly the pass size (content == texture).
		// RA's are pow2: the next pass's source texture size is that pow2,
		// its content the pass dims.
		if (nextui_mode) {
			last_tex_w = (float)pass_dst_w;
			last_tex_h = (float)pass_dst_h;
		}
		else {
			last_tex_w = (float)gl_next_pow2(pass_dst_w);
			last_tex_h = (float)gl_next_pow2(pass_dst_h);
		}
	}

	GLuint final_src = (nrofshaders > 0) ? shaders[nrofshaders - 1].target_texture : src_texture;
	// Final scale-to-screen pass: RA gl2_renderchain_render's last block
	// (gl2.c:1617-1683). InputSize/TextureSize = the last FBO's content /
	// pow2 size; OutputSize = the present viewport; MVP = the rotated
	// projection (gl2_set_projection allow_rotate). SET_ROTATION is
	// applied here, not to the chain, exactly like RA: the chain runs in
	// the core's orientation and only the final quad rotates.
	// final_noflip selects the straight-sampler pass (stock noshader.glsl,
	// no v flip) for sources that are already top-down (hw-render FBs); the
	// software path uses stock default.glsl, which flips v -- and that flip
	// is only correct because the pass textures are exactly their content
	// (uv span 1).
	//
	// nextui_mode: clip-space quad + identity MVP (stock system shaders
	// hardcode gl_Position = vec4(VertexCoord, 0, 1)); RA mode: unit quad +
	// rotated ortho projection, rotation living in this pass's MVP.
	ShaderPass * final_pass = final_noflip ? &s_noshader_pass : &s_pass_finalscale;
	final_pass->srcw = last_w;
	final_pass->srch = last_h;
	final_pass->texw = nextui_mode ? last_w : (int)last_tex_w;
	final_pass->texh = nextui_mode ? last_h : (int)last_tex_h;

	float final_mvp[16];
	PLAT_compute_present_mvp(rotation, final_mvp);
	runShaderPass(
		final_pass,
		final_src,
		orig_texture_src,
		NULL,
		GL_NONE,
		dst_x, dst_y, dst_w, dst_h,
		nextui_mode ? NULL : final_mvp,
		nextui_mode ? 0 : 1,
		nextui_mode);

	reloadShaderTextures = 0;
}
