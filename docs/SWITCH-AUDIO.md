# Switch audio (ReXGlue, libnx audout)

Status: works on the console since 0.72 (checked by the user).

## Parts
- `src/nx_audout_driver.h`: the audio driver. Copied into the SDK (`src/audio/nop/`) and wired into `CreateDriver` by `patches/sdk-edits-gh.py`.
  One output thread mixes all guest clients to stereo s16 and plays 2 audout buffers of 1024 samples (48 kHz). A frame counts as consumed when it goes into a buffer; only then the client's semaphore is released.
- `patches/audio_client_threads.inc` (inserted by `patches/sdk-edits-gh.py`): one host thread per guest audio client.
- `switch/host/switch_nx.cpp`: placement of "Audio Client" threads (0x2A, cores 1-2).
- Queue: `audio_maxqframes = 8` (`src/gh_app.h`), about 43 ms.
- Markers: `sdmc:/switch/gh/mute` (silence), menu entry "sound: on/off".

## Cause of the stutter and the fix
The SDK audio worker runs the guest callback of each client in turn on ONE thread. The callbacks mostly wait: they hand the frame to the game's mixer thread, 1.5-4 ms each.
Guardian Heroes opens 3 clients. Their callbacks together needed about 1.3 s per second, so the worker was saturated while using only 5-11 % CPU. The client with the highest index (served last) got ~510 of 940 callbacks per 5 s, and its sound had gaps.
The device side was fine (buffers returned every 16-24 ms). A bigger queue only delays the problem: the throughput was too low.

Fix: one host thread per client. The callbacks wait in parallel. In Eden: all clients deliver 940 frames per 5 s, 0 empty pops, answer time at most 12 ms (before: up to 330 ms).

## How it was found (keep for other ports)
Counters in the log every 5 s, per client: empty pops (the total underrun counter hides a late client), lowest queue depth, histogram of the time from semaphore release to the next submit; for the device: histogram of the time between returned buffers; for the worker: time per guest callback.
Eden shows the same pattern as the console, so the diagnosis can be done there.

## Side effects to know
Callbacks of different clients can run at the same time (before: one after another). No problem seen in Eden or on the console.
