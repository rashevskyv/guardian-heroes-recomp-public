# Projects catalog and honest checked / not checked list

Date of research: 2026-10-22. Clones lived in a temp folder with a short path (re-clone with `gh repo clone <repo> -- --depth 1` into a SHORT path).
"Read code" = I (or a delegated reader) opened source files. "README only" = README or docs only. "Search only" = a search snippet or listing.
I did not build or run any of these. Licences matter: GPL-3.0 repos give ideas, not files.

## Tools and runtimes

| Repo | Licence | What | Checked |
|---|---|---|---|
| hedge-dev/XenonRecomp | MIT | PPC→C++ recompiler, XenonAnalyse, XenonUtils (xex.cpp, ppc_context.h) | Read code: README, `ppc_context.h` macros, config options, `xex.cpp` named. Did NOT read `recompiler.cpp` in full |
| hedge-dev/XenosRecomp | MIT | Xenos shader ucode → HLSL → DXIL/SPIR-V | README only (by a delegated reader) |
| rexglue/rexglue-sdk (v0.10.0, c94f5eb) | BSD-3 | codegen + Xenia-derived runtime + Xenos plugin | Read code: config.cpp, pch template, builders (context, system), clock.cpp, command_processor WAIT_REG_MEM, kernel/crt, xthread names. Did NOT read the GPU backends in full |
| hedge-dev/UnleashedRecomp | GPL-3.0 | Sonic Unleashed PC; reference XenonRecomp port, 42 D3D hooks, plume | Read code: SWA.toml, memory.cpp, ppc_context.h, CMake flags; video.cpp read by a delegate |
| xenia-canary / xenia | BSD | emulator, ground truth | NOT cloned. Only what other repos cite |
| crownparkcomputing/xbox360-native-ports + CrownParkComputing/rexglue-vmx (branch rru-fixes) | n/a | ~25 ReXGlue ports; 97-commit SDK fork | Read the full `REXGLUE_FORK_CHANGES.md` and `ANDROID_AUDIT.md` via the API. Did NOT clone the fork's code |
| nate-santos/xbox360-recompilation-guide | n/a | strategy guide (Oct 2026) | Read in full; it is a secondary source, claims not re-verified |

## ReXGlue game ports

