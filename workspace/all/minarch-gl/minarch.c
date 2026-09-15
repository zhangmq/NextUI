#include <stdlib.h>
#include <msettings.h>

#include <SDL2/SDL_image.h>

#include "notification.h"
#include "ra_integration.h"

#include "ma_internal.h"
#include "ma_cheats.h"
#include "ma_audio.h"
#include "ma_input.h"
#include "ma_options.h"
#include "ma_frontend_opts.h"
#include "ma_saves.h"
#include "ma_video.h"
#include "ma_core.h"
#include "ma_game.h"
#include "ma_gl.h"
#include "ma_present.h"
#include "ma_environment.h"
#include "ma_config.h"
#include "ma_runframe.h"

///////////////////////////////////////

SDL_Surface* screen;
int quit = 0;
int newScreenshot = 0;
int show_menu = 0;
int simple_mode = 0;
enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;

// default frontend options
int screen_scaling = SCALE_ASPECT;
int resampling_quality = 2;
int ambient_mode = 0;
int screen_sharpness = SHARPNESS_SOFT;
int screen_effect = EFFECT_NONE;
int cfg_screenx = 64;
int cfg_screeny = 64;
int overlay = 0; 
int use_core_fps = 0;
int sync_ref = 0;
volatile int show_debug = 0;
int max_ff_speed = 3; // 4x
int ff_audio = 0;
int fast_forward = 0;
int rewind_pressed = 0;
int rewind_toggle = 0;
int last_rewind_pressed = 0;
int ff_toggled = 0;
int ff_hold_active = 0;
int ff_paused_by_rewind_hold = 0;
int rewinding = 0;
int rewind_cfg_enable = MINARCH_DEFAULT_REWIND_ENABLE;
int rewind_cfg_buffer_mb = MINARCH_DEFAULT_REWIND_BUFFER_MB;
int rewind_cfg_granularity = MINARCH_DEFAULT_REWIND_GRANULARITY;
int rewind_cfg_audio = MINARCH_DEFAULT_REWIND_AUDIO;
int rewind_cfg_compress = 1;
int rewind_cfg_lz4_acceleration = MINARCH_DEFAULT_REWIND_LZ4_ACCELERATION;
int rewind_init_ready = 0; // gate Rewind_init from syncFrontend until startup is past Core_load
int overclock = 0; // auto
int has_custom_controllers = 0;
int gamepad_type = 0; // index in gamepad_labels/gamepad_values

// these are no longer constants as of the RG CubeXX (even though they look like it)
int DEVICE_WIDTH = 0;
int DEVICE_HEIGHT = 0;
int DEVICE_PITCH = 0;
int shader_reset_suppressed = 0;

GFX_Renderer renderer;

///////////////////////////////////////

struct Core core;



///////////////////////////////
static struct Special {
	int palette_updated;
} special;
void Special_updatedDMGPalette(int frames) {
	// LOG_info("Special_updatedDMGPalette(%i)\n", frames);
	special.palette_updated = frames; // must wait a few frames
}
static void Special_refreshDMGPalette(void) {
	special.palette_updated -= 1;
	if (special.palette_updated>0) return;
	
	int rgb = getInt("/tmp/dmg_grid_color");
	GFX_setEffectColor(rgb);
}
static void Special_init(void) {
	if (special.palette_updated>1) special.palette_updated = 1;
	// else if (exactMatch((char*)core.tag, "GBC"))  {
	// 	putInt("/tmp/dmg_grid_color",0xF79E);
	// 	special.palette_updated = 1;
	// }
}
void Special_render(void) {
	if (special.palette_updated) Special_refreshDMGPalette();
}
static void Special_quit(void) {
	system("rm -f /tmp/dmg_grid_color");
}
///////////////////////////////

///////////////////////////////

void hdmimon(void) {
	// handle HDMI change
	static int had_hdmi = -1;
	int has_hdmi = GetHDMI();
	if (had_hdmi==-1) had_hdmi = has_hdmi;
	if (has_hdmi!=had_hdmi) {
		had_hdmi = has_hdmi;

		LOG_info("restarting after HDMI change...\n");
		Menu_beforeSleep();
		sleep(1); // cable-seating debounce; the output switch itself happens at next app start
		show_menu = 0;
		quit = 1;
	}
}

#define PWR_UPDATE_FREQ 5
#define PWR_UPDATE_FREQ_INGAME 20

