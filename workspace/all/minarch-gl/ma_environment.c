#include <string.h>

#include "ma_internal.h"
#include "ma_options.h"
#include "ma_input.h"
#include "ma_gl.h"
#include "ma_perf.h"
#include "ra_integration.h"
#include "ma_environment.h"

// RETRO_ENVIRONMENT_POLL_TYPE_OVERRIDE, raw core value: RA's
// enum poll_type_override_t (runloop.h:92-98)
//   DONTCARE 0 / EARLY 1 / NORMAL 2 / LATE 3
// which RA converts to its internal enum poll_type as `override - 1`
// (runloop.c:5073-5075; internal EARLY 0 / NORMAL 1 / LATE 2).  Behaviour:
//   DONTCARE  the core polls itself through our poll_cb -- and that is RA's
//             default for a core anyway (poll_type = POLL_TYPE_NORMAL,
//             runloop.c:8750);
//   EARLY     the frontend polls before retro_run;
//   NORMAL    the core polls through our poll_cb, when it asks;
//   LATE      the frontend polls on the core's first retro_input_state call of
//             the frame (RA core_input_state_poll_late, runloop.c:5060-5065).
// An earlier version of this file documented a THREE-value enum and therefore
// treated 2 as LATE (and 3 as not-a-case), which would have left a core asking
// for LATE with no poll at all.
int input_poll_type_override = 0;

// Set once per frame by the LATE lazy poll, cleared by the main loop; RA's
// RETRO_CORE_FLAG_INPUT_POLLED equivalent.
int input_state_polled_this_frame = 0;

static bool set_rumble_state(unsigned port, enum retro_rumble_effect effect, uint16_t strength) {
	// TODO: handle other args? not sure I can
	VIB_setStrength(strength);
	return 1;
}

/* RetroArch-private environment block (RA retroarch.h:45,53).  A threaded core
 * (mupen64plus_next) asks for a callback it calls to release its blocking waits
 * around state save/load.  RA answers with runloop_clear_all_thread_waits
 * (start/stop its audio driver); the core stores the pointer and calls it
 * UNCONDITIONALLY in retro_serialize/retro_unserialize
 * (mupen64plus_next libretro.c:2151/2184).  Answering false leaves it NULL and
 * every state save/load crashes the core with pc=0 (observed with SM64 EU and
 * "Threaded Renderer" enabled).  So answer it. */
#ifndef RETRO_ENVIRONMENT_RETROARCH_START_BLOCK
#define RETRO_ENVIRONMENT_RETROARCH_START_BLOCK 0x800000
#endif
#ifndef RETRO_ENVIRONMENT_GET_CLEAR_ALL_THREAD_WAITS_CB
#define RETRO_ENVIRONMENT_GET_CLEAR_ALL_THREAD_WAITS_CB (3 | RETRO_ENVIRONMENT_RETROARCH_START_BLOCK)
#endif

static bool ma_clear_all_thread_waits(unsigned clear_threads, void *data) {
	(void)data;
	/* Nothing to release on our side: hw-render cores run on the frontend's
	 * thread and minarch's audio device is SDL-owned, so unlike RA there is no
	 * frontend audio/thread state to start or stop.  Logged so the call is
	 * visible in the per-core log. */
	LOG_info("minarch: core clear-all-thread-waits -> %s\n",
			clear_threads ? "clear" : "restore");
	return true;
}

