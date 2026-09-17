/////////////////////////////////////////////////////////////////////////////////////////

// File: common/generic_video.c
// Generic implementations of video functions, to be used by platforms that don't
// provide their own implementations.
// Used by: tg5050
// Library dependencies: SDL2, OpenGL (e.g. gles2), pthread, NEON
// Tool dependencies: none
// Script dependencies: none

// \note This files does not have an acompanying header, as all functions are declared in api.h
// with minimal fallback implementations
// \sa FALLBACK_IMPLEMENTATION

/////////////////////////////////////////////////////////////////////////////////////////

#include "defines.h"
#include "platform.h"
#include "api.h"
#include "ma_chain.h"   // shader chain moved out (see ma_chain.h)
#include "ma_gl.h"      // hw-render present replay (PLAT_GL_screenCapture)
#include "utils.h"
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>
#include <math.h>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define NEXTUI_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define NEXTUI_TSAN 1
#endif

// RA FinalViewportSize (shader_glsl.c set_params, params.vp_width/height):
// the final output viewport of this frame's present, uploaded to every pass
// of the chain. Set by PLAT_run_shader_pipeline from the present rect.
// Set when the GL window accepts swap-interval vsync (see PLAT_initVideo);
// drives whether the hw-render present path throttles by usleep or by swap.
int ma_gl_vsync_active = 0;

#include "ma_present.h"   // one present for the whole frontend


// shader stuff

static struct VID_Context {
	SDL_Window* window;
	SDL_Surface* screen;
	SDL_GLContext gl_context;

	GFX_Renderer* blit; // yeesh
	int width;
	int height;
	int pitch;
	int sharpness;
	uint32_t clear_color;
} vid;

static int device_width;
static int device_height;
static int device_pitch;
static uint32_t SDL_transparentBlack = 0;

#define OVERLAYS_FOLDER SDCARD_PATH "/Overlays"

static char* overlay_path = NULL;

// Notification overlay state for RA achievements
typedef struct {
    SDL_Surface* surface;
    int x;
    int y;
    int dirty;
    GLuint tex;
    int tex_w, tex_h;
    int clear_frames;  // Frames to clear framebuffer after notification ends
} NotificationOverlay;

static NotificationOverlay notif = {0};

void PLAT_setNotificationSurface(SDL_Surface* surface, int x, int y) {
    notif.surface = surface;
    notif.x = x;
    notif.y = y;
    notif.dirty = 1;
}

void PLAT_clearNotificationSurface(void) {
    notif.surface = NULL;
    notif.dirty = 0;  // Nothing to update since surface is NULL
    notif.clear_frames = 3;  // Clear for 3 frames (triple buffering safety)
}


#define MAX_SHADERLINE_LENGTH 512
int extractPragmaParameters(const char *shaderSource, ShaderParam *params, int maxParams) {
    const char *pragmaPrefix = "#pragma parameter";
    char line[MAX_SHADERLINE_LENGTH];
    int paramCount = 0;

    const char *currentPos = shaderSource;

    while (*currentPos && paramCount < maxParams) {
        int i = 0;

        // Read a line
        while (*currentPos && *currentPos != '\n' && i < MAX_SHADERLINE_LENGTH - 1) {
            line[i++] = *currentPos++;
        }
        line[i] = '\0';
        if (*currentPos == '\n') currentPos++;

        // Check if it's a #pragma parameter line
        if (strncmp(line, pragmaPrefix, strlen(pragmaPrefix)) == 0) {
            const char *start = line + strlen(pragmaPrefix);
            while (*start == ' ') start++;

            ShaderParam *p = &params[paramCount];

            // Try to parse
            if (sscanf(start, "%127s \"%127[^\"]\" %f %f %f %f",
                       p->name, p->label, &p->def, &p->min, &p->max, &p->step) == 6) {
                paramCount++;
            } else {
                fprintf(stderr, "Failed to parse line:\n%s\n", line);
            }
        }
    }

    return paramCount; // number of parameters found
}

GLuint link_program(GLuint vertex_shader, GLuint fragment_shader, const char* cache_key) {
    char cache_path[512];
    snprintf(cache_path, sizeof(cache_path), SDCARD_PATH "/.shadercache/%s.bin", cache_key);

    GLuint program = glCreateProgram();
    GLint success;

    // Try to load cached binary first
    FILE *f = fopen(cache_path, "rb");
    if (f) {
        GLint binaryFormat;
        fread(&binaryFormat, sizeof(GLint), 1, f);
        fseek(f, 0, SEEK_END);
        size_t length = ftell(f) - sizeof(GLint);
        fseek(f, sizeof(GLint), SEEK_SET);
        void *binary = malloc(length);
        fread(binary, 1, length, f);
        fclose(f);

        glProgramBinary(program, binaryFormat, binary, length);
        free(binary);

        glGetProgramiv(program, GL_LINK_STATUS, &success);
        if (success) {
            LOG_info("Loaded shader program from cache: %s\n", cache_key);
            return program;
        } else {
            LOG_info("Cache load failed, falling back to compile.\n");
            glDeleteProgram(program);
            program = glCreateProgram();
        }
    }

    // Compile and link if cache failed
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &success);

    if (!success) {
        GLint logLength;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
        char* log = (char*)malloc(logLength);
        glGetProgramInfoLog(program, logLength, &logLength, log);
        LOG_error("Program link error: %s\n", log);
        free(log);
        return program;
    }

    GLint binaryLength;
    GLenum binaryFormat;
    glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &binaryLength);
    void* binary = malloc(binaryLength);
    glGetProgramBinary(program, binaryLength, NULL, &binaryFormat, binary);

    mkdir(SDCARD_PATH "/.shadercache", 0755);
    f = fopen(cache_path, "wb");
    if (f) {
        fwrite(&binaryFormat, sizeof(GLenum), 1, f);
        fwrite(binary, 1, binaryLength, f);
        fclose(f);
        LOG_info("Saved shader program to cache: %s\n", cache_key);
    }
    free(binary);

    LOG_info("Program linked and cached\n");
    return program;
}