// Pacing lives here, in the run loop, because it is a run-loop decision -- not
// a GL, SDL or core one. It composes the RA runloop pace bits (runloop.c
// RUNLOOP_PACE_*):
//
//   PACE_AUDIO  the blocking audio write (audio_sample_batch_callback) already
//               held retro_run to the sound card's drain rate. That is the one
//               real-time master clock -- the only reference that cannot drift
//               against what the user hears. Nothing is added on top of it.
//   PACE_TIMER  fallback for frames that wrote no audio at all (silent scenes,
//               muted cores, cores without an audio device): schedule the
//               present at frame_index * 1/fps on an absolute timeline, sleep
//               the bulk and busy-wait the remainder. Re-anchor when more than
//               two frames are lost.
//
// Skipped while fast-forwarding: limitFF (ma_runframe.c) owns that cadence, and
// while rewinding the loop is driven by the rewind cadence.
//
// This is deliberately the ONLY sleep/pacing site in the loop. Both the old
// software-path timer (GFX_flip_fixed_rate, called from inside the video
// callback) and the old hw-only copy in ma_gl.c were the same schedule with
// different guards; keeping two of them meant the two core families were paced
// by different clocks and only one of them respected audio backpressure.
static void pace_frame(void) {
	if (fast_forward || rewinding) return;
	double fps = core.fps;
	if (fps <= 0.0) return;

	if (ma_audio_wrote_frame)
		return; // this frame is already paced by the sound card
	if (MA_present_blocked())
		return; // ...or by the display (RA PACE_VSYNC)

	static int64_t frame_index = -1;
	static int64_t first_frame_start_time = 0;
	static double last_fps = 0.0;

	int64_t perf_freq = SDL_GetPerformanceFrequency();
	int64_t now = SDL_GetPerformanceCounter();

	if (++frame_index == 0 || fps != last_fps) {
		frame_index = 0;
		first_frame_start_time = now;
		last_fps = fps;
	}

	int64_t frame_duration = perf_freq / fps;
	int64_t time_of_frame = first_frame_start_time + frame_index * frame_duration;
	int64_t offset = now - time_of_frame;
	const int64_t max_lost_frames = 2;

	if (offset < 0) {
		useconds_t time_to_sleep_us = (useconds_t)((time_of_frame - now) * 1e6 / perf_freq);
		const useconds_t min_waiting_time = 2000;
		if (time_to_sleep_us > min_waiting_time)
			usleep(time_to_sleep_us - min_waiting_time);
		while (SDL_GetPerformanceCounter() < time_of_frame) {
			// busy-wait the remainder for accurate alignment
		}
	} else if (offset > max_lost_frames * frame_duration) {
		// fell behind by more than 2 frames: re-anchor the timeline
		frame_index = -1;
		last_fps = 0.0;
	}
}

