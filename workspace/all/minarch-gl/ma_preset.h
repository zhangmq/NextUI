#pragma once

#include "ma_internal.h"

// Shader preset plumbing kept out of upstream ma_config.c (see
// workspace/tmp/minarch-gl-rewrite/PROGRESS.txt, P4 + round 10).

// The merged preset list: minarch .cfg presets plus RetroArch .glslp presets
// from SHADERS_FOLDER.  Returns a malloc'd NULL-terminated array (the caller
// owns the array AND the name strings, exactly like list_files_in_folder) or
// NULL when the folder holds nothing usable.  *count, when non-NULL, gets the
// number of kept entries.
char** MA_preset_list(int *count);

// Index of the menu-grid slot for a stored shader-pragma value: the exact
// grid string when it matches, else the nearest in-range slot (see the .c).
int MA_shaderpragma_value_index(Option *option, const char *value);

// Remember a .glslp preset named by a per-core cfg so initShaders() can load
// it.  Call for the minarch_shaders_preset key with the raw stored text.
// has_flat_keys tells whether the same cfg also carries flattened
// minarch_shaderN/minarch_nrofshaders state (upstream's Config_getValue is
// file-static, so the caller probes its own cfg text).
void MA_preset_note_glslp(const char *value, int has_flat_keys);

// The remembered .glslp name ("" when none).  Never NULL.
const char* MA_preset_glslp_name(void);