bool environment_callback(unsigned cmd, void *data) { // copied from picoarch initially
	// LOG_info("environment_callback: %i\n", cmd);

	switch(cmd) {
	case RETRO_ENVIRONMENT_SET_ROTATION: { /* 1 */
		int rotation = *(int *)data;
		// Core requests the frontend to handle rotation (flycast ROT270 games
		// render unrotated and send SET_ROTATION(1)). Only the GLES hw-render
		// path uses it; software cores (fbneo) output their own orientation.
		MA_GL_set_rotation((unsigned)rotation);
		break;
	}
	case RETRO_ENVIRONMENT_GET_CLEAR_ALL_THREAD_WAITS_CB: { /* 0x800003, RA private */
		if (data) *(retro_environment_t *)data = ma_clear_all_thread_waits;
		LOG_info("minarch: core asked for the clear-all-thread-waits callback -> provided\n");
		break;
	}
	case RETRO_ENVIRONMENT_GET_OVERSCAN: { /* 2 */
		// RA default: runloop.c:1475 answers !video_crop_overscan and
		// config.def.h:1094 defaults DEFAULT_CROP_OVERSCAN to true, i.e. the
		// core is told to crop overscan away. minarch inherited a hardcoded
		// `true` verbatim from picoarch (the whole switch is "copied from
		// picoarch initially") with no documented rationale; see
		// workspace/tmp/ra-viewport/OVERSCAN-INVESTIGATION.md.
		// Measured 2026-09-17: none of the cores shipped on the device calls
		// this env at all (snes9x, fceumm, pcsx_rearmed, picodrive, mgba,
		// fbneo, mupen64plus_next, flycast), so this answer is a no-op today
		// and only defines the contract for cores that do honour it (libretro
		// deprecated the call in 2019: libretro.h:753-755).
		bool *out = (bool *)data;
		if (out)
			*out = false;
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
		return MA_perf_fill((struct retro_perf_callback *)data);
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
			core.max_width = av->geometry.max_width;
			core.max_height = av->geometry.max_height;
			core.base_width = av->geometry.base_width;
			core.base_height = av->geometry.base_height;
			renderer.dst_p = 0;

			// RA semantics (runloop.c SET_SYSTEM_AV_INFO): the hw-render
			// FBO size follows the reported max geometry; rebuild when it
			// grew. minarch re-runs the core's context_reset like RA's
			// driver reinit, so glsm re-reads get_current_framebuffer.
			MA_GL_update_fbo_size();
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
			// RA runloop.c:3066-3068: SET_GEOMETRY only acts when the
			// meaningful fields actually changed, and it never resizes the
			// hw-render FBO (only SET_SYSTEM_AV_INFO -> CMD_EVENT_REINIT
			// does, runloop.c:2813-2850 / gl3.c:3239).
			if (core.base_width != geom->base_width
					|| core.base_height != geom->base_height
					|| core.aspect_ratio != a) {
				core.base_width = geom->base_width;
				core.base_height = geom->base_height;
				core.aspect_ratio = a;
				renderer.dst_p = 0;
			}
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
	// RETRO_ENVIRONMENT_SET_SAVE_STATE_IN_BACKGROUND (0x800002, RetroArch block)
	// Core (flycast) tells the frontend whether it supports saving states on a
	// background thread. minarch always serializes synchronously from the menu
	// (Menu_saveState -> State_write on the main thread), so the "false"
	// contract (no background saving) matches our behavior exactly. Acknowledge
	// (return true) so the core does not assume a background-thread capable
	// frontend; the value in *out belongs to the core, leave it untouched.
	case 0x800002: {
		LOG_info("minarch: SET_SAVE_STATE_IN_BACKGROUND acknowledged (synchronous saves only)\n");
		return true;
	}
	// RETRO_ENVIRONMENT_POLL_TYPE_OVERRIDE (0x800004, RetroArch block)
	// The core says WHO polls input for each frame: 1 = EARLY (frontend polls
	// before retro_run and the core will NOT call our poll_cb), 2 = LATE
	// (frontend polls after retro_run), 0 = don't care (the core calls
	// poll_cb, which is what every other core does). mupen64plus-next sends
	// EARLY when ThreadedRenderer=True (libretro/libretro.c:1026-1029), and
	// its own frame path then skips poll_cb entirely
	// (mupen64plus-core/src/main/main.c:259-263 main_check_inputs). So merely
	// acknowledging the request leaves the frontend never polling: no
	// buttons, no menu, no shortcuts -- input_poll_callback() is the one
	// place that runs PAD_poll + shortcuts (ma_input.c:17). RA implements
	// both sides in core_run (runloop.c:9116-9117 early, 9157-9159 late);
	// minarch's main loop does the same for this value.
	case 0x800004: {
		if (data) {
			input_poll_type_override = (int)*(const unsigned *)data;
			LOG_info("minarch: POLL_TYPE_OVERRIDE = %d\n", input_poll_type_override);
		}
		return true;
	}
	default:
		// LOG_debug("Unsupported environment cmd: %u\n", cmd);
		return false;
	}
	return true;
}
