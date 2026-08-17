#include <string.h>
#include <time.h>

#include "ma_internal.h"
#include "ma_options.h"
#include "ma_input.h"
#include "ma_gl.h"
#include "ra_integration.h"
#include "ma_environment.h"

// Performance interface (RETRO_ENVIRONMENT_GET_PERF_INTERFACE).
// Required by cores that time things with the frontend's clock. flycast's
// retro_serialize_size() -> wait_until_dc_running() calls
// perf_cb.get_time_usec(); without this interface the callback is NULL and
// the core jumps to address 0 (SIGSEGV @ (nil)) during State_resume.
static retro_time_t perf_get_time_usec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (retro_time_t)ts.tv_sec * 1000000 + (retro_time_t)(ts.tv_nsec / 1000);
}
static uint64_t perf_get_cpu_features(void) { return 0; }
static retro_perf_tick_t perf_get_counter(void) {
	return (retro_perf_tick_t)perf_get_time_usec();
}
static void perf_register(struct retro_perf_counter *counter) { (void)counter; }
static void perf_start(struct retro_perf_counter *counter) { (void)counter; }
static void perf_stop(struct retro_perf_counter *counter) { (void)counter; }
static void perf_log(void) {}

static bool set_rumble_state(unsigned port, enum retro_rumble_effect effect, uint16_t strength) {
	// TODO: handle other args? not sure I can
	VIB_setStrength(strength);
	return 1;
}

