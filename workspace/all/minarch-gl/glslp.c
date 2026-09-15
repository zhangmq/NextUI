// RetroArch .glslp preset -> minarch preset translation (P4).
//
// Self-contained on purpose (stdio/string/stdlib/math only, no engine headers)
// so it can be unit-tested on the host: see workspace/tmp/glslp-test/.
//
// Why translate instead of teaching the engine a second format: the output is
// the exact key/value text the existing (device-proven) preset reader
// Config_readOptionsString() consumes, so .glslp support adds no new engine
// path and cannot disturb the .cfg presets.
//
// Mapping (documented in WORKLOG 2026-09-15; the two provisional choices are
// marked PROVISIONAL):
//   shaders        = N           -> minarch_nrofshaders = N (clamped to 3)
//   shaderN        = <path>      -> minarch_shader{n+1} = path after the last
//                                   "glsl/" (keeps RA's subdirectories), else
//                                   the basename
//   filter_linearN = true|false  -> minarch_shader{n+1}_filter = LINEAR|NEAREST
//                                   (absent -> NEAREST, RA's own default)
//   scale_typeN    = viewport    -> minarch_shader{n+1}_upscale = screen
//   scaleN         = <float>     -> minarch_shader{n+1}_upscale = nearest int
//                                   in 1..8 (RA allows fractional/absolute
//                                   sizes we cannot express -> quantised)
//   #reference "p"               -> p's keys first, this file overrides (depth 4)
//   srctype/scaletype            -> RA has no equivalent; fixed default
//                                   GLSLP_DEFAULT_TYPE (see below)
//
// Unsupported RA keys (float_framebuffer, srgb_framebuffer, mipmap_input,
// wrap_mode, frame_count_mod, feedback_pass, ...) are ignored: this engine has
// no equivalent, and silently dropping them is what the old max-shader-3 chain
// did anyway.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

// User decision (2026-09-15): a .glslp pass reports Source/Texture Type =
// "source" (the core frame size). This is the NextUI convention that all six
// shipped .cfg presets use, and what makes a pass>=2 pattern shader (lcd3x,
// scanlines) render at source resolution instead of at the previous pass's
// upscaled output. "relative" would be pure RA semantics; we intentionally do
// not use it here. RA .glslp has no srctype/scaletype key at all, so this is a
// default, not a translation of a file field.
#define GLSLP_DEFAULT_TYPE "source"

#define GLSLP_MAX_PASSES 3
#define GLSLP_MAX_DEPTH 4
#define GLSLP_PATH_MAX 1024

typedef struct {
	char file[256];      // shader file name as our loader wants it
	int filter_linear;   // 0/1
	int scale_screen;    // 1 = viewport
	double scale;        // RA scaleN
	int have_scale;
} GlslpPass;

