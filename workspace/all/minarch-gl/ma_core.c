#include <dlfcn.h>
#include <libgen.h>
#include <string.h>

#include "ma_internal.h"
#include "ma_saves.h"
#include "ma_video.h"
#include "ma_audio.h"
#include "ma_input.h"
#include "ma_cheats.h"
#include "ma_core.h"
#include "ma_gl.h"


void Core_getName(char* in_name, char* out_name) {
	strcpy(out_name, basename(in_name));
	char* tmp = strrchr(out_name, '_');
	tmp[0] = '\0';
}

// The core's retro_set_input_poll callback. RA's equivalent (poll_cb =
// input_driver_poll, runloop.c:8731) is a bare, idempotent driver read that the
// frontend may invoke from several places per frame; minarch's
// input_poll_callback is NOT idempotent -- it runs PAD_poll *and* the whole
// shortcut/menu stage (ma_input.c:17-219). So the frame must contain exactly
// one poll:
//   - override 0 (every other core): the core polls, as it always has.
//   - override EARLY/LATE (flycast and mupen64plus-next announce it whenever
//     their threaded renderer is on): the main loop polls before/after
//     retro_run (RA core_run:9116-9117 / 9157-9159) and this callback is a
//     no-op, even though flycast calls poll_cb() anyway
//     (shell/libretro/libretro.cpp:1078).

// ---------------------------------------------------------------------------
// MA_DPAD_POLICY (minarch-gl): d-pad <-> analog stick policy.
//
// EXPERIMENTAL -- test-only, and it may be REMOVED again.  It exists so the
// stick-less H700 SKUs can reach a core's analog axes at all, is switched by an
// environment variable plus an unbound Shortcuts hotkey, is not persisted and
// has no UI of its own.  If the same policy lands in the platform layer (where
// it would cover every app, including the launcher) this frontend-side copy
// should go away rather than be maintained twice.
//
// The H700 SKUs with no analog stick (rgsp, rg34xx, rg35xxsp, rg28xx) cannot
// reach a core's analog axes at all: input_state_callback() returns
// pad.laxis/raxis, which the platform only writes for SKUs that have sticks.
// This turns the physical d-pad into an analog source, switchable at runtime
// with the "Toggle D-Pad Mode" Shortcuts hotkey and defaulted by the
// environment:
//
//   NEXTUI_ANALOG_DPAD = off (default) | add-left | add-right | add-both
//                      | route-left | route-right
//
//   add-*   the d-pad keeps its own buttons AND drives the stick.  Arcade
//           driving games need this shape (Crazy Taxi shifts with d-pad
//           UP/DOWN while the analog channel steers).
//   route-* the d-pad drives the stick and its own JOYPAD bits are suppressed.
//           That is the community N64 pak's "Input Mode: Joystick" and the
//           right shape for games whose primary control is the d-pad.
//
// The hotkey toggles between the configured mode and off, so the configured
// value is never lost.  Values are full deflection (+/-32767) with the same
// sign convention the platform's SDL path uses for a real stick (up/left
// negative), so cores' own deadzones and scaling apply unchanged.
//
// Scope: minarch-gl only, and per session -- nothing is persisted (the env var
// is the only "config").  The same policy belongs in the platform layer if it
// should cover every app; this is deliberately structured so that only
// dpad_policy_analog()/dpad_policy_filter() would have to move.
// ---------------------------------------------------------------------------
enum {
	DPAD_MODE_OFF = 0,
	DPAD_MODE_ADD_LEFT,
	DPAD_MODE_ADD_RIGHT,
	DPAD_MODE_ADD_BOTH,
	DPAD_MODE_ROUTE_LEFT,
	DPAD_MODE_ROUTE_RIGHT,
};

static int dpad_mode = -1;      // -1 = not initialised yet
static int dpad_mode_saved = 0; // mode the hotkey toggles back to

