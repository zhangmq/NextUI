// ma_preset.c -- shader preset plumbing that upstream ma_config.c has no room
// for, moved here so the ma_config.c override stays hook-shaped.
//
// Three pieces:
//   1. the merged preset list (.cfg from minarch + .glslp from RetroArch),
//   2. numeric-grid matching for a stored shader-pragma value,
//   3. the hidden non-UI path that lets a hand-written
//      `minarch_shaders_preset = foo.glslp` actually load.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>

#include "ma_internal.h"
#include "ma_preset.h"

// 1. Upstream listed only ".cfg".  RetroArch presets are .glslp, so scan the
// folder unfiltered and keep both extensions (the .glslp ones go through the
// translator, glslp.c).
char** MA_preset_list(int *count) {
	int filecount = 0;
	if (count) *count = 0;
	char** all = list_files_in_folder(SHADERS_FOLDER, &filecount, NULL, NULL);
	if (!all) return NULL;
	char** kept = malloc(sizeof(char*) * (filecount + 1));
	if (!kept) { free(all); return NULL; }
	int n = 0;
	for (int i = 0; i < filecount; i++) {
		const char* ext = strrchr(all[i], '.');
		if (!ext) continue;
		if (strcasecmp(ext, ".cfg") && strcasecmp(ext, ".glslp")) continue;
		kept[n++] = all[i]; // reuse the strdup'd names
	}
	kept[n] = NULL;
	free(all); // only the array: the names are kept above
	if (count) *count = n;
	return kept;
}

// 2. Shader-pragma cfg values are numeric strings. The menu grid is built from
// the shader's declared min/step, so a stored value that is not exactly on
// the grid (older builds, other frontends, hand edits -- e.g. amp=1.24 on a
// 0.05 grid) would make Option_getValueIndex fall through to slot 0, the
// minimum. For scanlines amp the minimum is 0, which renders the whole
// picture black. Match numerically instead: exact grid-string first, else
// parse the stored value and pick the nearest in-range grid slot.
int MA_shaderpragma_value_index(Option *option, const char *value) {
	if (!option || !value) return 0;
	if (option->values) {
		for (int i = 0; option->values[i]; i++)
			if (!strcmp(option->values[i], value)) return i;

		int last = 0;
		while (option->values[last]) last++;
		last--;
		if (last < 0) return 0;

		float want = strtof(value, NULL);
		float v0 = strtof(option->values[0], NULL);
		float v1 = strtof(option->values[last], NULL);
		if (want < v0) want = v0;
		if (want > v1) want = v1;

		int best = 0;
		float bestd = 1e30f;
		for (int i = 0; i <= last; i++) {
			float vi = strtof(option->values[i], NULL);
			float d = fabsf(want - vi);
			if (d < bestd) { bestd = d; best = i; }
		}
		return best;
	}
	return 0;
}

// 3. P4 non-UI path: the preset NAME stored in a per-core cfg, kept only when
// it is an RA .glslp. A .cfg preset is flattened into the cfg by the menu (its
// minarch_shaderN keys are already there) and is re-applied by initShaders'
// option loop, so it needs no re-read; a .glslp is NOT flat and has to go
// through the translator. readShadersPreset is otherwise reachable only from
// the menu's Config_syncShaders, so without this a hand-written
// `minarch_shaders_preset = foo.glslp` would be listed in the menu yet never
// loaded. The RAW stored text is used (not option->value) because
// Option_getValueIndex falls back to slot 0 for a name missing from the
// scanned list, which would silently load an unrelated preset.
static char glslp_preset_name[MAX_PATH] = {0};

void MA_preset_note_glslp(const char *value, int has_flat_keys) {
	if (!value) return;
	size_t vl = strlen(value);
	if (vl <= 6 || strcasecmp(value + vl - 6, ".glslp")) return;
	// If the same cfg also carries flattened minarch_shaderN keys, the menu
	// already expanded the preset once and the stored (possibly edited) state
	// must win -- that is what keeps a preset name "display only" the way
	// upstream treats .cfg names.
	if (!has_flat_keys)
		snprintf(glslp_preset_name, sizeof(glslp_preset_name), "%s", value);
}

const char* MA_preset_glslp_name(void) {
	return glslp_preset_name;
}