char* load_shader_source(const char* filename) {
	char filepath[256];
	snprintf(filepath, sizeof(filepath), "%s", filename);
    FILE* file = fopen(filepath, "rb");
    if (!file) {
        fprintf(stderr, "Failed to open shader file: %s\n", filepath);
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    rewind(file);

    char* source = (char*)malloc(length + 1);
    if (!source) {
        fprintf(stderr, "Memory allocation failed\n");
        fclose(file);
        return NULL;
    }

    fread(source, 1, length, file);
    source[length] = '\0';
    fclose(file);
    return source;
}

// FNV-1a 32-bit over the shader source. RA has no persistent program-binary
// cache: a pass program is (re)built from the current .glsl source on every
// (re)load, so the compiled program always matches the file on disk. The
// disk cache below must therefore be keyed by content, not by file name --
// a bare name would resurrect a stale binary for a replaced source (e.g.
// the MVP-ified system shaders) and silently run the old program.
static unsigned shader_source_hash(const char *s) {
	unsigned h = 2166136261u;
	if (!s) return h;
	while (*s) {
		h ^= (unsigned char)*s++;
		h *= 16777619u;
	}
	return h;
}

GLuint load_shader_from_file(GLenum type, const char* filepath) {
    char* source = load_shader_source(filepath);
    if (!source) return 0;

    LOG_info("load shader from file %s\n", filepath);

    // Filter out lines starting with "#pragma parameter"
    char* cleaned = malloc(strlen(source) + 1);
    if (!cleaned) {
        fprintf(stderr, "Out of memory\n");
        free(source);
        return 0;
    }
    cleaned[0] = '\0';

    char* line = strtok(source, "\n");
    while (line) {
        if (strncmp(line, "#pragma parameter", 17) != 0) {
            strcat(cleaned, line);
            strcat(cleaned, "\n");
        }
        line = strtok(NULL, "\n");
    }

    const char* define = NULL;
    const char* default_precision = NULL;
    if (type == GL_VERTEX_SHADER) {
        define = "#define VERTEX\n";
    } else if (type == GL_FRAGMENT_SHADER) {
        define = "#define FRAGMENT\n";
        default_precision =
            "#ifdef GL_ES\n"
            // compat fix for fwidth, dFdx, dFdy
            "#ifdef GL_OES_standard_derivatives\n"
            "#extension GL_OES_standard_derivatives : enable\n"
            "#endif\n"
            "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
            "precision highp float;\n"
            "#else\n"
            "precision mediump float;\n"
            "#endif\n"
            "#endif\n"
            "#define PARAMETER_UNIFORM\n";
    } else {
        fprintf(stderr, "Unsupported shader type\n");
        free(source);
        free(cleaned);
        return 0;
    }

    const char* version_start = strstr(cleaned, "#version");
    const char* version_end = version_start ? strchr(version_start, '\n') : NULL;

    const char* replacement_version = "#version 300 es\n";
    const char* fallback_version = "#version 100\n";

    char* combined = NULL;
    size_t define_len = strlen(define);
    size_t precision_len = default_precision ? strlen(default_precision) : 0;
    size_t source_len = strlen(cleaned);
    size_t combined_len = 0;

    int should_replace_with_300es = 0;
    if (version_start && version_end) {
        char version_str[32] = {0};
        size_t len = version_end - version_start;
        if (len < sizeof(version_str)) {
            strncpy(version_str, version_start, len);
            version_str[len] = '\0';

            if (
                strstr(version_str, "#version 110") ||
                strstr(version_str, "#version 120") ||
                strstr(version_str, "#version 130") ||
                strstr(version_str, "#version 140") ||
                strstr(version_str, "#version 150") ||
                strstr(version_str, "#version 330") ||
                strstr(version_str, "#version 400") ||
                strstr(version_str, "#version 410") ||
                strstr(version_str, "#version 420") ||
                strstr(version_str, "#version 430") ||
                strstr(version_str, "#version 440") ||
                strstr(version_str, "#version 450")
            ) {
                should_replace_with_300es = 1;
            }
        }
    }

    if (version_start && version_end && should_replace_with_300es) {
        size_t header_len = version_end - cleaned + 1;
        size_t version_len = strlen(replacement_version);
        combined_len = version_len + define_len + precision_len + (source_len - header_len) + 1;
        combined = (char*)malloc(combined_len);
        if (!combined) {
            fprintf(stderr, "Out of memory\n");
            free(source);
            free(cleaned);
            return 0;
        }

        strcpy(combined, replacement_version);
        strcat(combined, define);
        if (default_precision) strcat(combined, default_precision);
        strcat(combined, cleaned + header_len);
    } else if (version_start && version_end) {
        size_t header_len = version_end - cleaned + 1;
        combined_len = header_len + define_len + precision_len + (source_len - header_len) + 1;
        combined = (char*)malloc(combined_len);
        if (!combined) {
            fprintf(stderr, "Out of memory\n");
            free(source);
            free(cleaned);
            return 0;
        }

        memcpy(combined, cleaned, header_len);
        memcpy(combined + header_len, define, define_len);
        if (default_precision)
            memcpy(combined + header_len + define_len, default_precision, precision_len);
        strcpy(combined + header_len + define_len + precision_len, cleaned + header_len);
    } else {
        size_t version_len = strlen(fallback_version);
        combined_len = version_len + define_len + precision_len + source_len + 1;
        combined = (char*)malloc(combined_len);
        if (!combined) {
            fprintf(stderr, "Out of memory\n");
            free(source);
            free(cleaned);
            return 0;
        }

        strcpy(combined, fallback_version);
        strcat(combined, define);
        if (default_precision) strcat(combined, default_precision);
        strcat(combined, cleaned);
    }

    GLuint shader = glCreateShader(type);
    const char* combined_ptr = combined;
    glShaderSource(shader, 1, &combined_ptr, NULL);
    glCompileShader(shader);

    free(source);
    free(cleaned);
    free(combined);

    GLint compiled;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "Shader compilation failed:\n%s\n", log);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

#define MAX_SHADER_PRAGMAS 32
void loadShaderPragmas(ShaderProgram *shader, const char *shaderSource) {
	shader->pragmas = calloc(MAX_SHADER_PRAGMAS, sizeof(ShaderParam));
	if (!shader->pragmas) {
		fprintf(stderr, "Out of memory allocating pragmas for %s\n", shader->filename);
		return;
	}
	shader->num_pragmas = extractPragmaParameters(shaderSource, shader->pragmas, MAX_SHADER_PRAGMAS);
}

ShaderParam* PLAT_getShaderPragmas(int i) {
    return shaders[i].program->pragmas;
}

void init_shader_program(ShaderProgram * shader, const char * path, const char * filename) {
	char filepath[512];
	snprintf(filepath, sizeof(filepath), "%s/%s", path, filename);

	const char *shaderSource  = load_shader_source(filepath);
	loadShaderPragmas(shader,shaderSource);

	GLuint vertex_shader1 = load_shader_from_file(GL_VERTEX_SHADER, filepath);
	GLuint fragment_shader1 = load_shader_from_file(GL_FRAGMENT_SHADER, filepath);

	// Link the shader program
	if (shader->shader_p != 0) {
		LOG_info("Deleting previous shader %i\n",shader->shader_p);
		glDeleteProgram(shader->shader_p);
	}
	// Cache key carries a content hash so a changed .glsl invalidates its
	// cached binary (see shader_source_hash).
	char cache_key[320];
	snprintf(cache_key, sizeof(cache_key), "%s-%08x", filename,
			shader_source_hash(shaderSource));
	shader->shader_p = link_program(vertex_shader1, fragment_shader1, cache_key);


	if (shader->shader_p == 0) {
		LOG_info("Shader linking failed for %s\n", filename);
	}

	GLint success = 0;
	glGetProgramiv(shader->shader_p, GL_LINK_STATUS, &success);
	if (!success) {
		char infoLog[512];
		glGetProgramInfoLog(shader->shader_p, 512, NULL, infoLog);
		LOG_info("Shader Program Linking Failed: %s\n", infoLog);
	} else {
		LOG_info("Shader Program Linking Success %s shader ID is %i\n", filename,shader->shader_p);

		// Populate uniforms and pragma uniforms
		shader->u_FrameDirection = glGetUniformLocation( shader->shader_p, "FrameDirection");
		shader->u_FrameCount = glGetUniformLocation( shader->shader_p, "FrameCount");
		shader->u_OutputSize = glGetUniformLocation( shader->shader_p, "OutputSize");
		shader->u_TextureSize = glGetUniformLocation( shader->shader_p, "TextureSize");
		shader->u_InputSize = glGetUniformLocation( shader->shader_p, "InputSize");
		shader->u_OrigTextureSize = glGetUniformLocation( shader->shader_p, "OrigTextureSize");
		shader->u_OrigInputSize = glGetUniformLocation( shader->shader_p, "OrigInputSize");
		shader->u_Texture = glGetUniformLocation(shader->shader_p, "Texture");
		shader->u_OrigTexture = glGetUniformLocation(shader->shader_p, "OrigTexture");
		shader->u_texelSize = glGetUniformLocation(shader->shader_p, "texelSize");
		shader->u_MVP = glGetUniformLocation(shader->shader_p, "MVPMatrix");
		shader->u_FinalViewportSize = glGetUniformLocation(shader->shader_p, "FinalViewportSize");
		for (int i = 0; i < shader->num_pragmas; ++i) {
			shader->pragmas[i].uniformLocation = glGetUniformLocation(shader->shader_p, shader->pragmas[i].name);
			shader->pragmas[i].value = shader->pragmas[i].def;

			LOG_info("Param: %s = %f (min: %f, max: %f, step: %f)\n",
					 shader->pragmas[i].name,
					 shader->pragmas[i].def,
					 shader->pragmas[i].min,
					 shader->pragmas[i].max,
					 shader->pragmas[i].step);
		}

	}
	shader->filename = strdup(filename);

}

void PLAT_initShaders() {
	SDL_GL_MakeCurrent(vid.window, vid.gl_context);
	glViewport(0, 0, device_width, device_height);

	// Init user shaders
	for (int i = 0; i < MAXSHADERS; i++) {
		memcpy(&shader_programs[i], &blank_shader_program, sizeof(ShaderProgram));
		memcpy(&shaders[i], &blank_shader_pass, sizeof(ShaderPass));
		shaders[i].program = &shader_programs[i];
	}

	// Init .system shaders
	// Final display shader (simple texture blit)
	init_shader_program(&s_shader_default, SYSSHADERS_FOLDER, "default.glsl");

	// Overlay shader, for png overlays and static line/grid overlays
	init_shader_program(&s_shader_overlay, SYSSHADERS_FOLDER, "overlay.glsl");

	// Stand-In if a shader is supposed to be applied, but wasnt compiled properly (shaper_p == NULL)
	init_shader_program(&s_noshader, SYSSHADERS_FOLDER, "noshader.glsl");
	
	LOG_info("default shaders loaded, %i\n\n", s_shader_default.shader_p);
}

void PLAT_initNotificationTexture(void) {
	// Pre-allocate notification texture to avoid frame skip on first notification
	glGenTextures(1, &notif.tex);
	glBindTexture(GL_TEXTURE_2D, notif.tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	// Allocate full-screen texture with transparent pixels
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, device_width, device_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	notif.tex_w = device_width;
	notif.tex_h = device_height;
}

static void sdl_log_stdout(
    void *userdata,
    int category,
    SDL_LogPriority priority,
    const char *message)
{
    (void)userdata;
    (void)category;
    (void)priority;

    LOG_info("[SDL] %s\n", message);
}

void PLAT_resetShaders() {
	reloadShaderTextures = 1;
	shaderResetRequested = 1;
}

SDL_Surface* PLAT_initVideo(void) {

#if NEXTUI_TSAN
	/*
	 * Mesa's llvmpipe spawns worker threads that race during teardown under TSAN.
	 * Softpipe keeps rendering single-threaded, avoiding the contested mutex/cond
	 * destruction without affecting release builds.
	 */
	setenv("GALLIUM_DRIVER", "softpipe", 0);
	setenv("LP_NUM_THREADS", "1", 1);
#endif
	SDL_LogSetOutputFunction(sdl_log_stdout, NULL);
	//SDL_LogSetAllPriority(SDL_LOG_PRIORITY_VERBOSE);
	SDL_InitSubSystem(SDL_INIT_VIDEO);
	SDL_ShowCursor(0);

//	SDL_version compiled;
//	SDL_version linked;
//	SDL_VERSION(&compiled);
//	SDL_GetVersion(&linked);
//	LOG_info("Compiled SDL version %d.%d.%d ...\n", compiled.major, compiled.minor, compiled.patch);
//	LOG_info("Linked SDL version %d.%d.%d.\n", linked.major, linked.minor, linked.patch);
//		LOG_info("Available video drivers:\n");
//	for (int i=0; i<SDL_GetNumVideoDrivers(); i++) {
//		LOG_info("- %s\n", SDL_GetVideoDriver(i));
//	}
//	LOG_info("Current video driver: %s\n", SDL_GetCurrentVideoDriver());
//	LOG_info("Available render drivers:\n");
//	for (int i=0; i<SDL_GetNumRenderDrivers(); i++) {
//		SDL_RendererInfo info;
//		SDL_GetRenderDriverInfo(i,&info);
//		LOG_info("- %s\n", info.name);
//	}
//	LOG_info("Available video displays: %d\n", SDL_GetNumVideoDisplays());
//	LOG_info("Available display modes:\n");
//	SDL_DisplayMode mode;
//	for (int i=0; i<SDL_GetNumDisplayModes(0); i++) {
//		SDL_GetDisplayMode(0, i, &mode);
//		LOG_info("- %ix%i (%s)\n", mode.w,mode.h, SDL_GetPixelFormatName(mode.format));
//	}
//	SDL_GetCurrentDisplayMode(0, &mode);
//	LOG_info("Current display mode: %ix%i (%s)\n", mode.w,mode.h, SDL_GetPixelFormatName(mode.format));

	int w = FIXED_WIDTH;
	int h = FIXED_HEIGHT;
	int p = FIXED_PITCH;

	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,"1");
	SDL_SetHint(SDL_HINT_RENDER_DRIVER,"opengl");
	SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION,"1");

	vid.window   = SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w,h, SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN);
	if (!vid.window) {
		LOG_error("SDL_CreateWindow failed: %s\n", SDL_GetError());
		exit(1);
	}
	// No SDL_Renderer: every frontend surface (game frame, menu/UI, effect,
	// overlay, notification, HUD) is composited in the ONE GL context below.
	// The SDL renderer used to own a second GLES2 context on this window purely
	// for the menu path (see the GL UI compositor).

	if(strcmp("Desktop", PLAT_getModel()) == 0) {
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	}
	else {
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	}

	// Hardware-render cores (flycast) need a complete default framebuffer:
	// they glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT) every frame.
	// Without depth/stencil the default framebuffer is incomplete on Mali.
	// Same attributes as standalone flycast's SDLGLGraphicsContext::Init
	// (DEPTH 24, STENCIL 8, DOUBLEBUFFER). Must be set before CreateContext.
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

	vid.gl_context = SDL_GL_CreateContext(vid.window);
	if (!vid.gl_context) {
		LOG_error("SDL_GL_CreateContext failed: %s\n", SDL_GetError());
		exit(1);
	}
	SDL_GL_MakeCurrent(vid.window, vid.gl_context);
	MA_present_init(vid.window);
	glViewport(0, 0, w, h);

	// Probe real swap-interval vsync (RA enable_vsync semantics) instead of
	// assuming the driver cannot vsync. The result is only logged here: the
	// hw-render present path still uses the usleep throttle until swap-vsync
	// pacing (incl. fast-forward interplay) is wired up.
	{
		int swap_ok = (SDL_GL_SetSwapInterval(1) == 0);
		if (swap_ok)
			SDL_GL_SetSwapInterval(0);
		ma_gl_vsync_active = 0;
		LOG_info("minarch: GL swap-interval vsync %s\n",
			swap_ok ? "supported" : "unsupported by driver");
	}

	vid.screen = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
	SDL_SetSurfaceBlendMode(vid.screen, SDL_BLENDMODE_BLEND);

	vid.width	= w;
	vid.height	= h;
	vid.pitch	= p;

	SDL_transparentBlack = SDL_MapRGBA(vid.screen->format, 0, 0, 0, 0);
	PLAT_setClearColor(SDL_transparentBlack);

	device_width	= w;
	device_height	= h;
	device_pitch	= p;

	vid.sharpness = SHARPNESS_SOFT;

	return vid.screen;
}