| Repo | Game | Target | Checked |
|---|---|---|---|
| ChanseyIsTheBest/sa2-nx | Sonic Adventure 2 XBLA | **Switch**, deko3d, 60 fps | Read code + docs through a delegate (Switch memory, threads, cpu-rounds, porting guide). Not built |
| Thorhax/usf4-nx | Ultra Street Fighter IV | **Switch**, NVK, 60 fps 540p | Read notes in full (918 lines) + SDK header/audio/xma via delegate |
| ChanseyIsTheBest/futari-nx, guwange-nx, peggle1-nx, bejeweled3-nx | CAVE shmups, Peggle, Bejeweled 3 (2D XBLA) | **Switch** | Read futari notes (1,574 lines) + research docs + bejeweled optimisation docs via delegate. guwange/peggle: README and listing only |
| givethesourceplox/UnleashedRecomp-NX, NaGaa95/MarathonRecomp-NX | Sonic Unleashed, Sonic 2006 | **Switch** (XenonRecomp) | Read Marathon Switch code (memory, threads, xma) and Unleashed memory/heap via delegate; Unleashed docs partly "not checked" |
| saulob/hard-corps-uprising-recompiled | Hard Corps: Uprising (= "Contra Uprising", Konami/Arc System Works XBLA, 58410A18) | PC, Linux/Deck | Read manifest, overrides.toml, patch table doc. Did NOT read the patch contents |
| birabittoh/NocturneRecomp | Castlevania: SotN XBLA | PC | Read manifest, config (rexcrt, midasm), docs (native rendering, RE), build.py |
| giangra/stuntdouble-reignition | Stuntman: Ignition | PC | Read NOTES.md, manifest, patch names. Patches 0001-0003 not read line by line |
| XDanfr/RaymanOriginsRecomp (BelmanteGu) | Rayman Origins | macOS arm64, **Android arm64**, 60 fps on Galaxy S23 | Read docs: ANDROID_PERFORMANCE.md in full, NATIVE_RENDERER, PROGRESS (delegate), toml. Runtime code not read |
| shipa-2/Xerenge | Burnout Revenge | PC | README only |
| wivi514/Dead_Rising_2_Case_Zero/West | DR2 XBLA | PC | docs/gotchas, toml via delegate (README level) |
| SpeedBreakerProject/speedbreaker | NFS Most Wanted 2005 | iOS arm64, Steam Deck | Read memory.cpp, CMake flags via delegate |
| florinp93/hells-gate-recomp, sal063/AC6_recomp, CoD3Recomp, skate3, Condemned2, SVR07, reNut, pinyon-shift (Forza Horizon) | various | PC / Android | Search only (descriptions) |
| deivid22srk/*-android, ViaXTrace/recomp-launcher | Android launchers/ports | Android | Search only |
| KeithKirenai/L4D-PC-Recomp, HelmXGaming/Skate 3, Fable2Recomp | | PC | Search only |

## What I did not find

- No port of Guardian Heroes (2011, Treasure/Sega XBLA) or of Castlevania: Harmony of Despair on GitHub (searched 2026-10-22).
- No ReXGlue port with `[rexcrt]` in sa2-nx's manifest (SA2 does not use rexcrt or skip_lr; it uses `elide_lr_stores`). Nocturne uses rexcrt. futari uses native hooks for memcpy/memset.
- GitHub search rate-limited after ~30 queries: the list of ports is partial.
- Xenia source, Ghidra analysis of any XEX, and a real XEXP patch format: not studied.
- Nothing here was executed. All performance numbers are the authors' own claims (some are PC numbers; sa2-nx says many are "user estimate").

## Prototype 1 / Prototype 2 (Radical Entertainment): searched 2026-10-22 on request
- **Prototype 1 (2009): one repo found**: `Nazatric/PrototypeRecomp` ("Prototype Android Recomp", 2 stars, updated 2026-10-16, ONE commit, "Phase 2H", work in progress). Native ARM64 Android + Vulkan target. Toolchain: **XenonRecomp** (hedge-dev, SIMDe) + its OWN hand-written runtime (`runtime/`: imports_core/fs/vd/xam, gdfx_volume, gpu_command_processor, main), clang `-O2`, 331 generated TUs, all eight codegen optimisation flags still OFF ("baseline"). Read: README, `prototype_phase2.toml`, `build_o2.sh`, `analysis/PHASE2D_BOOT_HANDSHAKE.md` (partly), file names. Did NOT read the runtime code. No renderer or fps numbers; the GPU is a command-processor stub in progress. No Switch effort; Android is the only ARM target. **The repo contains the game's own files (`default.xex`, `*.rcf`, `*.p3d`): I did not open or use them and recommend nobody copies them.**
- Boot lessons useful for any runtime: (1) `KeDelayExecutionThread(WaitMode, Alertable, PLARGE_INTEGER Interval)` has THREE args: reading the interval from r4 (the Alertable flag) made every guest sleep a no-op, and a task-queue pump spun at ~250k iterations/s (1.7 M critical-section acquisitions in 12 s); (2) the interval is in 100 ns units, negative = relative: x100 for ns, not /100 (a 100 ms sleep slept 10 us); (3) all waits need both timeout forms; (4) a truncated 28-entry switch table in the audio/ATG message loop was "the decisive fix" for boot; (5) MSVC `RtlUnwind` call sites are C++ EH, not setjmp/longjmp. Source: `analysis/PHASE2D_BOOT_HANDSHAKE.md`, `prototype_phase2.toml` header [DOC/CODE, single commit].
- **CORRECTION (user supplied the link): both games HAVE Switch ports. See the next block. My search missed them (the repos are named "prototype-switch" and "prototype2-nx", not "recomp").**

## Prototype 1 / Prototype 2 (Radical Entertainment) — searched 2026-10-22 on request
- **Prototype 1 (2009): one repo found**: `Nazatric/PrototypeRecomp` ("Prototype Android Recomp", 2 stars, updated 2026-10-16, ONE commit, "Phase 2H", work in progress). Native ARM64 Android + Vulkan target. Toolchain: **XenonRecomp** (hedge-dev; vendored under its scripts, SIMDe) + its OWN hand-written runtime (`runtime/`: imports_core/fs/vd/xam, gdfx_volume, gpu_command_processor, main), clang `-O2`, 331 generated TUs, all eight codegen optimisation flags still OFF ("baseline"). Read: README, `prototype_phase2.toml`, `build_o2.sh`, `analysis/PHASE2D_BOOT_HANDSHAKE.md` (partly), file names. Did NOT read the runtime code. No renderer or fps numbers are given; GPU is a command-processor stub in progress. No Switch effort; Android is the only ARM target. **The repo contains the game's own files (`default.xex`, `*.rcf`, `*.p3d`): I did not open or use them and recommend nobody copies them.**
- Lessons from its boot notes (useful for any runtime): (1) `KeDelayExecutionThread(WaitMode, Alertable, PLARGE_INTEGER Interval)` has THREE args: reading the interval from r4 (the Alertable flag) made every guest sleep a no-op and a task-queue pump spun at ~250k iterations/s (1.7 M critical-section acquisitions in 12 s), starving other threads; (2) the interval is in 100 ns units, negative = relative: ×100 for ns, not ÷100 (a 100 ms sleep slept 10 µs); (3) all waits need both timeout forms (absolute positive / relative negative); (4) a truncated 28-entry switch table in the audio/ATG message loop was "the decisive fix" for boot; (5) MSVC `RtlUnwind` call sites are C++ EH, not setjmp/longjmp (no setjmp redirects needed). Source: `analysis/PHASE2D_BOOT_HANDSHAKE.md`, `prototype_phase2.toml` header [DOC/CODE, single commit].
- **CORRECTION (user supplied the link): both games HAVE Switch ports. See the next block. My search missed them (the repos are named "prototype-switch" and "prototype2-nx", not "recomp").**

### Prototype 1 and 2 on Nintendo Switch (found via a link from the user, 2026-10-22)
- **fungalspores/prototype-switch** (GPL-3.0, 26 stars, 1 commit, updated 2026-10-22): Prototype 1 (title 4156084E, Russian edition tested) AND Prototype 2, ReXGlue v0.10.0 + masseffect-nx Horizon SDK + native Vulkan renderer on NVK (no Xenos emulation). Shared `rex-start` settings screen, HOME NSP forwarders. Reported speed (Switch OLED, CPU 1785 / GPU 768-1152 / RAM 2333): Prototype 1 menus 30-60 fps, gameplay 25-30 fps at 1280x720; Prototype 2 city 25-30 fps at 960x544 + FSR 1. Prototype 2 uses all four cores. Cloned in x360\prototype-switch. Read: README, overrides.toml, prototype.toml, proto_speed.cpp, header of tools/prepare_proto_codegen.py, build outline. Not read: renderer code, patches.
- **penettrator/prototype2-nx** (the Prototype 2 port, 0.2.0, GPL-3.0, 1 commit, Boosty-supported): ReXGlue + NVK; PGO for guest and app code, skinning input scan reuse, 800x448 + FSR 1, batched occlusion queries, core 3 via a forwarder NSP that permits cores 0-3. Cloned in x360\prototype2-nx. Read: README.en, CHANGELOG, FOURTH_CORE, perf_locals.toml, toml settings. Not read: sources.
- Third-party credits there: NebadasSwifty/masseffect-nx (SDK with Horizon layer, patch `rexglue-sdk-masseffect-nx.patch`), StevensND/nfsmw-nx (origin of the Horizon layer), R-drg/project8-sw (renderer base), danfromtico/mesa-switch (NVK) + patch. Built on an Android phone (PRoot Alpine, devkitA64) per the README.
- Hacks worth taking (details in performance.md §8g): per-address-stripe lock for mtmsrd regions; XAPI fibers replaced through `[rexcrt]` (ConvertThreadToFiber..SwitchToFiber); `proto_deadlock_fix` (main thread releases scene critical sections while it waits for the render thread to drain the command ring; render thread drains in place); save-size bug (`XFile::Write/SetLength` returned the size cached at open); console signature stubs; configurable back-buffer size by patching the hard-coded 1280x720 in the guest; no temperature-sensor session (clashed with Horizon OC); direct calls + guest LTO + leaf locals + ARM VMX build flags; PGO from an on-console instrumented run.
