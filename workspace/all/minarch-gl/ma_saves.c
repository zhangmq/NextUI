#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "ma_internal.h"
#include "ra_integration.h"
#include "notification.h"

#ifdef HAS_SRM
#include "streams/rzip_stream.h"
#include "streams/file_stream.h"
#endif

#include "ma_saves.h"

///////////////////////////////////////

static void formatSavePath(char* work_name, char* filename, const char* suffix) {
	char* tmp = strrchr(work_name, '.');
	if (tmp != NULL && strlen(tmp) > 2 && strlen(tmp) <= 5) {
		tmp[0] = '\0';
	}
	sprintf(filename, "%s/%s%s", core.saves_dir, work_name, suffix);
}

static void SRAM_getPath(char* filename) {
	char work_name[MAX_PATH];

	if (CFG_getSaveFormat() == SAVE_FORMAT_SRM
	 || CFG_getSaveFormat() == SAVE_FORMAT_SRM_UNCOMPRESSED) {
		strcpy(work_name, game.alt_name);
		formatSavePath(work_name, filename, ".srm");
	}
	else if (CFG_getSaveFormat() == SAVE_FORMAT_GEN) {
		strcpy(work_name, game.alt_name);
		formatSavePath(work_name, filename, ".sav");
	}
	else {
		sprintf(filename, "%s/%s.sav", core.saves_dir, game.alt_name);
	}

	LOG_info("SRAM_getPath %s\n", filename);
}

void SRAM_read(void) {
	size_t sram_size = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
	if (!sram_size) return;

	char filename[MAX_PATH];
	SRAM_getPath(filename);
	printf("sav path (read): %s\n", filename);

	void* sram = core.get_memory_data(RETRO_MEMORY_SAVE_RAM);

#ifdef HAS_SRM
	rzipstream_t* sram_file = rzipstream_open(filename, RETRO_VFS_FILE_ACCESS_READ);
	if(!sram_file) return;

	if (!sram || rzipstream_read(sram_file, sram, sram_size) < 0)
		LOG_error("rzipstream: Error reading SRAM data\n");

	rzipstream_close(sram_file);
#else
	FILE *sram_file = fopen(filename, "r");
	if (!sram_file) return;
	if (!sram || !fread(sram, 1, sram_size, sram_file)) {
		LOG_error("Error reading SRAM data\n");
	}
	fclose(sram_file);
#endif
}

void SRAM_write(void) {
	size_t sram_size = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
	if (!sram_size) return;

	char filename[MAX_PATH];
	SRAM_getPath(filename);
	printf("sav path (write): %s\n", filename);

	void *sram = core.get_memory_data(RETRO_MEMORY_SAVE_RAM);

#ifdef HAS_SRM
	if (CFG_getSaveFormat() == SAVE_FORMAT_SRM) {
		if(!rzipstream_write_file(filename, sram, sram_size))
			LOG_error("rzipstream: Error writing SRAM data to file\n");
	}
	else {
		if(!filestream_write_file(filename, sram, sram_size))
			LOG_error("filestream: Error writing SRAM data to file\n");
	}
#else
	FILE *sram_file = fopen(filename, "w");
	if (!sram_file) {
		LOG_error("Error opening SRAM file: %s\n", strerror(errno));
		return;
	}
	if (!sram || sram_size != fwrite(sram, 1, sram_size, sram_file)) {
		LOG_error("Error writing SRAM data to file\n");
	}
	fclose(sram_file);
#endif
	sync();
}

///////////////////////////////////////

static void RTC_getPath(char* filename) {
	sprintf(filename, "%s/%s.rtc", core.saves_dir, game.alt_name);
}
void RTC_read(void) {
	size_t rtc_size = core.get_memory_size(RETRO_MEMORY_RTC);
	if (!rtc_size) return;

	char filename[MAX_PATH];
	RTC_getPath(filename);
	printf("rtc path (read): %s\n", filename);

	FILE *rtc_file = fopen(filename, "r");
	if (!rtc_file) return;

	void* rtc = core.get_memory_data(RETRO_MEMORY_RTC);

	if (!rtc || !fread(rtc, 1, rtc_size, rtc_file)) {
		LOG_error("Error reading RTC data\n");
	}

	fclose(rtc_file);
}
void RTC_write(void) {
	size_t rtc_size = core.get_memory_size(RETRO_MEMORY_RTC);
	if (!rtc_size) return;

	char filename[MAX_PATH];
	RTC_getPath(filename);
	printf("rtc path (write) size(%u): %s\n", rtc_size, filename);

	FILE *rtc_file = fopen(filename, "w");
	if (!rtc_file) {
		LOG_error("Error opening RTC file: %s\n", strerror(errno));
		return;
	}

	void *rtc = core.get_memory_data(RETRO_MEMORY_RTC);

	if (!rtc || rtc_size != fwrite(rtc, 1, rtc_size, rtc_file)) {
		LOG_error("Error writing RTC data to file\n");
	}

	fclose(rtc_file);

	sync();
}

