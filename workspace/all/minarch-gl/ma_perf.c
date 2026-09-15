// ma_perf.c -- the frontend performance interface.
//
// Required by cores that time things with the frontend's clock. flycast's
// retro_serialize_size() -> wait_until_dc_running() calls
// perf_cb.get_time_usec(); without this interface the callback is NULL and the
// core jumps to address 0 (SIGSEGV @ (nil)) during State_resume.
//
// Only the clock is real: the counter/register/start/stop/log hooks are stubs,
// which is what this frontend's perf logging needs (none).
#include <time.h>

#include "ma_perf.h"

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

bool MA_perf_fill(struct retro_perf_callback *perf) {
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