static void glslp_trim(char *s) {
	char *p = s;
	while (*p && isspace((unsigned char)*p)) p++;
	if (p != s) memmove(s, p, strlen(p) + 1);
	size_t n = strlen(s);
	while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

// Strip surrounding quotes. Strip ALL leading/trailing quote characters
// instead of one matched pair: presets in the wild ship malformed quoting, and
// a single-pair strip silently keeps the extra character (found by the 619-
// preset sweep, workspace/tmp/glslp-test/sweep.py):
//   crt/crt-sines.glslp   filter_linear1  = "true""    -> would read as false
//   ntsc/blargg.glslp     shader1 = "shaders/blargg/blargg-0.glsl""  -> name
//                         keeps a trailing '"' and the file is not found.
// A .glslp value never legitimately begins or ends with a quote as data.
static void glslp_strip_quotes(char *s) {
	size_t n = strlen(s);
	while (n > 0 && (s[0] == '"' || s[0] == '\'')) {
		memmove(s, s + 1, n); /* includes the NUL */
		n--;
	}
	while (n > 0 && (s[n-1] == '"' || s[n-1] == '\''))
		s[--n] = '\0';
}

// Directory part of a path ("" when there is none).
static void glslp_dirname(const char *path, char *out, size_t out_size) {
	const char *slash = strrchr(path, '/');
	if (!slash) { out[0] = '\0'; return; }
	size_t n = (size_t)(slash - path);
	if (n >= out_size) n = out_size - 1;
	memcpy(out, path, n);
	out[n] = '\0';
}

// Turn an RA shader path into the name our loader resolves under
// SHADERS_FOLDER "/glsl": keep everything after the last "glsl/".
static void glslp_shader_name(const char *path, char *out, size_t out_size) {
	const char *glsl = NULL, *p = path;
	while ((p = strstr(p, "glsl/")) != NULL) { glsl = p + 5; p += 5; }
	if (glsl && *glsl) {
		snprintf(out, out_size, "%.*s", (int)out_size - 1, glsl);
		return;
	}
	const char *slash = strrchr(path, '/');
	snprintf(out, out_size, "%.*s", (int)out_size - 1, slash ? slash + 1 : path);
}

// Parse one .glslp (recursively honouring #reference) into `passes`/`nshaders`
// and report every key it found so the output can be complete.
static int glslp_parse(const char *path, GlslpPass *passes, int *nshaders, int depth);

static int glslp_parse_file(const char *path, GlslpPass *passes, int *nshaders, int depth) {
	FILE *f = fopen(path, "r");
	if (!f) return 0;

	char line[GLSLP_PATH_MAX];
	char dir[GLSLP_PATH_MAX];
	glslp_dirname(path, dir, sizeof(dir));

	while (fgets(line, sizeof(line), f)) {
		glslp_trim(line);
		if (!line[0]) continue;

		// #reference "other.glslp" -- typically the base preset this one tunes.
		if (!strncmp(line, "#reference", 10)) {
			char full[GLSLP_PATH_MAX];
			char *q = line + 10;
			glslp_trim(q);
			glslp_strip_quotes(q);
			if (*q && depth < GLSLP_MAX_DEPTH) {
				if (q[0] == '/' || !dir[0])
					snprintf(full, sizeof(full), "%.*s", GLSLP_PATH_MAX - 1, q);
				else
					snprintf(full, sizeof(full), "%.*s/%.*s",
							GLSLP_PATH_MAX / 2 - 1, dir, GLSLP_PATH_MAX / 2 - 1, q);
				glslp_parse(full, passes, nshaders, depth + 1);
			}
			continue;
		}

		if (line[0] == '#' || line[0] == ';') continue; // comment

		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = '\0';
		char key[128], value[GLSLP_PATH_MAX];
		snprintf(key, sizeof(key), "%.*s", (int)sizeof(key) - 1, line);
		snprintf(value, sizeof(value), "%.*s", (int)sizeof(value) - 1, eq + 1);
		glslp_trim(key);
		glslp_trim(value);
		glslp_strip_quotes(value);
		if (!key[0] || !value[0]) continue;

		if (!strcasecmp(key, "shaders")) {
			int n = atoi(value);
			if (n < 0) n = 0;
			if (n > GLSLP_MAX_PASSES) n = GLSLP_MAX_PASSES;
			*nshaders = n;
			continue;
		}

		// <field><index>, e.g. shader0, filter_linear1, scale_type2, scale_x0
		size_t klen = strlen(key);
		if (klen < 2) continue;
		char last = key[klen - 1];
		if (!isdigit((unsigned char)last)) continue;
		int idx = last - '0';
		if (idx < 0 || idx >= GLSLP_MAX_PASSES) continue;
		key[klen - 1] = '\0';

		GlslpPass *ps = &passes[idx];
		if (!strcasecmp(key, "shader")) {
			glslp_shader_name(value, ps->file, sizeof(ps->file));
		}
		else if (!strcasecmp(key, "filter_linear")) {
			ps->filter_linear = (!strcasecmp(value, "true") || !strcmp(value, "1"));
		}
		else if (!strcasecmp(key, "scale_type") || !strcasecmp(key, "scale_type_x")) {
			if (!strcasecmp(value, "viewport")) ps->scale_screen = 1;
			else ps->scale_screen = 0;   // source / absolute
		}
		else if (!strcasecmp(key, "scale") || !strcasecmp(key, "scale_x")) {
			ps->scale = atof(value);
			ps->have_scale = 1;
		}
		// scale_y / scale_type_y and everything else: ignored (documented).
	}
	fclose(f);
	return 1;
}

static int glslp_parse(const char *path, GlslpPass *passes, int *nshaders, int depth) {
	return glslp_parse_file(path, passes, nshaders, depth);
}

int GLSLP_to_minarch(const char *path, char *out, size_t out_size) {
	GlslpPass passes[GLSLP_MAX_PASSES];
	memset(passes, 0, sizeof(passes));
	int nshaders = 0;

	if (!path || !out || out_size < 64) return 0;
	if (!glslp_parse(path, passes, &nshaders, 0)) return 0;

	// Count passes that actually name a shader (RA presets may declare
	// `shaders` before/after the entries, and may leave gaps).
	int used = 0;
	for (int i = 0; i < GLSLP_MAX_PASSES; i++)
		if (passes[i].file[0]) used = i + 1;
	if (nshaders > used) nshaders = used;
	if (nshaders <= 0) return 0;

	size_t off = 0;
	int n = snprintf(out + off, out_size - off, "minarch_nrofshaders = %d\n", nshaders);
	if (n < 0 || (size_t)n >= out_size - off) return 0;
	off += (size_t)n;

	for (int i = 0; i < nshaders; i++) {
		GlslpPass *ps = &passes[i];
		if (!ps->file[0]) return 0;

		int upscale = 1;
		if (ps->scale_screen) {
			n = snprintf(out + off, out_size - off, "minarch_shader%d = %s\n", i + 1, ps->file);
			if (n < 0 || (size_t)n >= out_size - off) return 0;
			off += (size_t)n;
			n = snprintf(out + off, out_size - off, "minarch_shader%d_filter = %s\n",
					i + 1, ps->filter_linear ? "LINEAR" : "NEAREST");
			if (n < 0 || (size_t)n >= out_size - off) return 0;
			off += (size_t)n;
			n = snprintf(out + off, out_size - off,
					"minarch_shader%d_upscale = screen\n"
					"minarch_shader%d_srctype = %s\n"
					"minarch_shader%d_scaletype = %s\n",
					i + 1, i + 1, GLSLP_DEFAULT_TYPE, i + 1, GLSLP_DEFAULT_TYPE);
			if (n < 0 || (size_t)n >= out_size - off) return 0;
			off += (size_t)n;
			continue;
		}

		// source / absolute: our `upscale` is an integer multiplier of the
		// input, so quantise (documented).
		double sc = ps->have_scale ? ps->scale : 1.0;
		upscale = (int)floor(sc + 0.5);
		if (upscale < 1) upscale = 1;
		if (upscale > 8) upscale = 8;

		n = snprintf(out + off, out_size - off,
				"minarch_shader%d = %s\n"
				"minarch_shader%d_filter = %s\n"
				"minarch_shader%d_upscale = %d\n"
				"minarch_shader%d_srctype = %s\n"
				"minarch_shader%d_scaletype = %s\n",
				i + 1, ps->file,
				i + 1, ps->filter_linear ? "LINEAR" : "NEAREST",
				i + 1, upscale,
				i + 1, GLSLP_DEFAULT_TYPE,
				i + 1, GLSLP_DEFAULT_TYPE);
		if (n < 0 || (size_t)n >= out_size - off) return 0;
		off += (size_t)n;
	}

	return 1;
}
