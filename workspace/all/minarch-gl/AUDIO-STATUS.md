# Audio buffer status (cmd 62), minimum latency (63), AV enable (47)

minarch-gl used to answer `RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE` with a
constant `VIDEO|AUDIO` and left 62/63 unimplemented.  Cores that do *adaptive
frameskip* — mgba, pcsx_rearmed, Yabadabadoo — need 62 to know how full the
frontend's audio buffer is; without it Yabadabadoo logs

    Frontend has no audio buffer status interface, using fixed interval frameskip (1)

and renders every other frame for the rest of the session.

This file records what was implemented, the upstream semantics it follows, and
the measurements that acceptance was based on (RGSP / H700, 2026-10-20).

## Upstream semantics (RetroArch, `workspace/tmp/ra-current`)

| Behaviour | Reference |
|---|---|
| register/unregister 62 | `runloop.c:2541-2555` |
| per-frame call | `runloop.c:8029-8065` |
| occupancy | `100 - avail * 100 / buffer_size` |
| underrun | `occupancy < 25` (low-water mark) |
| not active | paused, or no audio device/buffer |
| 63 | `runloop.c:2556-2615` — a *minimum*, clamp 512 ms, 0/NULL resets, may reinit audio |
| 47 | `runloop.c:3241-3266` — VIDEO while the video driver is active, AUDIO while audio is active and not suspended |

The callback is called once per frame, on the frontend thread, with no audio
lock held, and is not called at all when the core never registered.

## Implementation

- `ma_environment.c`
  - **62**: store `cb->callback`; `data == NULL` (or `callback == NULL`)
    unregisters; both answer `true`.
  - **63**: clamp to 512 ms; only a request *above* the frontend default
    (8 frames of audio ≈ 133 ms at 60 fps) changes anything; the ring is
    rebuilt through the existing `SND_resetAudio()` path and the old/new sizes
    are logged.
  - **47**: `VIDEO` always (minarch always presents); `AUDIO` only when the
    audio callbacks would actually write samples — same predicate as
    `ma_audio.c` (fast-forward with `ff_audio` off, or rewind with rewind audio
    off, do not).
- `ma_audio.c`: `MA_Audio_buffer_status_tick()`, called from `minarch.c` right
  after `run_frame()` (RA's place in `core_run`), no-op unless a core
  registered.
- `api.c`: `SND_getBufferStats()` computes `frame_in`/`frame_out` occupancy
  under `audio_mutex` and returns it with the lock released; the audio callback
  records the **low-water mark** since the previous tick, because sampling only
  right after the core wrote its batch reads the ring at its fullest and makes
  a starving core look healthy on every other frame.
- `SND_setLatencyTarget()` + `SND_init()`: ring sized from the requested
  latency (never below two frames).
- `Core_quit()` clears the callback so a later core never inherits it.

`underrun` is `occupancy < 25 || a new driver underrun since the last tick ||
latch`.  The latch (`MINARCH_AUDIO_UNDERRUN_LATCH=<frames>`, default 0 = off)
exists because a single-frame pulse is enough for a core that skips one frame,
but not for one that needs several consecutive skips (Yabadabadoo's auto mode
caps consecutive skips at "Frameskip Interval").  `MINARCH_AUDIO_STATUS_LOG=1`
logs what the core is being told, once every 5 s.

## Measurements (RGSP, Golden Axe / Daytona USA, Yabadabadoo `frameskip=auto`)

Old frontend = `fixed interval (1)` fallback (render every 2nd frame).

| Configuration (GA, 75 s) | fps | rendered | underruns | est. silence |
|---|---|---|---|---|
| old frontend (fixed skip) | 41–44 | 50 % (fixed) | 1400 | 17.5 s |
| auto, RA-style instantaneous signal | 43–44 | 51 % | 1300 | 19.4 s |
| auto + latch(8) | 45–46 | 33 % | 1000 | 14.0 s |
| auto + latch(8) + interval 5 | 46–48 | 17 % | 900 | 11.6 s |
| auto + latch(8) + interval 5 + skip sprites + HUD off | **49.8** | 24 % | **800** | **7.9 s** |

So the feature does what it is for — with a sustained underrun signal the core
skips as aggressively as its budget allows and the audio starvation drops by
more than half — but it cannot manufacture samples a core cannot produce.  On
Daytona, where Yabadabadoo is CPU-bound rather than render-bound, cutting
rendering from 100 % to 17 % moves 39.6 → 40.3 fps and the underruns do not
change; the audio stays starved because the core produces ~32 k samples/s
against 44.1 k needed.

Non-skipping cores are unaffected: our Saturn core (hw-render, frameskip off)
measured 59.4 fps / 1 underrun in both builds, and a core that never registers
62 never sees a call.

## Per-frame cost of the frontend (measured, not inferred)

`MINARCH_VIDEO_PROFILE=1` logs the pieces every 5 s.  On Golden Axe with
Yabadabadoo (fixed skip 5, sprites skipped), steady state:

| section | ms/frame |
|---|---|
| whole loop | 21.1–21.5 |
| `run_frame()` | 20.7–21.1 |
|  · software present (convert+upload+draw+swap) | 1.2–1.7 |
|  · audio batch (resample + ring copy) | 0.70–0.75 |
|  · input poll (pad + power) | 0.15–0.17 |
| rest of loop (pace, telemetry, RA, notifications) | 0.39–0.45 |

So the frontend's own per-frame work is ≈ 2.3–2.6 ms.

**Pinned-state comparison** (same `retro_serialize` blob written by the
headless harness, loaded by minarch through its auto-resume slot, same
Yabadabadoo config, 600 frames / 5 s windows):

| run | per frame | notes |
|---|---|---|
| headless harness (`lrheadless`, no present/audio) | **17.2 ms** (58.0 fps, 1.24 cores) | core only |
| minarch (presents, audio ring, input) | **20.4–21.1 ms** (47–49 fps) | + present 1.2–1.4, audio 0.71, input 0.16, loop rest 0.4 |

⇒ the ~3.6 ms difference is the frontend's own work; there is no unexplained
overhead.  An earlier revision claimed ~6 ms by comparing against the harness
on "the same window" without pinning the state — **that comparison was
invalid** (the two runs were at different points of the attract sequence; the
same `video_cb` index dumped from each differs by 73 % of its pixels).