///////////////////////////////////////

int state_slot = 0;

void State_getPath(char* filename) {
	char work_name[MAX_PATH];

	// This is only here for compatibility with older versions of minarch,
	// should probably be removed at some point in the future.
	if (CFG_getStateFormat() == STATE_FORMAT_SRM_EXTRADOT
	 || CFG_getStateFormat() == STATE_FORMAT_SRM_UNCOMRESSED_EXTRADOT) {
		strcpy(work_name, game.alt_name);
		char* tmp = strrchr(work_name, '.');
		if (tmp != NULL && strlen(tmp) > 2 && strlen(tmp) <= 5) {
			tmp[0] = '\0';
		}

		if(state_slot == AUTO_RESUME_SLOT)
			sprintf(filename, "%s/%s.state.auto", core.states_dir, work_name);
		else
			sprintf(filename, "%s/%s.state.%i", core.states_dir, work_name, state_slot);
	}
	else if (CFG_getStateFormat() == STATE_FORMAT_SRM
	 	  || CFG_getStateFormat() == STATE_FORMAT_SRM_UNCOMRESSED) {
		strcpy(work_name, game.alt_name);
		char* tmp = strrchr(work_name, '.');
		if (tmp != NULL && strlen(tmp) > 2 && strlen(tmp) <= 5) {
			tmp[0] = '\0';
		}

		if(state_slot == AUTO_RESUME_SLOT)
			sprintf(filename, "%s/%s.state.auto", core.states_dir, work_name);
		else if(state_slot == 0)
			sprintf(filename, "%s/%s.state", core.states_dir, work_name);
		else
			sprintf(filename, "%s/%s.state%i", core.states_dir, work_name, state_slot);
	}
	else {
		sprintf(filename, "%s/%s.st%i", core.states_dir, game.alt_name, state_slot);
	}
}

#define RASTATE_HEADER_SIZE 16
int State_read(void) { // from picoarch
	// Block load states in RetroAchievements hardcore mode
	if (RA_isHardcoreModeActive()) {
		LOG_info("State load blocked - hardcore mode active\n");
		Notification_push(NOTIFICATION_ACHIEVEMENT, "Load states disabled in Hardcore mode", NULL);
		return 0;
	}

	int success = 0;

	// RA semantics (runloop.c command_event_load_auto_state ->
	// content_load_state): no serialize_size gate, no serialize_size
	// sizing -- an existing state file is read in full and handed to
	// retro_unserialize; failure is the core's call and just logged.
	// (flycast's serialize_size is timing dependent: it differs between
	// save time and an early startup auto-resume, so gating/sizing on it
	// would skip or truncate valid states.)
	size_t state_size = 0;
	int was_ff = fast_forward;
	fast_forward = 0;

	void *state = NULL;

	char filename[MAX_PATH];
	State_getPath(filename);

	uint8_t rastate_header[RASTATE_HEADER_SIZE] = {0};

#ifdef HAS_SRM
	rzipstream_t *state_rzfile = NULL;

	state_rzfile = rzipstream_open(filename, RETRO_VFS_FILE_ACCESS_READ);
	if(!state_rzfile) {
	  if (state_slot!=8) { // st8 is a default state in MiniUI and may not exist, that's okay
		LOG_error("Error opening state file: %s (%s)\n", filename, strerror(errno));
	  }
	  goto error;
	}
	if (rzipstream_read(state_rzfile, rastate_header, RASTATE_HEADER_SIZE) < RASTATE_HEADER_SIZE) {
	  LOG_error("Error reading rastate header from file: %s (%s)\n", filename, strerror(errno));
	  goto error;
	}

	if (memcmp(rastate_header, "RASTATE", 7) != 0) {
	  // This file only contains raw core state data
	  rzipstream_rewind(state_rzfile);
	}
	// No need to parse the header any further
	// (we only need MEM section which will always be the first one)

	// Read the FULL stored payload, not core.serialize_size(): flycast's
	// serialize_size is an estimate that can differ between save time
	// (TA buffer allocated) and a startup auto-resume (core not fully
	// booted yet -- first_run path), so sizing the read by the current
	// value truncates the file and the core's deserializer fails. The RZIP
	// header records the exact uncompressed length written at save time;
	// retro_unserialize parses the stream by its own structure, so feeding
	// it the complete payload is correct regardless of the current
	// serialize_size.
	int64_t content_len = rzipstream_get_size(state_rzfile);
	if (content_len <= 0) {
		LOG_error("Error getting state size from file: %s\n", filename);
		goto error;
	}
	if (content_len != (int64_t)state_size) {
		if (content_len > (int64_t)state_size) {
			void *bigger = realloc(state, (size_t)content_len);
			if (!bigger) {
				LOG_error("Couldn't grow state buffer to %lld bytes\n",
						(long long)content_len);
				goto error;
			}
			state = bigger;
		}
		state_size = (size_t)content_len;
	}

	if (rzipstream_read(state_rzfile, state, state_size) != (int64_t)state_size) {
	  LOG_error("Error reading state data from file: %s (%s)\n", filename, strerror(errno));
	  goto error;
	}

	if (!core.unserialize(state, state_size)) {
	  LOG_error("Error restoring save state: %s\n", filename);
	  goto error;
	}
	success = 1;

error:
	if (state) free(state);
	if (state_rzfile) rzipstream_close(state_rzfile);
#else
	FILE *state_file = fopen(filename, "r");
	if (!state_file) {
		if (state_slot!=8) { // st8 is a default state in MiniUI and may not exist, that's okay
			LOG_error("Error opening state file: %s (%s)\n", filename, strerror(errno));
		}
		goto error;
	}

	if (fread(rastate_header, 1, RASTATE_HEADER_SIZE, state_file) < RASTATE_HEADER_SIZE) {
		LOG_error("Error reading rastate header from file: %s (%s)\n", filename, strerror(errno));
		goto error;
	}

	if (memcmp(rastate_header, "RASTATE", 7) != 0) {
	  // This file only contains raw core state data; rewind
	  fseek(state_file, 0, SEEK_SET);
	}
	// Raw (non-RZIP) state: size the read by the actual file length for the
	// same reason as the RZIP branch above (serialize_size is timing
	// dependent on flycast).
	{
		fseek(state_file, 0, SEEK_END);
		long content_len = ftell(state_file);
		fseek(state_file, 0, SEEK_SET);
		if (content_len <= 0) {
			LOG_error("Error getting state size from file: %s\n", filename);
			goto error;
		}
		if ((size_t)content_len != state_size) {
			if ((size_t)content_len > state_size) {
				void *bigger = realloc(state, (size_t)content_len);
				if (!bigger) {
					LOG_error("Couldn't grow state buffer to %ld bytes\n", content_len);
					goto error;
				}
				state = bigger;
			}
			state_size = (size_t)content_len;
		}
	}

	if (fread(state, 1, state_size, state_file) != state_size) {
		LOG_error("Error reading state data from file: %s (%s)\n", filename, strerror(errno));
		goto error;
	}

	if (!core.unserialize(state, state_size)) {
		LOG_error("Error restoring save state: %s\n", filename);
		goto error;
	}
	success = 1;

error:
	if (state) free(state);
	if (state_file) fclose(state_file);
#endif
	fast_forward = was_ff;
	return success;
}