static int dpad_policy_parse(const char *v) {
	if (!v || !v[0] || !strcasecmp(v, "off"))     return DPAD_MODE_OFF;
	if (!strcasecmp(v, "add-left"))               return DPAD_MODE_ADD_LEFT;
	if (!strcasecmp(v, "add-right"))              return DPAD_MODE_ADD_RIGHT;
	if (!strcasecmp(v, "add-both"))               return DPAD_MODE_ADD_BOTH;
	if (!strcasecmp(v, "route-left"))             return DPAD_MODE_ROUTE_LEFT;
	if (!strcasecmp(v, "route-right"))            return DPAD_MODE_ROUTE_RIGHT;
	LOG_error("NEXTUI_ANALOG_DPAD: unknown value '%s' (off|add-left|add-right|add-both|route-left|route-right)\n", v);
	return DPAD_MODE_OFF;
}

static int dpad_policy_mode(void) {
	if (dpad_mode < 0) {
		dpad_mode = dpad_policy_parse(getenv("NEXTUI_ANALOG_DPAD"));
		if (dpad_mode != DPAD_MODE_OFF) {
			LOG_info("d-pad policy: %s (NEXTUI_ANALOG_DPAD)\n", getenv("NEXTUI_ANALOG_DPAD"));
		}
	}
	return dpad_mode;
}

static int dpad_policy_additive(int mode) {
	return mode == DPAD_MODE_ADD_LEFT || mode == DPAD_MODE_ADD_RIGHT || mode == DPAD_MODE_ADD_BOTH;
}

static int dpad_policy_left(int mode) {
	return mode == DPAD_MODE_ADD_LEFT || mode == DPAD_MODE_ADD_BOTH || mode == DPAD_MODE_ROUTE_LEFT;
}

static int dpad_policy_right(int mode) {
	return mode == DPAD_MODE_ADD_RIGHT || mode == DPAD_MODE_ADD_BOTH || mode == DPAD_MODE_ROUTE_RIGHT;
}

static void dpad_policy_toggle(void) {
	int mode = dpad_policy_mode();
	if (mode == DPAD_MODE_OFF) {
		// first use: the routing shape, i.e. "d-pad as the stick"
		dpad_mode = dpad_mode_saved ? dpad_mode_saved : DPAD_MODE_ROUTE_LEFT;
	} else {
		dpad_mode_saved = mode;
		dpad_mode = DPAD_MODE_OFF;
	}
	LOG_info("d-pad policy: %s\n", dpad_mode == DPAD_MODE_OFF ? "off" :
		dpad_mode == DPAD_MODE_ADD_LEFT ? "add-left" :
		dpad_mode == DPAD_MODE_ADD_RIGHT ? "add-right" :
		dpad_mode == DPAD_MODE_ADD_BOTH ? "add-both" :
		dpad_mode == DPAD_MODE_ROUTE_LEFT ? "route-left" : "route-right");
}

// Runs right after the upstream poll callback (which owns the Shortcuts table
// and the menu-open-on-MENU-release rule), so a MENU+button binding can still
// cancel the menu it would otherwise open.
static void dpad_policy_hotkey(void) {
	ButtonMapping *m = &config.shortcuts[SHORTCUT_TOGGLE_DPAD];
	if (!m->name || m->local < 0) return;                 // unbound
	if (m->mod && !PAD_isPressed(BTN_MENU)) return;
	if (!PAD_justPressed(1 << m->local)) return;
	dpad_policy_toggle();
	if (m->mod) show_menu = 0;
}

// The analog value the d-pad contributes (0 when this mode does not drive it).
static int16_t dpad_policy_analog(int mode, unsigned index, unsigned id) {
	int left = dpad_policy_left(mode), right = dpad_policy_right(mode);
	if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT && !left) return 0;
	if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT && !right) return 0;
	if (index != RETRO_DEVICE_INDEX_ANALOG_LEFT && index != RETRO_DEVICE_INDEX_ANALOG_RIGHT) return 0;

	if (id == RETRO_DEVICE_ID_ANALOG_X) {
		if (PAD_isPressed(BTN_DPAD_LEFT)) return -32767;
		if (PAD_isPressed(BTN_DPAD_RIGHT)) return 32767;
	} else if (id == RETRO_DEVICE_ID_ANALOG_Y) {
		if (PAD_isPressed(BTN_DPAD_UP)) return -32767;
		if (PAD_isPressed(BTN_DPAD_DOWN)) return 32767;
	}
	return 0;
}