void PLAT_setClearColor(uint32_t color) {
	vid.clear_color = color;
}

void PLAT_updateShader(int i, const char *filename, int *scale, int *filter, int *scaletype, int *srctype) {

    if (i < 0 || i >= MAXSHADERS) {
        return;
    }
    ShaderPass* shader_pass = &shaders[i];

    if (filename != NULL) {
        SDL_GL_MakeCurrent(vid.window, vid.gl_context);
        LOG_info("loading shader \n");

		init_shader_program(shader_pass->program, SHADERS_FOLDER "/glsl", filename);
	}
    if (scale != NULL) {
        shader_pass->scale = *scale +1;
		reloadShaderTextures = 1;
    }
    if (scaletype != NULL) {
        shader_pass->scaletype = *scaletype;
    }
    if (srctype != NULL) {
        shader_pass->srctype = *srctype;
    }
    if (filter != NULL) {
        shader_pass->filter = (*filter == 1) ? GL_LINEAR : GL_NEAREST;
		reloadShaderTextures = 1;
    }
	shader_pass->target_updated = 1;

}


void PLAT_setShaders(int nr) {
	LOG_info("set nr of shaders to %i\n",nr);
	nrofshaders = nr;
	reloadShaderTextures = 1;
}

// ---------------------------------------------------------------------------
// GL UI compositor (in-game menu, menus, transitions).
//
// The menu draws its UI into a CPU surface. Upstream handed that surface to the
// SDL renderer, which owns a SEPARATE GLES2 context on the same window: that is
// a third presentation stack beside the software and hw-render GL paths, it
// forces a context switch around every menu frame, and its background came from
// a post-swap window readback. It is now composited in the SAME GL context as
// the game frame, through the SAME factory overlay pass the Screen Effect /
// Overlay / Notification composite uses (s_pass_ui == overlay.glsl with
// premultiplied blending) -- one compositor for every frontend surface, and the
// menu frame is just another RGBA overlay texture.
//
// Layer order matches the SDL present it replaces exactly:
//   layer1 -> layer2 -> screen(UI) -> layer3 -> layer4 -> layer5
// (slot 0 is the UI screen surface itself; the API's layer 0 maps to slot 1,
// like upstream's `default: target_layer1`).
// ---------------------------------------------------------------------------
#define UI_LAYER_COUNT 6
typedef struct {
	GLuint tex;
	int tex_w, tex_h;   // allocated texture size
	int x, y, w, h;     // destination rect, device pixels
	int valid;
} UILayer;
static UILayer ui_layers[UI_LAYER_COUNT];
static uint32_t *ui_upload_buf = NULL;
static size_t ui_upload_cap = 0;

static int ui_layer_slot(int layer) {
	if (layer < 1 || layer > 5) return 1; // upstream: default -> target_layer1
	return layer;
}

// ARGB8888 (SDL byte order B,G,R,A) -> tightly packed RGBA, scaling RGB
// (brightness, = SDL_SetTextureColorMod) and A (opacity, =
// SDL_SetTextureAlphaMod). Row-aware: the surface pitch is not assumed to be
// w*4.
static const void *ui_prepare_pixels(SDL_Surface *s, float brightness, float alpha_scale) {
	size_t px = (size_t)s->w * (size_t)s->h;
	if (px == 0) return NULL;
	if (px > ui_upload_cap) {
		free(ui_upload_buf);
		ui_upload_buf = (uint32_t *)malloc(px * 4);
		if (!ui_upload_buf) { ui_upload_cap = 0; return NULL; }
		ui_upload_cap = px;
	}
	SDL_Surface *conv = NULL;
	SDL_Surface *src_surface = s;
	if (s->format->format != SDL_PIXELFORMAT_ARGB8888) {
		conv = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0);
		if (!conv) return NULL;
		src_surface = conv;
	}
	uint8_t *dst = (uint8_t *)ui_upload_buf;
	for (int y = 0; y < src_surface->h; y++) {
		const uint8_t *row = (const uint8_t *)src_surface->pixels + (size_t)y * src_surface->pitch;
		for (int x = 0; x < src_surface->w; x++) {
			uint8_t b = row[0], g = row[1], r = row[2], a = row[3];
			if (brightness != 1.0f) {
				r = (uint8_t)(r * brightness);
				g = (uint8_t)(g * brightness);
				b = (uint8_t)(b * brightness);
			}
			if (alpha_scale != 1.0f) a = (uint8_t)(a * alpha_scale);
			dst[0] = r; dst[1] = g; dst[2] = b; dst[3] = a;
			row += 4; dst += 4;
		}
	}
	if (conv) SDL_FreeSurface(conv);
	return ui_upload_buf;
}

static void ui_layer_upload(int slot, SDL_Surface *surface, int x, int y, int w, int h,
		float brightness, float alpha_scale) {
	if (!surface || slot < 0 || slot >= UI_LAYER_COUNT) return;
	const void *pix = ui_prepare_pixels(surface, brightness, alpha_scale);
	if (!pix) return;
	UILayer *L = &ui_layers[slot];
	if (!L->tex) glGenTextures(1, &L->tex);
	glBindTexture(GL_TEXTURE_2D, L->tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	PLAT_gl_unpack_reset();
	if (L->tex_w != surface->w || L->tex_h != surface->h) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, surface->w, surface->h, 0,
				GL_RGBA, GL_UNSIGNED_BYTE, pix);
		L->tex_w = surface->w;
		L->tex_h = surface->h;
	} else {
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, surface->w, surface->h,
				GL_RGBA, GL_UNSIGNED_BYTE, pix);
	}
	L->x = x; L->y = y; L->w = w; L->h = h;
	L->valid = 1;
}

