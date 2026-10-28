# deko3d renderer and field lessons (Castlevania HD, Hard Corps: Uprising, Guardian Heroes)

Field results from three real ports on a real Switch, October 2026.
Tags: [HW] measured on the console, [EDEN] seen in the Eden emulator, [LINUX] seen on the Linux debug host, [GUESS] inference.
Other Switch ports (sa2-nx, usf4-nx ...) are GPL-3: use their ideas, never their files.

## 1. Why deko3d, not the SDK's Vulkan path (NVK)
- [HW] Castlevania HD on the SDK's Xenos/Vulkan backend through NVK ran at 2.1 fps: a constant 478 ms per frame, guest main thread at 100% of a core, GPU "idle".
- [LINUX] Cause (found with gdb): the command processor waits for the previous frame's GPU work in `BeginSubmission`. The backend emulates the 360's EDRAM:
  about 111 render-target transfer passes, 12 resolves, 4 clears and 13 MB of texture reloads per frame. lavapipe needs 500 ms for it, NVK on the Tegra X1 478 ms.
- Excluded by numbers: sleeps (a 1 ms sleep takes 1.00 ms), the clock, memory cache attributes (write 10 GB/s, read 4 GB/s, 135 ns dependent load), MMIO,
  the address fold, thread placement, WAIT_REG_MEM polls, the file system (only the 50 s boot).
- [EDEN] NVK cannot render in Eden at all (it needs the real nvdrv). deko3d runs in Eden AND on the console, so deko3d gives a picture in the emulator before any console run.
- [HW] Guardian Heroes on deko3d: stable 60.0 fps, 16.7 ms frames, 120-136 draws per frame, CPU 1785 MHz, GPU 768 MHz.
- Rule: new 360 ports go to deko3d from day one. Eden with a picture is the gate before the console.

## 2. Xenos to deko3d translation facts that cost days
| Symptom | Cause and fix |
|---|---|
| Diagonal half of a full-screen quad missing | Xenos RECTANGLE_LIST (prim type 8) and QUAD_LIST must be expanded. Eden ignores the hardware fill-rectangle mode: use a geometry shader (4th vertex = v1 + v2 - v0). Keep the hardware mode behind a cvar. |
| Depth test does the wrong thing (LEQUAL acted as EQUAL) | Compare op off by one: Xenos `kNever` = 0, `DkCompareOp_Never` = 1. Return `DkCompareOp(f + 1)`. |
| Stage background missing, only the HUD draws | A resolve must clear depth when `rb_copy_control.depth_clear_enable` is set. Depth resolves go into SAMPLEABLE depth images (k_24_8) for shadow-map textures. |
| Picture brighter and flatter than the original | The present pass must apply the game's display gamma ramp. |
| Stale texture (old logo behind a new frame) | Re-upload textures when their guest pages change: dirty-page serials, mark file reads. Do not re-upload a resolve destination that was bound in the same swap that resolved it. |
| Text missing letters or texcoords wrong | xenos2glsl needs per-component write tracking of the vertex input; 1D fetches must sample 2D images. |
| Mirrored or repeated texture, noise at edges | Sampler address mode. Map Xenos ClampMode 0 wrap, 1 mirror, 2 clamp-to-edge, 3 mirror-once, 4 clamp-half-border, 5 mirror-once-half, 6 clamp-to-border to the deko3d wrap modes. Min/mag filters come from the fetch constants: the GAME sets the filter, not the renderer. The edge-noise cause is [GUESS]. |
| Image pool grows forever | The resize ping-pong between a resolve's aligned height and a texture bind's exact height created a new image each time. Key images by exact size. |
| Sprites drawn as square tiles with the wrong texture | Open fault in Hard Corps (atlas decode / UV / array layer). Compare against the Vulkan reference frame by frame. |
| k_16 / k_16_16 textures | Support them in the upload path. |

Debug aids worth building at once:
- a texture dumper (a marker file such as `sdmc:/switch/<game>/dk_dump_tex` writes every distinct decoded texture as a PPM),
- per-draw-type logs,
- a dump of missing shaders.

KEEP ALL MARKERS OFF BY DEFAULT. A leftover dump mode once drew a mirrored font atlas over the whole screen and cost 8-9 fps on the user's console.

Also build early: an on-screen strip drawn in the present pass with a small bitmap font, `<game> <version> NN.N fps`. Keep it on while testing.