bool environment_callback(unsigned cmd, void *data) { // copied from picoarch initially
	// LOG_info("environment_callback: %i\n", cmd);

	switch(cmd) {
	// case RETRO_ENVIRONMENT_SET_ROTATION: { /* 1 */
	// 	LOG_info("RETRO_ENVIRONMENT_SET_ROTATION %i\n", *(int *)data); // core requests frontend to handle rotation
	// 	break;
	// }
	case RETRO_ENVIRONMENT_GET_OVERSCAN: { /* 2 */
		bool *out = (bool *)data;
		if (out)
			*out = true;
		break;
	}
	case RETRO_ENVIRONMENT_GET_CAN_DUPE: { /* 3 */
		bool *out = (bool *)data;
		if (out)
			*out = true;
		break;
	}
	case RETRO_ENVIRONMENT_SET_MESSAGE: { /* 6 */
		const struct retro_message *message = (const struct retro_message*)data;
		if (message) LOG_info("%s\n", message->msg);
		break;
	}
	case RETRO_ENVIRONMENT_SHUTDOWN: { /* 7 */
		LOG_info("Core requested shutdown\n");
		quit = 1;
		break;
	}
	case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL: { /* 8 */
		// puts("RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL");
		// TODO: used by fceumm at least
	}
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: { /* 9 */
		const char **out = (const char **)data;
		if (out) {
			*out = core.bios_dir;
		}
		break;
	}
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: { /* 10 */
		const enum retro_pixel_format *format = (const enum retro_pixel_format *)data;
		LOG_info("Requested pixel format by core: %d\n", *format); // Log the requested format (raw integer value)

		// Check if the requested format is supported
		if (*format == RETRO_PIXEL_FORMAT_XRGB8888) {
			fmt = RETRO_PIXEL_FORMAT_XRGB8888;
			LOG_info("Format supported: RETRO_PIXEL_FORMAT_XRGB8888\n");
			return true;  // Indicate success
		} else if (*format == RETRO_PIXEL_FORMAT_RGB565) {
			fmt = RETRO_PIXEL_FORMAT_RGB565;
			LOG_info("Format supported: RETRO_PIXEL_FORMAT_RGB565\n");
			return true;  // Indicate success
		}
		// Log unsupported formats
		LOG_info("Format not supported, defaulting to RGB565\n");
		fmt = RETRO_PIXEL_FORMAT_RGB565;
		return false;  // Indicate failure
	}
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS: { /* 11 */
		// puts("RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS\n");
		Input_init((const struct retro_input_descriptor *)data);
		return false;
		break;
	}
	case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE: { /* 13 */
		const struct retro_disk_control_callback *var =
			(const struct retro_disk_control_callback *)data;

		if (var) {
			memset(&disk_control_ext, 0, sizeof(struct retro_disk_control_ext_callback));
			memcpy(&disk_control_ext, var, sizeof(struct retro_disk_control_callback));
		}
		break;
	}

	// TODO: this is called whether using variables or options
	case RETRO_ENVIRONMENT_GET_VARIABLE: { /* 15 */
		// puts("RETRO_ENVIRONMENT_GET_VARIABLE ");
		struct retro_variable *var = (struct retro_variable *)data;
		if (var && var->key) {
			var->value = OptionList_getOptionValue(&config.core, var->key);
			// printf("\t%s = \"%s\"\n", var->key, var->value);
		}
		// fflush(stdout);
		break;
	}
	// TODO: I think this is where the core reports its variables (the precursor to options)
	// TODO: this is called if RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION sets out to 0
	// TODO: not used by anything yet
	case RETRO_ENVIRONMENT_SET_VARIABLES: { /* 16 */
		// puts("RETRO_ENVIRONMENT_SET_VARIABLES");
		const struct retro_variable *vars = (const struct retro_variable *)data;
		if (vars) {
			OptionList_reset();
			OptionList_vars(vars);
		}
		break;
	}
	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME: { /* 18 */
		bool flag = *(bool*)data;
		// LOG_info("%i: RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME: %i\n", cmd, flag);
		break;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: { /* 17 */
		bool *out = (bool *)data;
		if (out) {
			*out = config.core.changed;
			config.core.changed = 0;
		}
		break;
	}
	case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK: { /* 21 */
		// LOG_info("%i: RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK\n", cmd);
		break;
	}
	case RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK: { /* 22 */
		// LOG_info("%i: RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK\n", cmd);
		break;
	}
	case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE: { /* 23 */
	        struct retro_rumble_interface *iface = (struct retro_rumble_interface*)data;

	        // LOG_info("Setup rumble interface.\n");
	        iface->set_rumble_state = set_rumble_state;
		break;
	}
	case RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES: {
		unsigned *out = (unsigned *)data;
		if (out)
			*out = (1 << RETRO_DEVICE_JOYPAD) | (1 << RETRO_DEVICE_ANALOG);
		break;
	}
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: { /* 27 */
		struct retro_log_callback *log_cb = (struct retro_log_callback *)data;
		if (log_cb)
			log_cb->log = (void (*)(enum retro_log_level, const char*, ...))LOG_note; // same difference
		break;
	}
	case RETRO_ENVIRONMENT_GET_PERF_INTERFACE: { /* 28 */
		struct retro_perf_callback *perf = (struct retro_perf_callback *)data;
		if (!perf)
			return false;
		perf->get_time_usec    = perf_get_time_usec;
		perf->get_cpu_features = perf_get_cpu_features;
		perf->get_perf_counter = perf_get_counter;
		perf->perf_register    = perf_register;
		perf->perf_start       = perf_start;
		perf->perf_stop        = perf_stop;
		perf->perf_log         = perf_log;
		return true;
	}
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: { /* 31 */
		const char **out = (const char **)data;
		if (out)
			*out = core.saves_dir; // save_dir;
		break;
	}
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: { /* 32 */
		const struct retro_system_av_info *av = (const struct retro_system_av_info *)data;
		if (av) {
			double a = av->geometry.aspect_ratio;
			if (a <= 0) a = (double)av->geometry.base_width / av->geometry.base_height;

			core.fps = av->timing.fps;
			core.sample_rate = av->timing.sample_rate;
			core.aspect_ratio = a;
			renderer.dst_p = 0;
		}
		return true;
	}
	case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO: { /* 35 */
		// LOG_info("RETRO_ENVIRONMENT_SET_CONTROLLER_INFO\n");
		const struct retro_controller_info *infos = (const struct retro_controller_info *)data;
		if (infos) {
			// TODO: store to gamepad_values/gamepad_labels for gamepad_device
			const struct retro_controller_info *info = &infos[0];
			for (int i=0; i<info->num_types; i++) {
				const struct retro_controller_description *type = &info->types[i];
				if (exactMatch((char*)type->desc,"dualshock")) { // currently only enabled for PlayStation
					has_custom_controllers = 1;
					break;
				}
				// printf("\t%i: %s\n", type->id, type->desc);
			}
		}
		fflush(stdout);
		return false; // TODO: tmp
		break;
	}
	case RETRO_ENVIRONMENT_SET_MEMORY_MAPS: { /* 36 | RETRO_ENVIRONMENT_EXPERIMENTAL */
		// Core is providing its memory map for achievement checking
		const struct retro_memory_map* mmap = (const struct retro_memory_map*)data;
		RA_setMemoryMap(mmap);
		break;
	}
	case RETRO_ENVIRONMENT_SET_GEOMETRY: { /* 37 */
		const struct retro_game_geometry *geom = (const struct retro_game_geometry *)data;
		if (geom) {
			double a = geom->aspect_ratio;
			if (a <= 0) a = (double)geom->base_width / geom->base_height;
			core.aspect_ratio = a;
			renderer.dst_p = 0;
		}
		return true;
	}
	case RETRO_ENVIRONMENT_GET_LANGUAGE: { /* 39 */
		// puts("RETRO_ENVIRONMENT_GET_LANGUAGE");
		if (data) *(int *) data = RETRO_LANGUAGE_ENGLISH;
		break;
	}
	case RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER: { /* (40 | RETRO_ENVIRONMENT_EXPERIMENTAL) */
		// puts("RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER");
		break;
	}

	case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: {
		// fixes fbneo save state graphics corruption
		// puts("RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE");
		int *out_p = (int *)data;
		if (out_p) {
			int out = 0;
			out |= RETRO_AV_ENABLE_VIDEO;
			out |= RETRO_AV_ENABLE_AUDIO;
			*out_p = out;
		}
		break;
	}

	// RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS (42 | RETRO_ENVIRONMENT_EXPERIMENTAL)
	// RETRO_ENVIRONMENT_GET_VFS_INTERFACE (45 | RETRO_ENVIRONMENT_EXPERIMENTAL)
	// RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE (47 | RETRO_ENVIRONMENT_EXPERIMENTAL)
	// RETRO_ENVIRONMENT_GET_INPUT_BITMASKS (51 | RETRO_ENVIRONMENT_EXPERIMENTAL)
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: { /* 51 | RETRO_ENVIRONMENT_EXPERIMENTAL */
		bool *out = (bool *)data;
		if (out)
			*out = true;
		break;
	}
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: { /* 52 */
		// puts("RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION");
		if (data) *(unsigned *)data = 2;
		break;
	}
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS: { /* 53 */
		// puts("RETRO_ENVIRONMENT_SET_CORE_OPTIONS");
		if (data) {
			OptionList_reset();
			OptionList_init((const struct retro_core_option_definition *)data);
			Config_readOptions();
		}
		break;
	}
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL: { /* 54 */
		// puts("RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL");
		const struct retro_core_options_intl *options = (const struct retro_core_options_intl *)data;
		if (options && options->us) {
			OptionList_reset();
			OptionList_init(options->us);
			Config_readOptions();
		}
		break;
	}
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY: { /* 55 */
		// puts("RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY");
	 	if (data) {
			const struct retro_core_option_display *display = (const struct retro_core_option_display *)data;
			LOG_info("Core asked for option key %s to be %s\n", display->key, display->visible ? "visible" : "invisible");
			OptionList_setOptionVisibility(&config.core, display->key, display->visible);
		}
		break;
	}
	case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER: { /* 56 */
		// We support GLES (via the SDL GL context); advertise GLES3 so cores
		// that honor the preference (flycast) skip the Vulkan probe entirely.
		enum retro_hw_context_type *type = (enum retro_hw_context_type *)data;
		if (type) *type = RETRO_HW_CONTEXT_OPENGLES3;
		return true;
	}
	case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION: { /* 57 */
		unsigned *out =	(unsigned *)data;
		if (out) *out = 1;
		break;
	}
	case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE: { /* 58 */
		const struct retro_disk_control_ext_callback *var =
			(const struct retro_disk_control_ext_callback *)data;

		if (var) {
			memcpy(&disk_control_ext, var, sizeof(struct retro_disk_control_ext_callback));
		}
		break;
	}
	// TODO: RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION 59
	// TODO: used by mgba, (but only during frameskip?)
	// case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK: { /* 62 */
	// 	LOG_info("RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK\n");
	// 	const struct retro_audio_buffer_status_callback *cb = (const struct retro_audio_buffer_status_callback *)data;
	// 	if (cb) {
	// 		LOG_info("has audo_buffer_status callback\n");
	// 		core.audio_buffer_status = cb->callback;
	// 	} else {
	// 		LOG_info("no audo_buffer_status callback\n");
	// 		core.audio_buffer_status = NULL;
	// 	}
	// 	break;
	// }
	// TODO: used by mgba, (but only during frameskip?)
	// case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY: { /* 63 */
	// 	LOG_info("RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY\n");
	//
	// 	const unsigned *latency_ms = (const unsigned *)data;
	// 	if (latency_ms) {
	// 		unsigned frames = *latency_ms * core.fps / 1000;
	// 		if (frames < 30)
	// 			// audio_buffer_size_override = frames;
	// 			LOG_info("audio_buffer_size_override = %i (unused?)\n", frames);
	// 		else
	// 			LOG_info("Audio buffer change out of range (%d), ignored\n", frames);
	// 	}
	// 	break;
	// }

	// TODO: RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE 64
	case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE: { /* 65 */
		// const struct retro_system_content_info_override* info = (const struct retro_system_content_info_override* )data;
		// if (info) LOG_info("has overrides");
		break;
	}
	// RETRO_ENVIRONMENT_GET_GAME_INFO_EXT 66
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2: { /* 67 */
		// puts("RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2");
		if (data) {
			OptionList_reset();
			OptionList_v2_init((const struct retro_core_options_v2 *)data);
			Config_readOptions();
		}
		break;
	}
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: { /* 68 */
		// puts("RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL");
		if (data) {
			const struct retro_core_options_v2_intl *intl = (const struct retro_core_options_v2_intl *)data;
			if (intl && intl->us) {
				OptionList_reset();
				OptionList_v2_init(intl->us);
				Config_readOptions();
			}
		}
		break;
	}
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK: {  /* 69 */
		// puts("RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK");
		if (data) {
			struct retro_core_options_update_display_callback *update_display_cb = (struct retro_core_options_update_display_callback *) data;
			core.update_visibility_callback = update_display_cb->callback;
		}
		else {
			core.update_visibility_callback = NULL;
		}
		break;
	}
	// used by fceumm
	// TODO: used by gambatte for L/R palette switching (seems like it needs to return true even if data is NULL to indicate support)
	case RETRO_ENVIRONMENT_SET_VARIABLE: {
		// puts("RETRO_ENVIRONMENT_SET_VARIABLE");
		const struct retro_variable *var = (const struct retro_variable *)data;
		if (var && var->key) {
			// printf("\t%s = %s\n", var->key, var->value);
			OptionList_setOptionValue(&config.core, var->key, var->value);
			break;
		}

		int *out = (int *)data;
		if (out) *out = 1;

		break;
	}

	// unused
	// case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK: {
	// 	puts("RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK"); fflush(stdout);
	// 	break;
	// }
	// case RETRO_ENVIRONMENT_GET_THROTTLE_STATE: {
	// 	puts("RETRO_ENVIRONMENT_GET_THROTTLE_STATE"); fflush(stdout);
	// 	break;
	// }
	// case RETRO_ENVIRONMENT_GET_FASTFORWARDING: {
	// 	puts("RETRO_ENVIRONMENT_GET_FASTFORWARDING"); fflush(stdout);
	// 	break;
	// };
	case RETRO_ENVIRONMENT_SET_HW_RENDER:
	{
		struct retro_hw_render_callback *cb = (struct retro_hw_render_callback*)data;

		// Log the requested context
		LOG_info("Core requested GL context type: %d, version %d.%d\n",
			cb->context_type, cb->version_major, cb->version_minor);

		// GLES hardware render: hand the negotiation to ma_gl, which fills
		// get_proc_address / get_current_framebuffer and stashes the core's
		// context_reset / context_destroy callbacks.
		if (cb->context_type == RETRO_HW_CONTEXT_OPENGLES2
			|| cb->context_type == RETRO_HW_CONTEXT_OPENGLES3
			|| cb->context_type == RETRO_HW_CONTEXT_OPENGLES_VERSION) {
			return MA_GL_set_hw_render(cb);
		}

		// minarch has no Vulkan / desktop GL implementation: refuse honestly
		// so the core falls back to GLES (flycast: DX11 -> Vulkan -> GLES3 ->
		// GLES2). A fake "accept" here makes the core believe the context
		// exists and renders into NULL -> black screen / crash (CONTEXT.md §3).
		LOG_info("minarch: context type %d not supported, refusing (core should fall back to GLES)\n",
			cb->context_type);
		return false;
	}
	default:
		// LOG_debug("Unsupported environment cmd: %u\n", cmd);
		return false;
	}
	return true;
}