// Draw one layer through the shared overlay pass (its own program/VBO/attrib/
// blend state per draw; see runShaderPass).
static void ui_layer_draw(int slot) {
	UILayer *L = &ui_layers[slot];
	if (!L->valid || !L->tex || L->w <= 0 || L->h <= 0) return;
	runShaderPass(&s_pass_ui, L->tex, L->tex, NULL, GL_NONE,
			L->x, L->y, L->w, L->h, NULL, 0, 1);
}

// One UI present: clear to the configured background colour, then composite
// layer1 -> layer2 -> UI screen -> layer3..5 and swap. This replaces
// PLAT_flip's SDL-renderer path and is the only frontend present that is not a
// game frame.
static void ui_present(SDL_Surface *ui_surface) {
	SDL_GL_MakeCurrent(vid.window, vid.gl_context);
	uint32_t c = vid.clear_color;
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, device_width, device_height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_BLEND);
	glClearColor(((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f,
			(c & 0xFF) / 255.0f, ((c >> 24) & 0xFF) / 255.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	if (ui_surface)
		ui_layer_upload(0, ui_surface, 0, 0, device_width, device_height, 1.0f, 1.0f);

	ui_layer_draw(1);
	ui_layer_draw(2);
	ui_layer_draw(0);
	ui_layer_draw(3);
	ui_layer_draw(4);
	ui_layer_draw(5);

	MA_present_frame();
}

static void clearVideo(void) {
	// Flush a few black frames through the one GL present path.
	if (!vid.screen) return;
	SDL_FillRect(vid.screen, NULL, vid.clear_color);
	for (int i=0; i<3; i++) ui_present(vid.screen);
}

void PLAT_quitVideo(void) {
	clearVideo();

	// Make sure the GL context is current before tearing down GL objects.
	SDL_GL_MakeCurrent(vid.window, vid.gl_context);

	for (int i = 0; i < UI_LAYER_COUNT; i++) {
		if (ui_layers[i].tex) glDeleteTextures(1, &ui_layers[i].tex);
		ui_layers[i].tex = 0;
		ui_layers[i].valid = 0;
	}
	free(ui_upload_buf);
	ui_upload_buf = NULL;
	ui_upload_cap = 0;
	glFinish();

	SDL_GL_MakeCurrent(NULL, NULL);
	SDL_GL_DeleteContext(vid.gl_context);
	vid.gl_context = NULL;
	SDL_FreeSurface(vid.screen);
	vid.screen = NULL;

	// Cleanup and shutdown
	SDL_DestroyWindow(vid.window);
	if (overlay_path) free(overlay_path);

	SDL_QuitSubSystem(SDL_INIT_VIDEO);
	system("cat /dev/zero > /dev/fb0 2>/dev/null");
}

void PLAT_clearVideo(SDL_Surface* screen) {
	// SDL_FillRect(screen, NULL, 0); // TODO: revisit
	SDL_FillRect(screen, NULL, SDL_transparentBlack);
}
void PLAT_clearAll(void) {
	// Drop every UI layer, clear the UI surface and present once. Upstream
	// bounced through the SDL renderer twice to "pull the context back to SDL";
	// there is only one GL context now, so a clear + present is the whole job.
	PLAT_clearLayers(0);
	PLAT_clearVideo(vid.screen);
	PLAT_flip(vid.screen, 0);
}

void PLAT_setVsync(int vsync) {
	// No effect on Ge8300
	//int interval = 1;
	//if (vsync == VSYNC_OFF) interval = 0;
	//else if (vsync == VSYNC_LENIENT) interval = -1; // Adaptive, fallback to 1 usually happens internally if not supported
	//
	//// Try to set swap interval
	//if (SDL_GL_SetSwapInterval(interval) < 0) {
	//	// If -1 (adaptive) failed, try 1 (strict)
	//	if (interval == -1) {
	//		LOG_info("Adaptive VSync not supported, falling back to Strict\n");
	//		SDL_GL_SetSwapInterval(1);
	//	} else {
	//		LOG_error("Failed to set swap interval: %s\n", SDL_GetError());
	//	}
	//} else {
	//	LOG_info("VSync set to %d (requested %d)\n", interval, vsync);
	//}
}

static int hard_scale = 4; // TODO: base src size, eg. 160x144 can be 4


static void resizeVideo(int w, int h, int p) {
	if (w==vid.width && h==vid.height && p==vid.pitch) return;

	// TODO: minarch disables crisp (and nn upscale before linear downscale) when native, is this true?

	if (w>=device_width && h>=device_height) hard_scale = 1;
	// else if (h>=160) hard_scale = 2; // limits gba and up to 2x (seems sufficient for 640x480)
	else hard_scale = 4;

	// LOG_info("resizeVideo(%i,%i,%i) hard_scale: %i crisp: %i\n",w,h,p, hard_scale,vid.sharpness==SHARPNESS_CRISP);

	// Only the frame description changes now: the present paths read their
	// source dimensions from the caller, and there is no SDL texture to resize.
	vid.width	= w;
	vid.height	= h;
	vid.pitch	= p;

	reloadShaderTextures = 1;
}

SDL_Surface* PLAT_resizeVideo(int w, int h, int p) {
	resizeVideo(w,h,p);
	return vid.screen;
}

// GL hw-render present filter (ma_gl.c samples the core FBO texture with
// it). Software-path twin is s_pass_finalscale.filter (finalscale pass
// samples orig_texture); the GL path has no shader pass to override the
// choice (runShaderPass only runs in PLAT_GL_Swap), so the menu value
// always applies there. 1 = GL_LINEAR ("LINEAR"), 0 = GL_NEAREST.
int g_sharpness_linear = 1;

void PLAT_setSharpness(int sharpness) {
	if(sharpness==1) {
		s_pass_finalscale.filter = GL_LINEAR;
	}
	else {
		s_pass_finalscale.filter = GL_NEAREST;
	}
	g_sharpness_linear = (sharpness == 1);
	reloadShaderTextures = 1;
}

static struct FX_Context {
	int scale;
	int type;
	int color;
	int next_scale;
	int next_type;
	int next_color;
	int live_type;
} effect = {
	.scale = 1,
	.next_scale = 1,
	.type = EFFECT_NONE,
	.next_type = EFFECT_NONE,
	.live_type = EFFECT_NONE,
	.color = 0,
	.next_color = 0,
};
static void rgb565_to_rgb888(uint32_t rgb565, uint8_t *r, uint8_t *g, uint8_t *b) {
    // Extract the red component (5 bits)
    uint8_t red = (rgb565 >> 11) & 0x1F;
    // Extract the green component (6 bits)
    uint8_t green = (rgb565 >> 5) & 0x3F;
    // Extract the blue component (5 bits)
    uint8_t blue = rgb565 & 0x1F;

    // Scale the values to 8-bit range
    *r = (red << 3) | (red >> 2);
    *g = (green << 2) | (green >> 4);
    *b = (blue << 3) | (blue >> 2);
}
static char* effect_path;
static int effectUpdated = 0;
static pthread_mutex_t video_prep_mutex = PTHREAD_MUTEX_INITIALIZER;

static void updateEffect(void) {
	// Read effect state with mutex protection
	pthread_mutex_lock(&video_prep_mutex);
	int next_scale = effect.next_scale;
	int next_type = effect.next_type;
	int next_color = effect.next_color;
	int curr_scale = effect.scale;
	int curr_type = effect.type;
	int curr_color = effect.color;
	pthread_mutex_unlock(&video_prep_mutex);

	if (next_scale==curr_scale && next_type==curr_type && next_color==curr_color) return; // unchanged

	// Update effect state with mutex protection
	pthread_mutex_lock(&video_prep_mutex);
	int live_scale = effect.scale;
	int live_color = effect.color;
	effect.scale = effect.next_scale;
	effect.type = effect.next_type;
	effect.color = effect.next_color;
	int effect_type = effect.type;
	int effect_scale = effect.scale;
	int effect_color = effect.color;
	int live_type = effect.live_type;
	pthread_mutex_unlock(&video_prep_mutex);

	if (effect_type==EFFECT_NONE) return; // disabled
	if (effect_type==live_type && effect_scale==live_scale && effect_color==live_color) return; // already loaded

	int opacity = 128; // 1 - 1/2 = 50%
	if (effect_type==EFFECT_LINE) {
		if (effect_scale<3) {
			effect_path = RES_PATH "/line-2.png";
		}
		else if (effect_scale<4) {
			effect_path = RES_PATH "/line-3.png";
		}
		else if (effect_scale<5) {
			effect_path = RES_PATH "/line-4.png";
		}
		else if (effect_scale<6) {
			effect_path = RES_PATH "/line-5.png";
		}
		else if (effect_scale<8) {
			effect_path = RES_PATH "/line-6.png";
		}
		else {
			effect_path = RES_PATH "/line-8.png";
		}
	}
	else if (effect_type==EFFECT_GRID) {
		if (effect_scale<3) {
			effect_path = RES_PATH "/grid-2.png";
			opacity = 64; // 1 - 3/4 = 25%
		}
		else if (effect_scale<4) {
			effect_path = RES_PATH "/grid-3.png";
			opacity = 112; // 1 - 5/9 = ~44%
		}
		else if (effect_scale<5) {
			effect_path = RES_PATH "/grid-4.png";
			opacity = 144; // 1 - 7/16 = ~56%
		}
		else if (effect_scale<6) {
			effect_path = RES_PATH "/grid-5.png";
			opacity = 160; // 1 - 9/25 = ~64%
			// opacity = 96; // TODO: tmp, for white grid
		}
		else if (effect_scale<8) {
			effect_path = RES_PATH "/grid-6.png";
			opacity = 112; // 1 - 5/9 = ~44%
		}
		else if (effect_scale<11) {
			effect_path = RES_PATH "/grid-8.png";
			opacity = 144; // 1 - 7/16 = ~56%
		}
		else {
			effect_path = RES_PATH "/grid-11.png";
			opacity = 136; // 1 - 57/121 = ~52%
		}
	}
	effectUpdated = 1;

}
int screenx = 0;
int screeny = 0;
void PLAT_setOffsetX(int x) {
    if (x < 0 || x > 128) return;
    screenx = x - 64;
	LOG_info("screenx: %i %i\n",screenx,x);
}
void PLAT_setOffsetY(int y) {
    if (y < 0 || y > 128) return;
    screeny = y - 64;
	LOG_info("screeny: %i %i\n",screeny,y);
}
static int overlayUpdated=0;
void PLAT_setOverlay(const char* filename, const char* tag) {
	if (overlay_path) {
		free(overlay_path);
		overlay_path = NULL;
	}

	pthread_mutex_lock(&video_prep_mutex);
	overlayUpdated=1;
	pthread_mutex_unlock(&video_prep_mutex);

    if (!filename || strcmp(filename, "") == 0 || strcmp(filename, "None") == 0) {
		overlay_path = strdup("");
        LOG_info("Skipping overlay update.\n");
        return;
    }

    size_t path_len = strlen(OVERLAYS_FOLDER) + strlen(tag) + strlen(filename) + 4; // +3 for slashes and null-terminator
    overlay_path = malloc(path_len);

    if (!overlay_path) {
        perror("malloc failed");
        return;
    }

    snprintf(overlay_path, path_len, "%s/%s/%s", OVERLAYS_FOLDER, tag, filename);
    LOG_info("Overlay path set to: %s\n", overlay_path);

}


void applyRoundedCorners(SDL_Surface* surface, SDL_Rect* rect, int radius) {
	if (!surface) return;

    Uint32* pixels = (Uint32*)surface->pixels;
    SDL_PixelFormat* fmt = surface->format;
	SDL_Rect target = {0, 0, surface->w, surface->h};
	if (rect)
		target = *rect;

    Uint32 transparent_black = SDL_MapRGBA(fmt, 0, 0, 0, 0);  // Fully transparent black

	const int xBeg = target.x;
	const int xEnd = target.x + target.w;
	const int yBeg = target.y;
	const int yEnd = target.y + target.h;
	for (int y = yBeg; y < yEnd; ++y)
	{
		for (int x = xBeg; x < xEnd; ++x) {
            int dx = (x < xBeg + radius) ? xBeg + radius - x : (x >= xEnd - radius) ? x - (xEnd - radius - 1) : 0;
            int dy = (y < yBeg + radius) ? yBeg + radius - y : (y >= yEnd - radius) ? y - (yEnd - radius - 1) : 0;
            if (dx * dx + dy * dy > radius * radius) {
                pixels[y * target.w + x] = transparent_black;  // Set to fully transparent black
            }
        }
    }
}

void PLAT_clearLayers(int layer) {
	// The UI present clears the framebuffer to vid.clear_color first, so an
	// invalidated layer1 shows exactly what the SDL target clear showed.
	if (layer==0 || layer==1) ui_layers[1].valid = 0;
	if (layer==0 || layer==2) ui_layers[2].valid = 0;
	if (layer==0 || layer==3) ui_layers[3].valid = 0;
	if (layer==0 || layer==4) ui_layers[4].valid = 0;
	if (layer==0 || layer==5) ui_layers[5].valid = 0;
}

void PLAT_drawOnLayer(SDL_Surface *inputSurface, int x, int y, int w, int h, float brightness, bool maintainAspectRatio,int layer) {
	if (!inputSurface) return;

	SDL_Rect dstRect = { x, y, w, h };
	if (maintainAspectRatio) {
		float aspectRatio = (float)inputSurface->w / (float)inputSurface->h;
		if (w / (float)h > aspectRatio) {
			dstRect.w = (int)(h * aspectRatio);
		} else {
			dstRect.h = (int)(w / aspectRatio);
		}
	}
	ui_layer_upload(ui_layer_slot(layer), inputSurface,
			dstRect.x, dstRect.y, dstRect.w, dstRect.h, brightness, 1.0f);
}


int PLAT_textShouldScroll(TTF_Font* font, const char* in_name,int max_width, SDL_mutex* fontMutex) {
    int text_width = 0;
	if (fontMutex) SDL_LockMutex(fontMutex);
	TTF_SizeUTF8(font, in_name, &text_width, NULL);
	if (fontMutex) SDL_UnlockMutex(fontMutex);

	if (text_width <= max_width) {
		return 0;
	} else {
		return 1;
	}
}
static int text_offset = 0;
void PLAT_resetScrollText() {
	text_offset = 0;
}
void PLAT_animateSurfaceOpacity(
	SDL_Surface *inputSurface,
	int x, int y, int w, int h,
	int start_opacity, int target_opacity,
	int duration_ms,
	int layer
) {
	if (!inputSurface) return;

	const int fps = 60;
	const int frame_delay = 1000 / fps;
	int total_frames = duration_ms / frame_delay;
	if (total_frames < 1) total_frames = 1;

	// GL path: re-upload the surface into the layer texture with the current
	// opacity and present. Same layer mapping as upstream
	// ((layer==0) ? layer2 : layer4).
	int slot = (layer == 0) ? 2 : 4;
	for (int frame = 0; frame <= total_frames; ++frame) {

		float t = (float)frame / total_frames;
		int current_opacity = start_opacity + (int)((target_opacity - start_opacity) * t);
		if (current_opacity < 0) current_opacity = 0;
		if (current_opacity > 255) current_opacity = 255;

		ui_layer_upload(slot, inputSurface, x, y, w, h, 1.0f,
				(float)current_opacity / 255.0f);
		vid.blit = 0;
		PLAT_flip(vid.screen,0);

	}
}

void PLAT_setEffect(int next_type) {
	pthread_mutex_lock(&video_prep_mutex);
	effect.next_type = next_type;
	pthread_mutex_unlock(&video_prep_mutex);
}
void PLAT_setEffectColor(int next_color) {
	pthread_mutex_lock(&video_prep_mutex);
	effect.next_color = next_color;
	pthread_mutex_unlock(&video_prep_mutex);
}
void PLAT_vsync(int remaining) {
	if (remaining>0) SDL_Delay(remaining);
}


void PLAT_blitRenderer(GFX_Renderer* renderer) {
	// The software present path (PLAT_GL_Swap) reads vid.blit directly; there is
	// no SDL renderer to clear any more.
	vid.blit = renderer;
	resizeVideo(vid.blit->true_w,vid.blit->true_h,vid.blit->src_p);
}

void PLAT_clearShaders() {
	// this funciton was empty so am abusing it for now for this, later need to make a seperate function for it
	// set blit to 0 maybe should be seperate function later
	vid.blit = NULL;
}

void PLAT_flipHidden() {
	// "Composite without presenting": no caller in the tree, but keep the
	// capability -- draw the same UI composite and skip the swap.
	SDL_GL_MakeCurrent(vid.window, vid.gl_context);
	if (vid.screen)
		ui_layer_upload(0, vid.screen, 0, 0, device_width, device_height, 1.0f, 1.0f);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, device_width, device_height);
	ui_layer_draw(1);
	ui_layer_draw(2);
	ui_layer_draw(0);
	ui_layer_draw(3);
	ui_layer_draw(4);
	ui_layer_draw(5);
}


void PLAT_flip(SDL_Surface* screen, int ignored) {
	// ONE frontend present for every non-game frame (menus, in-game menu,
	// transitions): composite the UI layers + the UI surface in the main GL
	// context and swap. There is no second context, no SDL renderer and no
	// post-swap window readback left in this path -- see the GL UI compositor
	// above. Upstream used the SDL renderer here.
	ui_present(screen ? screen : vid.screen);
	vid.blit = NULL;
}


int frame_count = 0;   // extern in ma_chain.h (the chain uses it)
static GLuint orig_texture = 0;

// GL-side queries: whether a shader chain is configured, and the filter the
// chain's first pass expects on its source texture (the software path
// applies it to orig_texture at upload; the hw-render path applies it to
// its normalized chain-source texture).
int PLAT_shaders_active(void) {
	return nrofshaders > 0;
}
int PLAT_first_shader_filter(void) {
	return (nrofshaders > 0) ? shaders[0].filter : 0;
}

typedef struct {
    SDL_Surface* loaded_effect;
    SDL_Surface* loaded_overlay;
    int effect_ready;
    int overlay_ready;
} FramePreparation;

static FramePreparation frame_prep = {0};

int prepareFrameThread(void *data) {
    while (1) {
		updateEffect();

		// Check effectUpdated flag with mutex
		pthread_mutex_lock(&video_prep_mutex);
		int effect_updated = effectUpdated;
		pthread_mutex_unlock(&video_prep_mutex);

        if (effect_updated) {
			LOG_info("effect updated %s\n",effect_path);
			if(effect_path) {
				SDL_Surface* tmp = IMG_Load(effect_path);
				SDL_Surface* converted = NULL;
				if (tmp) {
					converted = SDL_ConvertSurfaceFormat(tmp, SDL_PIXELFORMAT_RGBA32, 0);
					SDL_FreeSurface(tmp);
				}

				pthread_mutex_lock(&video_prep_mutex);
				frame_prep.loaded_effect = converted;
				effectUpdated = 0;
				frame_prep.effect_ready = 1;
				pthread_mutex_unlock(&video_prep_mutex);
			} else {
				pthread_mutex_lock(&video_prep_mutex);
				frame_prep.loaded_effect = 0;
				effectUpdated = 0;
				frame_prep.effect_ready = 1;
				pthread_mutex_unlock(&video_prep_mutex);
			}
        }

		// Check if effect is disabled
		pthread_mutex_lock(&video_prep_mutex);
		int effect_type = effect.type;
		SDL_Surface* loaded_effect = frame_prep.loaded_effect;
		pthread_mutex_unlock(&video_prep_mutex);

		if(effect_type == EFFECT_NONE && loaded_effect != 0) {
			pthread_mutex_lock(&video_prep_mutex);
			frame_prep.loaded_effect = 0;
			frame_prep.effect_ready = 1;
			pthread_mutex_unlock(&video_prep_mutex);
		}

		// Check overlayUpdated flag with mutex
		pthread_mutex_lock(&video_prep_mutex);
		int overlay_updated = overlayUpdated;
		pthread_mutex_unlock(&video_prep_mutex);

        if (overlay_updated) {

			LOG_info("overlay updated\n");
			if(overlay_path) {
				SDL_Surface* tmp = IMG_Load(overlay_path);
				SDL_Surface* converted = NULL;
				if (tmp) {
					converted = SDL_ConvertSurfaceFormat(tmp, SDL_PIXELFORMAT_RGBA32, 0);
					SDL_FreeSurface(tmp);
				}

				pthread_mutex_lock(&video_prep_mutex);
				frame_prep.loaded_overlay = converted;
				frame_prep.overlay_ready = 1;
				overlayUpdated=0;
				pthread_mutex_unlock(&video_prep_mutex);
			} else {
				pthread_mutex_lock(&video_prep_mutex);
				frame_prep.loaded_overlay = 0;
				frame_prep.overlay_ready = 1;
				overlayUpdated=0;
				pthread_mutex_unlock(&video_prep_mutex);
			}
        }

        SDL_Delay(120);
    }
    return 0;
}

static SDL_Thread *prepare_thread = NULL;

// Effect/overlay GL textures + bookkeeping. Shared state between the two
// present paths (software PLAT_GL_Swap and the hw-render quad path) -- the
// same GL context renders both, only one path runs per core.
static GLuint effect_tex = 0;
static int effect_w = 0, effect_h = 0;
static GLuint overlay_tex = 0;
static int overlay_w = 0, overlay_h = 0;

// Debug HUD layer: one surface + one texture for BOTH present paths, filled by
// the single rasterizer in ma_video.c (PLAT_draw_debug_hud -> drawDebugHud) and
// composited by PLAT_composite_overlays below.  Half device resolution on a
// fullscreen quad, so the text keeps upstream's apparent size.
static uint32_t *dbg_hud_pixels = NULL;
static int dbg_hud_w = 0, dbg_hud_h = 0;
static GLuint dbg_hud_tex = 0;
static int dbg_hud_tex_sized = 0;

// Every RGBA overlay draw goes through s_pass_overlay, and runShaderPass reads
// the pass's source size for the TextureSize/InputSize/TexelSize uniforms. Set
// it from the texture actually being drawn right before each draw, so the pass
// never carries another layer's numbers.
static void overlay_pass_src(int w, int h) {
	s_pass_overlay.srcw = s_pass_overlay.texw = w;
	s_pass_overlay.srch = s_pass_overlay.texh = h;
}

static void update_debug_hud_texture(void) {
	int w = device_width / 2, h = device_height / 2;
	if (w <= 0 || h <= 0) return;
	if (!dbg_hud_pixels || dbg_hud_w != w || dbg_hud_h != h) {
		free(dbg_hud_pixels);
		dbg_hud_pixels = calloc((size_t)w * (size_t)h, 4);
		dbg_hud_w = w;
		dbg_hud_h = h;
		dbg_hud_tex_sized = 0;
	}
	if (!dbg_hud_pixels) return;

	// Transparent background: the HUD's black text boxes and white glyphs are
	// opaque, everything else blends through to the frame either path produced.
	memset(dbg_hud_pixels, 0, (size_t)w * (size_t)h * 4);
	PLAT_draw_debug_hud(dbg_hud_pixels, (unsigned)w, (unsigned)h,
			(size_t)w * 4, RETRO_PIXEL_FORMAT_XRGB8888);

	if (!dbg_hud_tex) {
		glGenTextures(1, &dbg_hud_tex);
		if (!dbg_hud_tex) return;
	}
	glBindTexture(GL_TEXTURE_2D, dbg_hud_tex);
	PLAT_gl_unpack_reset();
	if (!dbg_hud_tex_sized) {
		// NEAREST, matching the effect/overlay/notification layers (and the
		// software path's HUD, which is stamped into the core frame and only
		// ever scaled by the chain's own filter). The HUD surface is exactly
		// half the device (360x240) and the composite draws it as a
		// clip-space quad over the full 720x480 viewport -- exactly 2x. GL
		// samples a 2x-magnified texel at s = x/2 + 0.25, i.e. a 0.25-texel
		// phase off the texel centre, so GL_LINEAR was a real 0.75/0.25
		// blend between neighbouring texels (measured: 0.00 of the near-white
		// dot-matrix pixels had an identical horizontal neighbour, where 2x
		// replication gives ~0.5). That only softened the font and bled the
		// transparent background through the glyph edges; NEAREST restores
		// the exact 2x replication the half-res design is for.
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
				GL_UNSIGNED_BYTE, dbg_hud_pixels);
		dbg_hud_tex_sized = 1;
	} else {
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA,
				GL_UNSIGNED_BYTE, dbg_hud_pixels);
	}
}