Button glyphs: the game draws Xbox icons, but our input is positional (Xbox A bottom = Switch B). Patch the UI atlas cells in the texture upload (swap A and B, X and Y; found by hue).
Guardian Heroes: a k_4_4_4_4 2048x2225 atlas with 24x26 cells. Later step: redraw the icons in Switch style, with an option `button_glyphs`.

## 3. Switch layer lessons
- Memory (title takeover pool about 3285 MB): take the guest pool (1088 MB) as the FIRST allocation, carved from the top of libnx's heap and OUTSIDE newlib malloc.
  Malloc misbehaves with a 1 GB chunk inside it. Symptom: "no RAM for the 0x0 block", then NVK "out of host memory" and std::bad_alloc. Keep a malloc self-test in the log.
- Horizon priorities: only 0x3B is time-sliced (10 ms). Spinning guest workers at 0x3B starve the main thread. Main 0x2C, GPU ring 0x2C, audio 0x2B. Never use core 3. `svcSleepThread(0)` yields only to equal priority.
- Waits and I/O:
  - multi-object waits that polled became a condition variable;
  - the WAIT_REG_MEM blind Sleep was capped at 1 ms;
  - do not scan a directory after a missing-file stat (it cost 2 s per missing optional file);
  - inline the host-offset and dirty-mark helpers (GCC called them 650 times per 256 KB of generated code).
- Codegen flags (`skip_msr`, `ctr_as_local`, `xer_as_local`, `cr_as_local`, `reserved_as_local` ...) must sit in the manifest's `[entrypoint]` table. At the top level they are ignored.
  Codegen reuses old output when its stamp matches: use `--ignore-stamp`. In Guardian Heroes only cr/ctr/xer/reserved as local were safe;
  `non_argument` and `non_volatile` caused heap corruption. Measure, then keep what works.
- Exit: `__nx_applet_exit_mode = 1`, then `__libnx_exit`. Version: NACP `0.<commit count>.<sha7>`, also in the log and on screen.
- Sound (solved in Guardian Heroes 0.72): a real Switch driver on libnx audout (48 kHz stereo s16, one output thread mixing all guest clients, 2 buffers of 1024 samples, semaphore released when a frame goes into a buffer; `src/nx_audout_driver.h` in gh-recomp). The SDK audio worker runs the guest callback of every client in turn on ONE thread; the callbacks mostly wait on the game's mixer thread (1.5-4 ms each), so 3 clients saturate it and the last client starves (stutter with a healthy device). Fix: one host thread per client (`patches/audio_client_threads.inc`, `patches/sdk-edits-gh.py`), placed at priority 0x2A on cores 1-2. A bigger queue does not help (throughput, not jitter). Diagnose with per-client counters: empty pops, queue minimum, release-to-submit histogram, time per callback; the total underrun counter hides one late client. Eden shows the same pattern as the console. Details: gh-recomp `docs/SWITCH-AUDIO.md`.

## 4. Workflow that worked
- Gates in order: Linux with the null GPU, then an Eden null-GPU run (swaps grow, clean exit; marker file `nogpu`), then Eden WITH a picture (deko3d), then the console.
- Eden: one portable folder per session (never two sessions in the same `user\sdmc`), sound muted (`audio_muted=true`), scripted key presses through a `pad.txt`, window captures with PrintWindow, so no window stays on the user's desktop.
- Console push: FTP (Kefir Hub, port 5000). Upload under a temp name, rename, read back and compare SHA-1. The Hub's FTP is down while a game runs and resets connections when the Hub restarts: retry.
  A Windows MTP copy through the shell silently wrote nothing. Never push a debug build to the file name the user plays.
- Evidence first: when a build is slow or wrong, measure (frame ms, a per-thread CPU line, a PC sampler, a wait log) before changing code. One change per test, with the version.

## 5. Legal and release practice
- A Switch NRO and a Linux binary contain the RECOMPILED game code (a derived work). Safer: publish source and build scripts and let users build from their own files.
  If binaries are released anyway, say so in the release notes and keep all game data out.
- Game data goes only into PRIVATE repositories, as release assets. Split large files into 300 MB volumes (one 1.9 GB upload timed out with HTTP 408).
  Never change a repository's visibility: one public flip exposed a data release for minutes.
- Public repository: a fresh single-commit tree from an export script that excludes internal notes, screenshots of the game, local paths and LAN IPs, and that audits for big blobs, images and game files.
- Licences: ReXGlue is BSD-3. sa2-nx and the Prototype ports are GPL-3 (ideas only). The Hard Corps PC port is GPL-2, so a fork stays GPL-2.