#define DPAD_JOYPAD_DIRS ((1 << RETRO_DEVICE_ID_JOYPAD_UP) | (1 << RETRO_DEVICE_ID_JOYPAD_DOWN) | \
                          (1 << RETRO_DEVICE_ID_JOYPAD_LEFT) | (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT))

static int dpad_policy_debug = -1;

void core_input_poll_callback(void) {
	// RA core_input_state_poll_maybe (runloop.c:5069-5078): the callback the
	// core calls polls only under NORMAL.  DONTCARE(0) is RA's default for a
	// core (POLL_TYPE_NORMAL), and EARLY(1)/LATE(3) are polled by the
	// frontend instead (the main loop / core_input_state_callback below).
	if (input_poll_type_override == 0 || input_poll_type_override == 2)
		input_poll_callback();
	dpad_policy_hotkey();
}

// LATE: RA polls on the core's first retro_input_state read of the frame
// (core_input_state_poll_late, runloop.c:5060-5065), i.e. from inside the
// core's own frame.  Wrapping the libretro callback here keeps ma_input.c --
// an upstream file the overlay does not override -- untouched.
static int16_t core_input_state_callback(unsigned port, unsigned device,
		unsigned index, unsigned id) {
	if (input_poll_type_override == 3 && !input_state_polled_this_frame) {
		input_state_polled_this_frame = 1;
		input_poll_callback();
		dpad_policy_hotkey();
	}

	int mode = dpad_policy_mode();
	if (mode == DPAD_MODE_OFF || port != 0 || index != 0)
		return input_state_callback(port, device, index, id);

	if (dpad_policy_debug < 0)
		dpad_policy_debug = getenv("NEXTUI_ANALOG_DPAD_DEBUG") != NULL;

	if (device == RETRO_DEVICE_JOYPAD) {
		int16_t v = input_state_callback(port, device, index, id);
		if (dpad_policy_additive(mode) == 0) {
			if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
				if (dpad_policy_debug && (v & DPAD_JOYPAD_DIRS))
					LOG_info("DPADPOLICY joypad d-pad bits %#x suppressed (MASK)\n", v & DPAD_JOYPAD_DIRS);
				v = (int16_t)(v & ~DPAD_JOYPAD_DIRS);
			}
			else if (id <= RETRO_DEVICE_ID_JOYPAD_R3 && (DPAD_JOYPAD_DIRS & (1 << id))) {
				if (dpad_policy_debug && v) LOG_info("DPADPOLICY joypad dir id=%u suppressed\n", id);
				v = 0;
			}
		}
		return v;
	}

	if (device == RETRO_DEVICE_ANALOG) {
		int16_t v = input_state_callback(port, device, index, id);
		if (v == 0) {
			v = dpad_policy_analog(mode, index, id);
			if (v && dpad_policy_debug)
				LOG_info("DPADPOLICY axis idx=%u id=%u -> %i\n", index, id, v);
		}
		return v;
	}

	return input_state_callback(port, device, index, id);
}