// The prep thread loads effect/overlay surfaces for BOTH present paths
// (software PLAT_GL_Swap and the hw-render quad path); start it lazily
// from whichever path first needs it.
static void ensure_prepare_thread(void) {
	if (prepare_thread == NULL) {
		prepare_thread = SDL_CreateThread(prepareFrameThread, "PrepareFrameThread", NULL);
		if (prepare_thread == NULL) {
			LOG_error("Error creating background thread: %s\n", SDL_GetError());
		}
	}
}

// Consume the prep thread's effect/overlay surfaces into GL textures.
// Shared by both present paths (same GL context; only one path runs per
// core, so each ready flag has a single consumer in practice).
static void update_effect_overlay_textures(void) {
	// Check if effect needs updating
	pthread_mutex_lock(&video_prep_mutex);
	int effect_ready = frame_prep.effect_ready;
	SDL_Surface* loaded_effect = frame_prep.loaded_effect;
	pthread_mutex_unlock(&video_prep_mutex);

	if (effect_ready) {
		if(loaded_effect) {
			if(!effect_tex) glGenTextures(1, &effect_tex);
			glBindTexture(GL_TEXTURE_2D, effect_tex);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			PLAT_gl_unpack_reset();
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, loaded_effect->w, loaded_effect->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, loaded_effect->pixels);
			effect_w = loaded_effect->w;
			effect_h = loaded_effect->h;
			s_pass_effect.srcw = s_pass_effect.texw = effect_w;
			s_pass_effect.srch = s_pass_effect.texh = effect_h;
		} else {
			if (effect_tex) {
				glDeleteTextures(1, &effect_tex);
			}
			effect_tex = 0;
		}
		pthread_mutex_lock(&video_prep_mutex);
        frame_prep.effect_ready = 0;
		pthread_mutex_unlock(&video_prep_mutex);
    }

	// Check if overlay needs updating
	pthread_mutex_lock(&video_prep_mutex);
	int overlay_ready = frame_prep.overlay_ready;
	SDL_Surface* loaded_overlay = frame_prep.loaded_overlay;
	pthread_mutex_unlock(&video_prep_mutex);

	if (overlay_ready) {
		if(loaded_overlay) {
			if(!overlay_tex) glGenTextures(1, &overlay_tex);
			glBindTexture(GL_TEXTURE_2D, overlay_tex);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			PLAT_gl_unpack_reset();
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, loaded_overlay->w, loaded_overlay->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, loaded_overlay->pixels);
			overlay_w = loaded_overlay->w;
			overlay_h = loaded_overlay->h;
			LOG_info("minarch: overlay texture %dx%d pitch=%d\n",
					overlay_w, overlay_h, (int)loaded_overlay->pitch);
			s_pass_overlay.srcw = s_pass_overlay.texw = overlay_w;
			s_pass_overlay.srch = s_pass_overlay.texh = overlay_h;

		} else {
			if (overlay_tex) {
				glDeleteTextures(1, &overlay_tex);
			}
			overlay_tex = 0;
		}
		pthread_mutex_lock(&video_prep_mutex);
        frame_prep.overlay_ready = 0;
		pthread_mutex_unlock(&video_prep_mutex);
    }
}