// Frame-time distribution, off unless MINARCH_FRAME_LOG is set in the
// environment. The on-screen HUD only shows a windowed average, which hides
// the distinction that matters when a device stutters: is a slow frame a rare
// phase-beat (one 33ms frame among 16.7ms ones -- audio clock beating against
// vsync) or a systematic overrun (every frame 33ms -- the frame simply does not
// fit)? We cannot answer that from inside the loop with one number, and the HUD
// itself perturbs the measurement, so this logs a bucket histogram instead and
// costs nothing when disabled.
static void frame_histogram_tick(void) {
	// env var for a shell that has one, or just drop the marker file (no
	// device script has to be edited to start a measurement run)
	static int enabled = -1;
	if (enabled < 0) {
		const char *v = getenv("MINARCH_FRAME_LOG");
		enabled = (v && v[0] && v[0] != '0') || exists("/tmp/minarch_frame_log");
	}
	if (!enabled) return;

	enum { NB = 7 };
	static uint32_t buckets[NB];
	static uint32_t frames = 0;
	static double   sum_ms = 0.0;
	static double   worst_ms = 0.0;
	static uint64_t last_counter = 0;
	static uint32_t last_log_ms = 0;

	uint64_t now = SDL_GetPerformanceCounter();
	if (!last_counter) { last_counter = now; last_log_ms = SDL_GetTicks(); return; }
	double ms = (double)(now - last_counter) * 1000.0
			/ (double)SDL_GetPerformanceFrequency();
	last_counter = now;
	if (ms <= 0.0 || ms > 1000.0) return; // 1s+ means we were not measuring

	int b;
	if      (ms < 17.0) b = 0; // fits a 60Hz frame
	else if (ms < 19.0) b = 1; // a hair over
	else if (ms < 25.0) b = 2; // clearly late but not a full drop
	else if (ms < 35.0) b = 3; // one dropped frame (2 vsync periods)
	else if (ms < 50.0) b = 4;
	else if (ms < 70.0) b = 5;
	else                b = 6;
	buckets[b]++;
	frames++;
	sum_ms += ms;
	if (ms > worst_ms) worst_ms = ms;

	uint32_t now_ms = SDL_GetTicks();
	if (now_ms - last_log_ms >= 5000) {
		double elapsed = (double)(now_ms - last_log_ms);
		LOG_info("minarch: frames over %.1fs: %u total, %.1f fps, %.2f ms avg, %.1f ms worst; "
				"buckets <17/<19/<25/<35/<50/<70/>=70ms = %u/%u/%u/%u/%u/%u/%u\n",
				elapsed / 1000.0, frames, frames * 1000.0 / elapsed, sum_ms / (frames ? frames : 1), worst_ms,
				buckets[0], buckets[1], buckets[2], buckets[3], buckets[4], buckets[5], buckets[6]);
		{
			uint64_t s = 0, mx = 0, gmin = 0;
			unsigned n = 0, o1 = 0, oh = 0, gn = 0, g5 = 0, g12 = 0, g20 = 0, gge = 0;
			MA_present_take_stats(&s, &mx, &n, &o1, &oh, &gn, &gmin, &g5, &g12, &g20, &gge);
			if (n)
				LOG_info("minarch: present over the same window: %u swaps, %.0f us avg, "
						"%llu us max, %u over 1ms, %u over 8.3ms\n",
						n, (double)s / (double)n, (unsigned long long)mx, o1, oh);
			if (gn)
				LOG_info("minarch: present gaps: %u gaps, min %llu us; <5/<12/<20/>=20ms = %u/%u/%u/%u\n",
						gn, (unsigned long long)gmin, g5, g12, g20, gge);
		}
		{
			unsigned nf = 0, dp = 0; uint64_t ds = 0, dm = 0;
			MA_GL_take_present_stats(&nf, &dp, &ds, &dm);
			if (nf || dp)
				LOG_info("minarch: hw video_cb: %u new, %u dupe; our draw %.0f us avg, %llu us max\n",
						nf, dp, (double)ds / (double)(nf + dp), (unsigned long long)dm);
		}
		memset(buckets, 0, sizeof(buckets));
		frames = 0; sum_ms = 0.0; worst_ms = 0.0;
		last_log_ms = now_ms;
	}
}