int State_write(void) { // from picoarch
	// Block save states in RetroAchievements hardcore mode
	if (RA_isHardcoreModeActive()) {
		LOG_info("State save blocked - hardcore mode active\n");
		Notification_push(NOTIFICATION_ACHIEVEMENT, "Save states disabled in Hardcore mode", NULL);
		return 0;
	}

	int success = 0;
	size_t state_size = core.serialize_size();
	if (!state_size) return 0;

	int was_ff = fast_forward;
	fast_forward = 0;

	void *state = calloc(1, state_size);
	if (!state) {
		LOG_error("Couldn't allocate memory for state\n");
		goto error;
	}

	bool serialize_ok = core.serialize(state, state_size);
	if (!serialize_ok) {
		LOG_error("Error serializing save state\n");
		goto error;
	}

	char filename[MAX_PATH];
	State_getPath(filename);
#ifdef HAS_SRM
	if (CFG_getStateFormat() == STATE_FORMAT_SRM || CFG_getStateFormat() == STATE_FORMAT_SRM_EXTRADOT) {
		if(!rzipstream_write_file(filename, state, state_size)) {
			LOG_error("rzipstream: Error writing state data to file: %s\n", filename);
			goto error;
		}
		success = 1;
	}
	else {
		if(!filestream_write_file(filename, state, state_size)) {
			LOG_error("filestream: Error writing state data to file: %s\n", filename);
			goto error;
		}
		success = 1;
	}

error:
	if (state) free(state);
#else
	FILE *state_file = fopen(filename, "w");
	if (!state_file) {
		LOG_error("Error opening state file: %s (%s)\n", filename, strerror(errno));
		goto error;
	}
	if (state_size != fwrite(state, 1, state_size, state_file)) {
		LOG_error("Error writing state data to file: %s (%s)\n", filename, strerror(errno));
		goto error;
	}
	success = 1;
	error:
	if (state) free(state);
	if (state_file) fclose(state_file);
#endif

	sync();
	fast_forward = was_ff;
	return success;
}

void State_autosave(void) {
	int last_state_slot = state_slot;
	state_slot = AUTO_RESUME_SLOT;
	State_write();
	state_slot = last_state_slot;
}
void State_resume(void) {
	if (!exists(RESUME_SLOT_PATH)) return;

	int last_state_slot = state_slot;
	state_slot = getInt(RESUME_SLOT_PATH);
	unlink(RESUME_SLOT_PATH);
	State_read();
	state_slot = last_state_slot;
	Rewind_on_state_change();
}