// GL hw-render present accessors: the quad path (ma_gl.c) polls these
// instead of PLAT_GL_Swap (which never runs for hw-render cores).
void PLAT_prepare_overlay_textures(void) {
	ensure_prepare_thread();
	update_effect_overlay_textures();
}
// Effect PNG density selection (line-N.png / grid-N.png) follows the integer
// scale of the presenter's rect (P3 removed the CPU scaler -- both present
// paths derive it from their present rect now).
void PLAT_setEffectScale(int scale) {
	pthread_mutex_lock(&video_prep_mutex);
	effect.next_scale = scale;
	pthread_mutex_unlock(&video_prep_mutex);
}

// GL context accessors for the libretro hardware-render (glsm) path
SDL_Window* PLAT_getGLWindow(void) { return vid.window; }
SDL_GLContext PLAT_getGLContext(void) { return vid.gl_context; }

// The single overlay composite stage -- see the declaration in api.h.  This
// used to exist twice: the software path inlined it here, while the hw-render
// path carried its own program (ma_gl.c ma_gl_overlay_*) and silently never
// drew notifications at all (PLAT_GL_Swap is a software-core path).  Both
// present paths now run this one implementation, in the software path's
// original order: game -> effect -> overlay -> notification.
//
// Everything here is RGBA with alpha, so each draw runs through the factory
// overlay pass (s_pass_overlay, overlay.glsl) with blending on -- never the
// present program, whose fragment shader forces alpha = 1 (RA modern_opaque)
// and whose vertex shader applies the present MVP.
//
// BLEND STATE DISCIPLINE (2026-09-17, N64 edge-band root cause): the overlay
// pass is the ONLY draw in this frontend that ENABLES a GL cap -- runShaderPass
// turns GL_BLEND on for an alpha pass (ma_chain.c: alpha==1 -> glEnable
// (GL_BLEND) + glBlendFunc).  Everything else here only ever disables caps.
// For a libretro GL core that shares this context (mupen64plus-next/GLideN64
// through glsm) that asymmetry is a real leak: glsm tracks only the caps the
// CORE toggled itself (gl_state.cap_state) and its per-frame
// glsm_ctl(GLSM_CTL_STATE_BIND/UNBIND) re-establishes exactly those
// (glsm.c:3117-3156, 3219-3236).  A cap the frontend enabled behind glsm's
// back is never turned back off, so the core's next frame ran with GL_BLEND
// still enabled and with the overlay's blend func -- its opaque writes became
// blended and the outermost rows of the frame (which the core's own draws do
// not fully cover) kept stale colour: the "N64 + debug HUD edge band".
// Measured (device, N64 SM64, HUD on, 5-6 samples per condition): without this
// restore the top/bottom two rows carry 150..436 non-black px that grow with
// the scene; with it, 0/0/0/0 in every sample.  Restoring the blend state the
// composite found costs four state queries per frame and makes the composite
// leave no trace in a context the core's cached state machine owns.
void PLAT_composite_overlays(int effect_x, int effect_y, unsigned int pipeline_src) {
	GLint blend_prev_enabled = 0;
	GLint blend_prev_src = GL_ONE, blend_prev_dst = GL_ZERO;
	GLint blend_prev_eq = GL_FUNC_ADD;
	glGetIntegerv(GL_BLEND, &blend_prev_enabled);
	glGetIntegerv(GL_BLEND_SRC_RGB, &blend_prev_src);
	glGetIntegerv(GL_BLEND_DST_RGB, &blend_prev_dst);
	glGetIntegerv(GL_BLEND_EQUATION_RGB, &blend_prev_eq);

	if (effect_tex) {
		overlay_pass_src(effect_w, effect_h);
		runShaderPass(
			&s_pass_overlay, effect_tex, pipeline_src,
			NULL,
			GL_NONE,
			effect_x, effect_y, effect_w, effect_h,
			NULL, 0, 1);
	}

	if (overlay_tex) {
		overlay_pass_src(overlay_w, overlay_h);
		runShaderPass(
			&s_pass_overlay, overlay_tex, pipeline_src,
			NULL,
			GL_NONE,
			0, 0, device_width, device_height,
			NULL, 0, 1);
	}

	// Notification overlay (RA achievements/indicators; the surface is
	// published by Notification_renderToLayer through
	// PLAT_setNotificationSurface).  Texture pre-allocated in
	// PLAT_initNotificationTexture.
	if (notif.dirty && notif.surface) {
		glBindTexture(GL_TEXTURE_2D, notif.tex);
		PLAT_gl_unpack_reset();
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, notif.surface->w, notif.surface->h, GL_RGBA, GL_UNSIGNED_BYTE, notif.surface->pixels);
		notif.dirty = 0;
	}

	if (notif.tex && notif.surface) {
		overlay_pass_src(notif.tex_w, notif.tex_h);
		runShaderPass(
			&s_pass_overlay, notif.tex, pipeline_src,
			NULL,
			GL_NONE,
			notif.x, notif.y, notif.tex_w, notif.tex_h,
			NULL, 0, 1);
	}

	// Debug HUD, last of the overlays (it is debug info: always on top).
	// This is the one composition point for BOTH present paths: the game image
	// from either path is already finished here (the software path calls this
	// from sw_present_draw, the hw-render path from ma_gl_present_quad), so the
	// HUD looks the same on both -- and because it is part of the draw that
	// PLAT_GL_screenCapture replays, screenshots contain it too.
	if (PLAT_debug_hud_active()) {
		update_debug_hud_texture();
		if (dbg_hud_tex) {
			// The OVERLAY pass is the one draw in the frontend that ENABLES
			// GL_BLEND (runShaderPass: alpha==1 pass -> glEnable(GL_BLEND) +
			// glBlendFunc). See the blend save/restore at the top of this
			// function: the core's own cap bookkeeping does not know about
			// that enable, so it must not be left behind.
			overlay_pass_src(dbg_hud_w, dbg_hud_h);
			runShaderPass(
				&s_pass_overlay, dbg_hud_tex, pipeline_src,
				NULL,
				GL_NONE,
				0, 0, device_width, device_height,
				NULL, 0, 1);
		}
	}

	// Leave the blend state exactly as this composite found it: the core's
	// cap bookkeeping (glsm_state.cap_state) never saw the enable above and
	// will not turn it back off (see the header comment). glBlendFunc also
	// writes the alpha factors to these values; the only remaining consumer
	// of this context is the core, whose next STATE_BIND re-establishes its
	// own blendfunc/blendfunc_separate.
	if (blend_prev_enabled) glEnable(GL_BLEND);
	else                   glDisable(GL_BLEND);
	glBlendFunc((GLenum)blend_prev_src, (GLenum)blend_prev_dst);
	glBlendEquation((GLenum)blend_prev_eq);
}

