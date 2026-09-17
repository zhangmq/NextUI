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
void core_input_poll_callback(void) {
	// RA core_input_state_poll_maybe (runloop.c:5069-5078): the callback the
	// core calls polls only under NORMAL.  DONTCARE(0) is RA's default for a
	// core (POLL_TYPE_NORMAL), and EARLY(1)/LATE(3) are polled by the
	// frontend instead (the main loop / core_input_state_callback below).
	if (input_poll_type_override == 0 || input_poll_type_override == 2)
		input_poll_callback();
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
