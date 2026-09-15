#pragma once

#include "libretro.h"

// Performance interface (RETRO_ENVIRONMENT_GET_PERF_INTERFACE), kept in its
// own module so ma_environment.c stays hook-shaped.
// Returns false when the caller passed no struct (the core gets `false` and
// falls back to its own clock).
bool MA_perf_fill(struct retro_perf_callback *perf);