// ---------------------------------------------------------------------------
// Software present, split into "what to draw" (replayable) and "present it".
//
// PLAT_GL_screenCapture needs the frame that was presented, and the only
// well-defined way to get it is to draw it again -- exactly what RA does by
// reading its viewport BEFORE the swap (gl3_read_viewport). Reading the window
// after SDL_GL_SwapWindow is undefined by spec; it happens to work on this
// driver, which is precisely the kind of reliance this removes.
// ---------------------------------------------------------------------------
static GLuint sw_present_tex = 0;
static int sw_present_w = 0, sw_present_h = 0;
static int sw_present_dx = 0, sw_present_dy = 0, sw_present_dw = 0, sw_present_dh = 0;

static void sw_present_draw(void) {
	if (!sw_present_tex || sw_present_w <= 0 || sw_present_h <= 0) return;
	PLAT_run_shader_pipeline(sw_present_tex, sw_present_tex,
			sw_present_w, sw_present_h, 0,
			sw_present_dx, sw_present_dy, sw_present_dw, sw_present_dh,
			(float)sw_present_w, (float)sw_present_h, 0, 1);

	if (effect_tex || overlay_tex || notif.tex || PLAT_debug_hud_active()) {
		// Game draw is done; composite the overlays through the ONE shared
		// stage (also used by the hw-render present in ma_gl.c). The debug HUD
		// lives in that stage too, so it must be able to run even when no
		// effect/overlay/notification texture exists.
		PLAT_composite_overlays(sw_present_dx, sw_present_dy, sw_present_tex);
	}
}