void Core_open(const char* core_path, const char* tag_name) {
	LOG_info("Core_open\n");
	core.handle = dlopen(core_path, RTLD_LAZY);
	
	if (!core.handle) LOG_error("%s\n", dlerror());
	
	core.init = dlsym(core.handle, "retro_init");
	core.deinit = dlsym(core.handle, "retro_deinit");
	core.get_system_info = dlsym(core.handle, "retro_get_system_info");
	core.get_system_av_info = dlsym(core.handle, "retro_get_system_av_info");
	core.set_controller_port_device = dlsym(core.handle, "retro_set_controller_port_device");
	core.reset = dlsym(core.handle, "retro_reset");
	core.run = dlsym(core.handle, "retro_run");
	core.serialize_size = dlsym(core.handle, "retro_serialize_size");
	core.serialize = dlsym(core.handle, "retro_serialize");
	core.unserialize = dlsym(core.handle, "retro_unserialize");
	core.cheat_reset = dlsym(core.handle, "retro_cheat_reset");
	core.cheat_set = dlsym(core.handle, "retro_cheat_set");
	core.load_game = dlsym(core.handle, "retro_load_game");
	core.load_game_special = dlsym(core.handle, "retro_load_game_special");
	core.unload_game = dlsym(core.handle, "retro_unload_game");
	core.get_region = dlsym(core.handle, "retro_get_region");
	core.get_memory_data = dlsym(core.handle, "retro_get_memory_data");
	core.get_memory_size = dlsym(core.handle, "retro_get_memory_size");
	
	void (*set_environment_callback)(retro_environment_t);
	void (*set_video_refresh_callback)(retro_video_refresh_t);
	void (*set_audio_sample_callback)(retro_audio_sample_t);
	void (*set_audio_sample_batch_callback)(retro_audio_sample_batch_t);
	void (*set_input_poll_callback)(retro_input_poll_t);
	void (*set_input_state_callback)(retro_input_state_t);
	
	set_environment_callback = dlsym(core.handle, "retro_set_environment");
	set_video_refresh_callback = dlsym(core.handle, "retro_set_video_refresh");
	set_audio_sample_callback = dlsym(core.handle, "retro_set_audio_sample");
	set_audio_sample_batch_callback = dlsym(core.handle, "retro_set_audio_sample_batch");
	set_input_poll_callback = dlsym(core.handle, "retro_set_input_poll");
	set_input_state_callback = dlsym(core.handle, "retro_set_input_state");
	
	struct retro_system_info info = {};
	core.get_system_info(&info);
	

	LOG_info("Block Extract: %d\n", info.block_extract);

	Core_getName((char*)core_path, (char*)core.name);
	sprintf((char*)core.version, "%s (%s)", info.library_name, info.library_version);
	strcpy((char*)core.tag, tag_name);
	strcpy((char*)core.extensions, info.valid_extensions);
	
	core.need_fullpath = info.need_fullpath;
	
	LOG_info("core: %s version: %s tag: %s (valid_extensions: %s need_fullpath: %i)\n", core.name, core.version, core.tag, info.valid_extensions, info.need_fullpath);
	
	sprintf((char*)core.config_dir, USERDATA_PATH "/%s-%s", core.tag, core.name);
	sprintf((char*)core.states_dir, SHARED_USERDATA_PATH "/%s-%s", core.tag, core.name);
	sprintf((char*)core.saves_dir, SDCARD_PATH "/Saves/%s", core.tag);
	sprintf((char*)core.bios_dir, SDCARD_PATH "/Bios/%s", core.tag);
	sprintf((char*)core.cheats_dir, SDCARD_PATH "/Cheats/%s", core.tag);
	sprintf((char*)core.overlays_dir, SDCARD_PATH "/Overlays/%s", core.tag);
	
	char cmd[512];
	sprintf(cmd, "mkdir -p \"%s\"; mkdir -p \"%s\"", core.config_dir, core.states_dir);
	system(cmd);

	set_environment_callback(environment_callback);
	set_video_refresh_callback(video_refresh_callback);
	set_audio_sample_callback(audio_sample_callback);
	set_audio_sample_batch_callback(audio_sample_batch_callback);
	set_input_poll_callback(core_input_poll_callback);
	set_input_state_callback(core_input_state_callback);
}
void Core_init(void) {
	LOG_info("Core_init\n");
	core.init();
	core.initialized = 1;
}

void Core_applyCheats(struct Cheats *cheats)
{
	if (!cheats)
		return;

	if (!core.cheat_reset || !core.cheat_set)
		return;

	core.cheat_reset();
	for (int i = 0; i < cheats->count; i++) {
		if (cheats->cheats[i].enabled) {
			core.cheat_set(i, cheats->cheats[i].enabled, cheats->cheats[i].code);
		}
	}
}