int main(int argc , char* argv[]) {
	//static char asoundpath[MAX_PATH];
	//sprintf(asoundpath, "%s/.asoundrc", getenv("HOME"));
	//LOG_info("minarch: need asoundrc at %s\n", asoundpath);
	//if(exists(asoundpath))
	//	LOG_info("asoundrc exists at %s\n", asoundpath);
	//else 
	//	LOG_info("asoundrc does not exist at %s\n", asoundpath);

	if(argc < 2)
		return EXIT_FAILURE;

	PWR_setCPUSpeed(CPU_SPEED_PERFORMANCE); // start in performance mode for fast loading
	PWR_pinToCores(CPU_CORE_PERFORMANCE); // thread affinity

	char core_path[MAX_PATH];
	char rom_path[MAX_PATH];
	char tag_name[MAX_PATH];

	strcpy(core_path, argv[1]);
	strcpy(rom_path, argv[2]);
	getEmuName(rom_path, tag_name);
	
	LOG_info("rom_path: %s\n", rom_path);
	
	screen = GFX_init(MODE_MENU);

	// initialize default shaders
	GFX_initShaders();
	PLAT_initNotificationTexture();

	PAD_init();
	DEVICE_WIDTH = screen->w;
	DEVICE_HEIGHT = screen->h;
	DEVICE_PITCH = screen->pitch;
	// LOG_info("DEVICE_SIZE: %ix%i (%i)\n", DEVICE_WIDTH,DEVICE_HEIGHT,DEVICE_PITCH);
	
	LEDS_initLeds();
	VIB_init();
	PWR_init();
	if (!HAS_POWER_BUTTON)
		PWR_disableSleep();
	MSG_init();
	IMG_Init(IMG_INIT_PNG);
	Core_open(core_path, tag_name);

	Game_open(rom_path); // nes tries to load gamegenie setting before this returns ffs
	if (!game.is_open) goto finish;
	
	simple_mode = exists(SIMPLE_MODE_PATH);
	
	// restore options
	Config_load(); // before init?
	Config_init();
	Config_readOptions(); // cores with boot logo option (eg. gb) need to load options early
	
	Core_init();

	// Initialize RetroAchievements after core.init() but before Core_load()
	// Set up memory accessors for achievement memory reading
	RA_setMemoryAccessors(core.get_memory_data, core.get_memory_size);
	RA_init();

	// TODO: find a better place to do this
	// mixing static and loaded data is messy
	// why not move to Core_init()?
	Menu_setCoreVersionDesc(core.version);
	Core_load();

	// GLES hardware-render cores (flycast) negotiate the GL context during
	// retro_load_game; MA_GL_set_hw_render already ran context_reset inside
	// the SET_HW_RENDER callback (before load_game returned), so the core's
	// emu thread has a valid GL context from the start.
	
	Input_init(NULL);
	Config_readOptions(); // but others load and report options later (eg. nes)
	Config_readControls(); // restore controls (after the core has reported its defaults)

	// Mute audio during startup to avoid pops (InitSettings would be logical, but too late)
	SND_overrideMute(1);
	SND_init(core.sample_rate, core.fps);
	SND_registerDeviceWatcher(Audio_onSinkChanged);
	InitSettings(); // after we initialize audio
	Menu_init();
	Notification_init();
	
	// Load game for RetroAchievements tracking (must be after Notification_init)
	// Pass ROM data if available, otherwise just path (for cores that load from file)
	{
		char* rom_path_for_ra = game.tmp_path[0] ? game.tmp_path : game.path;
		RA_loadGame(rom_path_for_ra, game.data, game.size, core.tag);
	}
	
	State_resume();
	Menu_initState(); // make ready for state shortcuts

	PWR_disableAutosleep();
	// we dont need five second updates while ingame, and wifi status isnt displayed either
	PWR_updateFrequency(PWR_UPDATE_FREQ, 0); 

	// force a vsync immediately before loop
	// for better frame pacing?
	GFX_clearAll();
	GFX_clearLayers(0);
	GFX_clear(screen);

	// need to draw real black background first otherwise u get weird pixels sometimes

	GFX_flip(screen);

	Special_init(); // after config

	chooseSyncRef();
	
	int has_pending_opt_change = 0;

	// then initialize custom  shaders from settings
	initShaders();
	Config_readOptions();
	applyShaderSettings();
	int rewind_initialized = Rewind_init(core.serialize_size ? core.serialize_size() : 0);
	rewind_init_ready = 1;  // Mark setup as attempted, even if rewind init failed, so option changes can retry it later.
	if (rewind_initialized && core.serialize_size) Rewind_on_state_change();
	// release config when all is loaded
	Config_free();

	LOG_info("total startup time %ims\n\n",SDL_GetTicks());
	LOG_info("minarch: entering main loop, MA_GL_active=%d\n", MA_GL_is_active());
	
	// we started in performance mode, now reset to the desired mode
	// if the config didn't specify the desired cpu speed, the default is 0 = auto
	setOverclock(overclock);

	while (!quit) {
		GFX_startFrame();

		/* Clear the "audio wrote this frame" flag (read by pace_frame for
		 * RA-style pace composition). */
		ma_audio_wrote_frame = 0;

		run_frame();

		// Hardware-render present, on this thread and once per frame -- the
		// software path presents from its video callback (also this thread).
		// See MA_GL_present_from_loop (no-op for software cores).
		MA_GL_present_from_loop();

		// The single pacing site for both core families (see pace_frame).
		pace_frame();

		// Measurement only (MINARCH_FRAME_LOG); see frame_histogram_tick.
		frame_histogram_tick();

		// GL hw-render debug HUD: the present paths' statistics sampler
		// (GFX_flip/GFX_GL_Swap) never runs for hw-render cores, and its
		// current_fps must NOT be fed from this loop either -- current_fps is
		// the audio resample denominator, and writing it here made the audio
		// pitch follow the main-loop rate. MA_GL_hud_stats_tick keeps its own
		// state and fills only the perf display fields.
		//
		// The CPU/GPU telemetry is polled on this (main) thread -- not on the
		// core's render thread -- once per frame while the HUD is shown;
		// perf.cpu_usage comes from the separate CPU monitor thread
		// (updateCPUMonitor), enabled together with show_debug. The TTF text
		// rasterization (MA_GL_hud_update) also stays here because the
		// notification/menu code uses the same font objects on this thread;
		// the video-callback thread only uploads and draws the panel.
		if (show_debug) {
			// Only the hw-render path needs its own frame-stats sampler; the
			// software paths already fill perf.fps/avg/max from their present
			// sampler (feeding MA_GL_hud_stats_tick there would double-count).
			if (MA_GL_is_active())
				MA_GL_hud_stats_tick();
			PLAT_getCPUTemp();
			PLAT_getCPUSpeed();
			PLAT_getGPUTemp();
			PLAT_getGPUSpeed();
			PLAT_getGPUUsage();
			MA_GL_hud_update();
		}
		
		// Process RetroAchievements for this frame
		RA_doFrame();
		
		// Update and render notifications overlay
		Notification_update(SDL_GetTicks());
		
		// Poll for volume/brightness/colortemp changes and show system indicators
		{
			static int last_volume = -1;
			static int last_brightness = -1;
			static int last_colortemp = -1;
			
			int cur_volume = GetVolume();
			int cur_brightness = GetBrightness();
			int cur_colortemp = GetColortemp();
			
			if (last_volume == -1) {
				// First frame - just initialize cached values, don't show indicator
				last_volume = cur_volume;
				last_brightness = cur_brightness;
				last_colortemp = cur_colortemp;
			} else {
				// Check for changes
				if (cur_volume != last_volume) {
					last_volume = cur_volume;
					if (CFG_getNotifyAdjustments())
						Notification_showSystemIndicator(SYSTEM_INDICATOR_VOLUME);
				}
				if (cur_brightness != last_brightness) {
					last_brightness = cur_brightness;
					if (CFG_getNotifyAdjustments())
						Notification_showSystemIndicator(SYSTEM_INDICATOR_BRIGHTNESS);
				}
				if (cur_colortemp != last_colortemp) {
					last_colortemp = cur_colortemp;
					if (CFG_getNotifyAdjustments())
						Notification_showSystemIndicator(SYSTEM_INDICATOR_COLORTEMP);
				}
			}
		}
		
		Notification_renderToLayer(5);  // Always call - handles cleanup when inactive

		if (has_pending_opt_change) {
			has_pending_opt_change = 0;
			if (Core_updateAVInfo()) {
				LOG_info("AV info changed, reset sound system");
				SND_resetAudio(core.sample_rate, core.fps);
			}
			chooseSyncRef();
		}

		if (show_menu) {
			PWR_updateFrequency(PWR_UPDATE_FREQ,1);
			Menu_loop();
			// The menu presents via the SDL_Renderer, which owns a separate
			// GLES2 context and leaves it current. Re-make our hw-render GL
			// context current so the next retro_run's glsm BIND + core render
			// execute in the right context (otherwise flycast renders into
			// phantom objects and its GLCache shadow state gets polluted).
			MA_GL_make_current();
			// Process RA async operations while menu is shown
			RA_idle();
			PWR_updateFrequency(PWR_UPDATE_FREQ_INGAME,0);
			has_pending_opt_change = config.core.changed;
			chooseSyncRef();
		}

		Audio_checkAndResetIfNeeded();

		hdmimon();
	}
	int cw, ch;
	unsigned char* pixels = GFX_GL_screenCapture(&cw, &ch);
	
	renderer.dst = pixels;
	SDL_Surface* rawSurface = SDL_CreateRGBSurfaceWithFormatFrom(
		pixels, cw, ch, 32, cw * 4, SDL_PIXELFORMAT_ABGR8888
	);
	SDL_Surface* converted = SDL_ConvertSurfaceFormat(rawSurface, screen->format->format, 0);
	screen = converted;
	SDL_FreeSurface(rawSurface);
	free(pixels); 
	GFX_animateSurfaceOpacity(converted, 0, 0, cw, ch, 255, 0, CFG_getMenuTransitions() ? 200 : 20, 1);
	SDL_FreeSurface(converted); 
	
	Video_cleanup();

	PLAT_clearTurbo();

	Menu_quit();
	QuitSettings();

finish:
    Perf_setCPUMonitorEnabled(0);

	// Unload game and shutdown RetroAchievements before Notification_quit —
	// RA background threads (sync, badge downloads) may call notification
	// APIs, so the notification mutex should outlive all RA threads.
	RA_unloadGame();
	RA_quit();
	Notification_quit();
	
	Game_close();
	Rewind_free();
	Core_unload();
	// GLES hardware-render cores free their GL resources here while the
	// context is still alive.
	MA_GL_context_destroy();
	Core_quit();
	Core_close();
	Config_quit();
	Special_quit();
	MSG_quit();
	PWR_quit();
	VIB_quit();
	SND_removeDeviceWatcher();
	// Disabling this is a dumb hack for bluetooth, we should really be using 
	// bluealsa with --keep-alive=-1 - but SDL wont reconnect the stream on next start.
	// Reenable as soon as we have a more recent SDL available, if ever.
	//SND_quit();
	PAD_quit();
	GFX_quit();
	Menu_waitScreenshot();
	return EXIT_SUCCESS;
}