void PLAT_GL_Swap() {

	//uint64_t performance_frequency = SDL_GetPerformanceFrequency();
	//uint64_t frame_start = SDL_GetPerformanceCounter();

	ensure_prepare_thread();

    static int lastframecount = 0;
    if (reloadShaderTextures) lastframecount = frame_count;
    if (frame_count < lastframecount + 3 || notif.clear_frames > 0) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (notif.clear_frames > 0) notif.clear_frames--;
    }

	// Screen Scaling comes from the shared RA viewport model -- the same
	// function the hw-render path uses (it reads screen_scaling /
	// core.aspect_ratio). P3 removed the CPU scaler chain: the rect may
	// extend past the screen for NATIVE / CROPPED upscales and the final
	// pass's viewport clips the overflow, like RA.
	int dst_x = 0, dst_y = 0, dst_w = 0, dst_h = 0;
	PLAT_compute_present_rect(vid.blit->src_w, vid.blit->src_h,
			&dst_x, &dst_y, &dst_w, &dst_h);
	dst_x += screenx;
	dst_y += screeny;

	// Keep the frame description in sync with the rect this present uses (the
	// capture replay reads the rect back through sw_present_dx/dy/dw/dh) and
	// derive the integer scale that feeds the Screen Effect density -- the
	// deleted CPU scaler used to be what supplied that scale.
	vid.blit->dst_x = dst_x;
	vid.blit->dst_y = dst_y;
	vid.blit->dst_w = dst_w;
	vid.blit->dst_h = dst_h;
	{
		int sw = (int)vid.blit->src_w, sh = (int)vid.blit->src_h;
		int fx = (sw > 0) ? dst_w / sw : 1;
		int fy = (sh > 0) ? dst_h / sh : 1;
		int fx_scale = (fx <= fy) ? fx : fy;
		if (fx_scale < 1) fx_scale = 1;
		vid.blit->scale = fx_scale;
		GFX_setEffectScale(fx_scale);
	}
	SDL_Rect dst_rect = { dst_x, dst_y, dst_w, dst_h };

    if (!vid.blit->src) {
        return;
    }

	SDL_GL_MakeCurrent(vid.window, vid.gl_context);

	static int overlayload = 0;

	static GLuint src_texture = 0;
	static int src_w_last = 0, src_h_last = 0;
	static int last_w = 0, last_h = 0;

	if (shaderResetRequested) {
		if (orig_texture) { glDeleteTextures(1, &orig_texture); orig_texture = 0; }
		src_w_last = src_h_last = 0;
		last_w = last_h = 0;
		if (effect_tex) {
			glDeleteTextures(1, &effect_tex);
			effect_tex = 0;
			effect_w = effect_h = 0;
			// Force reload by marking as ready again if effect is active
			pthread_mutex_lock(&video_prep_mutex);
			if (effect.type != EFFECT_NONE) {
				frame_prep.effect_ready = 1;
			}
			pthread_mutex_unlock(&video_prep_mutex);
		}
		if (overlay_tex) {
			glDeleteTextures(1, &overlay_tex);
			overlay_tex = 0;
			overlay_w = overlay_h = 0;
			// Force reload if we had an overlay
			pthread_mutex_lock(&video_prep_mutex);
			if (frame_prep.loaded_overlay) {
				frame_prep.overlay_ready = 1;
			}
			pthread_mutex_unlock(&video_prep_mutex);
		}
		reloadShaderTextures = 1;
	}

	// Effect/overlay textures are consumed by the shared updater (also
	// polled by the hw-render present path).
	update_effect_overlay_textures();

	if (!orig_texture || reloadShaderTextures) {
        // if (orig_texture) {
        //     glDeleteTextures(1, &orig_texture);
        //     orig_texture = 0;
        // }
		if (orig_texture==0)
			glGenTextures(1, &orig_texture);
        glBindTexture(GL_TEXTURE_2D, orig_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, nrofshaders > 0 ? shaders[0].filter : s_pass_finalscale.filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nrofshaders > 0 ? shaders[0].filter : s_pass_finalscale.filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    glBindTexture(GL_TEXTURE_2D, orig_texture);
    if (vid.blit->src_w != src_w_last || vid.blit->src_h != src_h_last || reloadShaderTextures) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, vid.blit->src_w, vid.blit->src_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, vid.blit->src);
        src_w_last = vid.blit->src_w;
        src_h_last = vid.blit->src_h;
        orig_w = vid.blit->src_w;
        orig_h = vid.blit->src_h;
        origtex_w = vid.blit->src_w;
        origtex_h = vid.blit->src_h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, vid.blit->src_w, vid.blit->src_h, GL_RGBA, GL_UNSIGNED_BYTE, vid.blit->src);
    }

    last_w = vid.blit->src_w;
    last_h = vid.blit->src_h;

	// Retain this present's description so PLAT_GL_screenCapture can replay the
	// exact same draw into an offscreen target (RA reads its viewport before
	// the swap; a post-swap window read is undefined).
	sw_present_tex = orig_texture;
	sw_present_w = last_w;  sw_present_h = last_h;
	sw_present_dx = dst_rect.x; sw_present_dy = dst_rect.y;
	sw_present_dw = dst_rect.w; sw_present_dh = dst_rect.h;

	// Software cores: NextUI conventions -- exact-size pass textures, the
	// Source/Texture Type numbers, clip-space quads + identity MVP and the
	// stock system shaders (see AGENTS.md「呈现/shader 链架构」).
	//
	// Present over a CLEAN frame.  The game draw covers only the present rect
	// (Screen Scaling plus the Screen X/Y offsets), so whatever this back
	// buffer held outside that rect would stay on screen: with SDL's two back
	// buffers holding different leftovers, the uncovered band flickers between
	// them and can show the last menu frame.  The hw path has always cleared
	// its target every frame (ma_gl.c, ma_gl_present_quad); do the same here,
	// to the same opaque black, so the two paths look identical.  The 3-frame
	// clear above stays: it also clears depth after a shader reload.
	glBindFramebuffer(GL_FRAMEBUFFER, PLAT_chain_present_target());
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);
	sw_present_draw();
	// The debug HUD is composited by PLAT_composite_overlays inside
	// sw_present_draw (one layer of the shared overlay stage), so there is
	// nothing to do for it here.

	MA_present_frame();

    frame_count++;
	reloadShaderTextures = 0;
	shaderResetRequested = 0;

	//{
	//	uint64_t op_ts = SDL_GetPerformanceCounter();
	//	uint64_t frame_duration = op_ts - frame_start;
	//	frame_start = op_ts;
	//	double elapsed_time_s = (double)frame_duration / performance_frequency;
	//	double frame_ms = elapsed_time_s * 1000.0;
	//	LOG_info("10: %.2f ms\n", frame_ms);
	//}
}

// flipping image upside down
void PLAT_pixelFlipper(uint8_t* pixels, int width, int height) {
    const int rowBytes = width * 4;
    uint8_t* rowTop;
    uint8_t* rowBottom;

    for (int y = 0; y < height / 2; ++y) {
        rowTop = pixels + y * rowBytes;
        rowBottom = pixels + (height - 1 - y) * rowBytes;

        int x = 0;
// NEON optimization for compatible ARM architectures
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        for (; x + 15 < rowBytes; x += 16) {
            uint8x16_t top = vld1q_u8(rowTop + x);
            uint8x16_t bottom = vld1q_u8(rowBottom + x);

            vst1q_u8(rowTop + x, bottom);
            vst1q_u8(rowBottom + x, top);
        }
#endif
        for (; x < rowBytes; ++x) {
            uint8_t temp = rowTop[x];
            rowTop[x] = rowBottom[x];
            rowBottom[x] = temp;
        }
    }
}

unsigned char* PLAT_GL_screenCapture(int* outWidth, int* outHeight) {
	// Capture the frame that was PRESENTED, deterministically: replay the last
	// present draw into an offscreen device-sized texture and read that back.
	//
	// RA reads its viewport before the swap for exactly this reason
	// (gl3_read_viewport). The previous implementation read the window
	// backbuffer AFTER SDL_GL_SwapWindow, whose contents are undefined by
	// spec -- it happens to work on libmali, which is a reliance, not a
	// contract. The replay goes through the same draw the present used
	// (shader chain + effect/overlay/notification composite), so the capture
	// is byte-identical to what was on screen.
	static GLuint cap_tex = 0, cap_fbo = 0;
	int width = device_width, height = device_height;

	if (outWidth) *outWidth = width;
	if (outHeight) *outHeight = height;
	if (width <= 0 || height <= 0) return NULL;

	unsigned char* pixels = malloc((size_t)width * height * 4); // RGBA
	if (!pixels) return NULL;

	SDL_GL_MakeCurrent(vid.window, vid.gl_context);

	if (!cap_tex) {
		glGenTextures(1, &cap_tex);
		glBindTexture(GL_TEXTURE_2D, cap_tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
				GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glGenFramebuffers(1, &cap_fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, cap_fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				GL_TEXTURE_2D, cap_tex, 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			LOG_error("minarch: capture FBO incomplete\n");
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glDeleteFramebuffers(1, &cap_fbo); cap_fbo = 0;
			glDeleteTextures(1, &cap_tex); cap_tex = 0;
			free(pixels);
			return NULL;
		}
	}

	glBindFramebuffer(GL_FRAMEBUFFER, cap_fbo);
	glViewport(0, 0, width, height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_BLEND);
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);

	PLAT_chain_set_present_target(cap_fbo);
	if (MA_GL_is_active()) MA_GL_present_draw();
	else                  sw_present_draw();
	PLAT_chain_set_present_target(0);

	glBindFramebuffer(GL_FRAMEBUFFER, cap_fbo);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	PLAT_pixelFlipper(pixels, width, height);

	return pixels; // caller must free
}