int Core_updateAVInfo(void) {
	struct retro_system_av_info av_info = {};
	core.get_system_av_info(&av_info);

	double a = av_info.geometry.aspect_ratio;
	if (a<=0) a = (double)av_info.geometry.base_width / av_info.geometry.base_height;

	int changed = (core.fps != av_info.timing.fps || core.sample_rate != av_info.timing.sample_rate || core.aspect_ratio != a
		|| core.max_width != av_info.geometry.max_width || core.max_height != av_info.geometry.max_height
		|| core.base_width != av_info.geometry.base_width || core.base_height != av_info.geometry.base_height);

	core.fps = av_info.timing.fps;
	core.sample_rate = av_info.timing.sample_rate;
	core.aspect_ratio = a;
	core.max_width = av_info.geometry.max_width;
	core.max_height = av_info.geometry.max_height;
	core.base_width = av_info.geometry.base_width;
	core.base_height = av_info.geometry.base_height;

	if (changed) LOG_info("aspect_ratio: %f (%ix%i) fps: %f, max %ux%u\n", a, av_info.geometry.base_width,av_info.geometry.base_height, core.fps,
		av_info.geometry.max_width, av_info.geometry.max_height);

	// RA semantics: hw-render FBO size follows the reported max geometry;
	// rebuild when it grew.
	MA_GL_update_fbo_size();

	return changed;
}

void Core_load(void) {
	LOG_info("Core_load\n");
	struct retro_game_info game_info;
	game_info.path = game.tmp_path[0]?game.tmp_path:game.path;
	game_info.data = game.data;
	game_info.size = game.size;
	LOG_info("game path: %s (%i)\n", game_info.path, game.size);
	core.load_game(&game_info);

	if (Cheats_load())
		Core_applyCheats(&cheatcodes);

	SRAM_read();
	RTC_read();
	// NOTE: must be called after core.load_game!
	core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD); // set a default, may update after loading configs
	Core_updateAVInfo();

	// RA contract: the core's context_reset runs once per GL context, after
	// retro_load_game has returned -- RA calls it from drivers_init() right
	// after video_driver_init_internal() (retroarch.c:1647-1648), and
	// drivers_init is not reachable from inside retro_load_game. Both GLES
	// cores negotiate hw render *inside* load_game (flycast:
	// set_opengl_hw_render; mupen64plus_next: glsm_state_ctx_init ->
	// SET_HW_RENDER), so the reset has to be issued from here:
	//   - mupen64plus_next defers plugin_connect_all() to the first
	//     context_reset after load_game (libretro.c:135 first_context_reset,
	//     reinit_gfx_plugin at :516-520); without it `gfx` stays zeroed and
	//     main_run calls gfx.romOpen() through a NULL pointer (pc=0);
	//   - flycast's renderer is (re)built by its context_reset callback
	//     (libretro.cpp:1154-1163), which is where glsm caches the frontend
	//     FBO (glsm.c:2759).
	// Exactly one reset: a second one inside the same context makes mupen's
	// glsm take its window-change path (glsm.c:3365-3375) and crash before
	// RomOpen; see the comment in MA_GL_set_hw_render().
	// No-op for software cores (MA_GL_context_reset returns early when no
	// hw-render context was negotiated).
	MA_GL_context_reset();
}
void Core_reset(void) {
	core.reset();
	Rewind_on_state_change();
}
void Core_unload(void) {
	// Disabling this is a dumb hack for bluetooth, we should really be using 
	// bluealsa with --keep-alive=-1 - but SDL wont reconnect the stream on next start.
	// Reenable as soon as we have a more recent SDL available, if ever.
	//SND_quit();
}
void Core_quit(void) {
	if (core.initialized) {
		SRAM_write();
		Cheats_free();
		RTC_write();
		core.unload_game();
		core.deinit();
		core.initialized = 0;
	}
}
void Core_close(void) {
	if (core.handle) dlclose(core.handle);
}
