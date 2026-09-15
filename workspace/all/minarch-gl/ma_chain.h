// ma_chain -- the shader pipeline, shared by the software and hardware paths.
//
// Moved out of the overridden generic_video.c so that override goes back to
// hook-shaped (it was carrying ~850 changed lines in 20 hunks).  Types live
// here; state and execution live in ma_chain.c.  Nothing here knows about
// cores: a caller hands in the texture to run the chain on.
#ifndef MA_CHAIN_H
#define MA_CHAIN_H

#include "defines.h"  // must precede api.h (it needs BTN_ID_COUNT)
#include "api.h"      // ShaderParam, MAXSHADERS, GL types

typedef struct ShaderProgram {
	GLuint shader_p;
	char *filename;

	// Cached from glGetUniformLocation()
	GLint u_FrameDirection;
	GLint u_FrameCount;
	GLint u_OutputSize;
	GLint u_TextureSize;
	GLint u_InputSize;
	GLint u_OrigTextureSize;
	GLint u_OrigInputSize;
	GLint u_Texture;
	GLint u_OrigTexture;
	GLint u_texelSize;
	GLint u_MVP;
	GLint u_FinalViewportSize;
	
	ShaderParam *pragmas;  // Dynamic array of parsed pragma parameters
	int num_pragmas;       // Count of valid pragma parameters
} ShaderProgram;

typedef struct ShaderPass {
	ShaderProgram * program;
	int filter;
	int alpha;
	GLuint target_texture;
	int target_updated;
	int target_w;  // allocated pow2 dims of target_texture (RA fbo_rect w/h)
	int target_h;
	int scale;
	int srctype;
	int scaletype;
	int srcw;
	int srch;
	int texw;
	int texh;
} ShaderPass;

// State (defined in ma_chain.c).  The menu/config/preset code touches shaders[]
// and s_pass_finalscale directly, exactly as it did when they lived in
// generic_video.c.
extern ShaderProgram s_shader_default, s_shader_overlay, s_noshader;
extern ShaderProgram shader_programs[MAXSHADERS];
extern ShaderPass shaders[MAXSHADERS];
extern const ShaderProgram blank_shader_program;
extern const ShaderPass blank_shader_pass;
extern ShaderPass s_pass_finalscale, s_pass_effect, s_pass_overlay, s_pass_notif;
extern ShaderPass s_noshader_pass;
extern int nrofshaders;
extern int reloadShaderTextures;
extern int shaderResetRequested;
extern int s_final_vp_w, s_final_vp_h;
extern int orig_w, orig_h, origtex_w, origtex_h;
extern int frame_count;      // owned by generic_video.c (present path)

// Pass executor and chain entry point (PLAT_run_shader_pipeline is also
// declared in api.h under its GFX_ name).
void runShaderPass(ShaderPass * shader_pass, GLuint src_texture,
				   GLuint orig_texture_src, GLuint * target_texture, int next_filter,
                   int x, int y, int dst_width, int dst_height,
				   const float mvp[16], int unit_quad, int exact_fbo);

#endif
