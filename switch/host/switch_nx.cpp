// Nintendo Switch (libnx) start-up for the gh port: the log on the SD card, the clocks, the exit
// path and the crash report. Everything lives in sdmc:/switch/gh/. Adapted from the N64 port's
// host/switch/switch_nx.cpp (rashevskyv/CBFD-Recompiled, branch switch).
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

#include "gh_version.h"  // written by switch/build.sh: GH_VERSION, GH_BUILD_UTC

// The version the deko3d backend prints on screen (src/gpu/deko3d_backend.cpp, overlay strip).
extern "C" const char* rex_nx_version(void) { return GH_VERSION; }

extern "C" {
// libnx's own exit (init.c): __appExit (userAppExit, then the services), then the function from
// envGetExitFuncPtr. No static destructors.
[[noreturn]] void __libnx_exit(int rc);
// The NRO's load address: _start, the first instruction (ELF address 0). Not __start__, which is an
// absolute 0 that is not relocated (the Xbox port). Crash addresses are given relative to it.
void _start(void);
// __libnx_exception_handler runs on this stack when a thread faults.
alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
// Report crashes even when a debugger is attached.
u32 __nx_exception_ignoredebug = 1;
// nvdrv's transfer memory: libnx gives 8 MB; the N64 port needed 32 MB on hardware.
u32 __nx_nv_transfermem_size = 0x2000000;
// membench_nx.cpp
void rex_nx_membench(void);
// memory_nx.cpp: the RAM behind the guest memory, taken first; the physical (0xA0000000) block.
void rex_nx_reserve_pool(void);
const uint8_t* rex_nx_physical_base(void);
// libnx's heap bounds (newlib.c).
extern char* fake_heap_start;
extern char* fake_heap_end;
// Counted by the SDK's command processor (sdk-edits.py, WAIT_REG_MEM), printed every 2 s here.
volatile uint64_t rex_nx_wait_polls = 0, rex_nx_wait_sleep_ms = 0, rex_nx_wait_yields = 0, rex_nx_wait_slept_ticks = 0;
// memory_nx.cpp: dirty-page flushes asked by the command processor.
extern volatile uint64_t rex_nx_flush_calls, rex_nx_flush_pages;
}

namespace {

const char* const kDataDir = "sdmc:/switch/gh";

Thread log_flush_thread;
volatile bool log_flush_running = false;

// CPU 1785 MHz (the highest), GPU 768 MHz (docked's rate), memory 1600 MHz, as the Xbox ports do.
// The old rates come back on exit.
struct Clock {
    const char* name;
    PcvModuleId module;
    u32 wanted;
    ClkrstSession session;
    bool open;
    u32 old;
    volatile u32 set;
};
Clock clocks[] = {
    {"CPU", PcvModuleId_CpuBus, 1785000000},
    {"GPU", PcvModuleId_GPU, 768000000},
    {"memory", PcvModuleId_EMC, 1600000000},
};
bool clkrst_up = false;

// Is another process serving this name? (sys-clk's own check.)
bool service_running(const char* name) {
    Handle handle;
    const Result rc = smRegisterService(&handle, smEncodeName(name), false, 1);
    if (R_SUCCEEDED(rc)) {
        smUnregisterService(smEncodeName(name));
        svcCloseHandle(handle);
        return false;
    }
    return rc == MAKERESULT(21, 4);
}

void raise_clocks() {
    // sys-clk and Horizon OC set the clocks themselves; fighting them froze consoles (NFSU2's port).
    if (service_running("sys:clk") || service_running("hoc:clk")) {
        std::printf("[clock] sys-clk or Horizon OC is running: the clocks are left to it\n");
        return;
    }
    if (!hosversionAtLeast(8, 0, 0) || R_FAILED(clkrstInitialize())) {
        std::printf("[clock] no clkrst: the clocks are unchanged\n");
        return;
    }
    clkrst_up = true;
    for (Clock& clock : clocks) {
        if (R_FAILED(clkrstOpenSession(&clock.session, clock.module, 3))) {
            std::printf("[clock] no clkrst session for the %s: unchanged\n", clock.name);
            continue;
        }
        clock.open = true;
        clkrstGetClockRate(&clock.session, &clock.old);
        // Never lower a rate (docked memory, or someone else's overclock).
        const Result rc = clock.old < clock.wanted ? clkrstSetClockRate(&clock.session, clock.wanted) : 0;
        u32 now = 0;
        clkrstGetClockRate(&clock.session, &now);
        clock.set = now;
        std::printf("[clock] %s %u -> %u MHz%s\n", clock.name, clock.old / 1000000, now / 1000000,
                    R_FAILED(rc) ? " (refused)" : "");
    }
}

// HOME, docking and sleep put the system's clocks back: set ours again.
void keep_clocks() {
    for (Clock& clock : clocks) {
        u32 now = 0;
        if (clock.set == 0 || R_FAILED(clkrstGetClockRate(&clock.session, &now)) || now >= clock.set) {
            continue;
        }
        clkrstSetClockRate(&clock.session, clock.set);
        std::printf("[clock] %s was put back to %u MHz: set to %u MHz again\n", clock.name, now / 1000000,
                    clock.set / 1000000);
    }
}

void restore_clocks() {
    if (!clkrst_up) {
        return;
    }
    for (Clock& clock : clocks) {
        if (clock.open) {
            if (clock.old != 0 && clock.set != clock.old) {
                clkrstSetClockRate(&clock.session, clock.old);
            }
            clkrstCloseSession(&clock.session);
        }
    }
    clkrstExit();
    clkrst_up = false;
}

// libnx's exit ends here instead of returning to hbl, with the services closed. Returning to hbl
// unmapped the NRO while threads still ran (the probe's exit crash, 2026-10-21).
[[noreturn]] void end_process(int) {
    svcExitProcess();
}

// Per-thread CPU time (svcGetInfo ThreadTickCount) for the 2-s profile line: which thread burns the
// time, the guest's, the GPU command processor's or another. Threads register in the create wrap
// (libnx's pthread_t is its Thread*), names come from the SDK's pthread_setname_np.
struct NxThread {
    Thread* thread;  // null = free
    Handle handle;
    char name[40];
    u64 last_ticks;
    bool boosted;
};
// gh: a game thread (unnamed "XThread...", time-sliced at 0x3B) that keeps 20 percent of a core is moved above the other game
// threads (0x2C, still below the GPU 0x28, vsync 0x27, audio worker 0x2A and audio output 0x29): the game's audio
// engine thread starved behind its 0x3B peers and the sound crackled (Prototype 2 had the same lesson).
bool nx_auto_boost = false;
NxThread nx_threads[64];
Mutex nx_threads_lock;

void register_thread(Thread* thread, const char* name) {
    mutexLock(&nx_threads_lock);
    for (NxThread& t : nx_threads) {
        if (t.thread == nullptr) {
            t.thread = thread;
            t.handle = thread->handle;
            std::snprintf(t.name, sizeof(t.name), "%s", name);
            t.last_ticks = 0;
            t.boosted = false;
            break;
        }
    }
    mutexUnlock(&nx_threads_lock);
}

// One line per 2 s: percent of one core per thread over the period (threads under 1% left out).
void print_cpu_profile(u64 period_ticks) {
    char line[1024];
    int n = std::snprintf(line, sizeof(line), "[nx] cpu:");
    unsigned threads = 0, unreadable = 0;
    Result last_rc = 0;
    mutexLock(&nx_threads_lock);
    for (NxThread& t : nx_threads) {
        if (t.thread == nullptr) continue;
        threads++;
        u64 ticks = 0;
        const Result rc = svcGetInfo(&ticks, InfoType_ThreadTickCount, t.handle, UINT64_MAX);
        if (R_FAILED(rc)) {
            // An exited thread (closed handle) or an emulator without this InfoType (Eden).
            unreadable++;
            last_rc = rc;
            continue;
        }
        const u64 delta = t.last_ticks ? ticks - t.last_ticks : 0;
        t.last_ticks = ticks;
        const unsigned pct = (unsigned)(delta * 100 / period_ticks);
        if (nx_auto_boost && !t.boosted && pct >= 20 && std::strncmp(t.name, "XThread", 7) == 0) {
            svcSetThreadPriority(t.handle, 0x2C);
            t.boosted = true;
            std::printf("[nx] thread \"%s\" kept %u%% of a core at 0x3B: priority 0x2C now\n", t.name, pct);
        }
        if (pct >= 1 && n < (int)sizeof(line) - 48) {
            // Horizon time-slices only priority 0x3B (sa2-nx's notes): the priority matters.
            s32 prio = 0;
            svcGetThreadPriority(&prio, t.handle);
            n += std::snprintf(line + n, sizeof(line) - n, " %s %u%% p%02X |", t.name, pct, prio);
        }
    }
    mutexUnlock(&nx_threads_lock);
    if (unreadable) {
        n += std::snprintf(line + n, sizeof(line) - n, " %u of %u threads unreadable (rc 0x%X)", unreadable,
                           threads, last_rc);
    }
    std::printf("%s\n", line);
}


// PC sampler: 100 times a second the busiest threads are paused for a few microseconds and their
// program counter read (svcSetThreadActivity + svcGetThreadContext3). Counts per 256-byte bucket
// of the NRO; the 2-s line prints the top buckets, addr2line on gh.elf names the functions.
struct SampledThread {
    const char* prefix;  // thread name prefix to sample
    int ordinal;         // the n-th thread with that prefix (the game's XThreads)
    char name[48];       // the matched thread's name, for the line
    struct Bucket {
        u64 key;  // (pc - _start) >> 8, 0 = free
        u32 count;
        u64 last_pc;  // the latest exact PC seen in the bucket (svc stubs are 16 bytes apart)
    } buckets[512];
    u32 samples, outside, failed;
    Result last_rc;
};
// The main thread, the command processor, the audio worker and the game's first six other threads
// (named "XThread..." by the SDK): a loader thread that goes silent shows where it sits.
SampledThread sampled[9] = {{"Main XThread", 0}, {"GPU Commands", 0}, {"Audio Worker", 0}, {"XThread", 0},
                            {"XThread", 1},      {"XThread", 2},      {"XThread", 3},      {"XThread", 4},
                            {"XThread", 5}};
Thread sampler_thread;
volatile bool sampler_running = false;

void sample_pc(SampledThread& st, u64 pc) {
    const u64 base = reinterpret_cast<u64>(&_start);
    st.samples++;
    if (pc < base || pc - base >= 0x10000000ull) {
        st.outside++;
        return;
    }
    const u64 key = ((pc - base) >> 8) + 1;
    for (u32 i = (u32)(key * 2654435761u) % 512, n = 0; n < 512; i = (i + 1) % 512, n++) {
        if (st.buckets[i].key == key) {
            st.buckets[i].count++;
            st.buckets[i].last_pc = pc - base;
            return;
        }
        if (st.buckets[i].key == 0) {
            st.buckets[i].key = key;
            st.buckets[i].count = 1;
            st.buckets[i].last_pc = pc - base;
            return;
        }
    }
}

void sampler_loop(void*) {
    while (sampler_running) {
        svcSleepThread(10'000'000LL);
        for (SampledThread& st : sampled) {
            Handle handle = INVALID_HANDLE;
            mutexLock(&nx_threads_lock);
            int seen = 0;
            for (const NxThread& t : nx_threads) {
                if (t.thread != nullptr && std::strncmp(t.name, st.prefix, std::strlen(st.prefix)) == 0) {
                    if (seen++ < st.ordinal) continue;
                    handle = t.handle;
                    std::strncpy(st.name, t.name, sizeof(st.name) - 1);
                    break;
                }
            }
            mutexUnlock(&nx_threads_lock);
            if (handle == INVALID_HANDLE) continue;
            if (R_FAILED(svcSetThreadActivity(handle, ThreadActivity_Paused))) {
                st.failed++;
                continue;
            }
            ThreadContext ctx;
            const Result rc = svcGetThreadContext3(&ctx, handle);
            svcSetThreadActivity(handle, ThreadActivity_Runnable);
            if (R_FAILED(rc)) {
                st.failed++;
                st.last_rc = rc;
                continue;
            }
            sample_pc(st, ctx.pc.x);
        }
    }
}

// The top buckets of each sampled thread since the last report, as ELF offsets.
void print_pc_profile() {
    for (SampledThread& st : sampled) {
        if (st.samples == 0 && st.failed == 0) continue;
        char line[512];
        int n = std::snprintf(line, sizeof(line), "[nx] pc %s (%u samples):", st.name[0] ? st.name : st.prefix, st.samples);
        for (int top = 0; top < 6; top++) {
            SampledThread::Bucket* best = nullptr;
            for (SampledThread::Bucket& b : st.buckets) {
                if (b.key != 0 && (best == nullptr || b.count > best->count)) best = &b;
            }
            if (best == nullptr || best->count == 0) break;
            n += std::snprintf(line + n, sizeof(line) - n, top == 0 ? " +0x%llX %u%% (pc +0x%llX) |" : " +0x%llX %u%% |",
                               (unsigned long long)((best->key - 1) << 8), best->count * 100 / st.samples,
                               (unsigned long long)best->last_pc);
            best->key = 0;
            best->count = 0;
        }
        if (st.outside) n += std::snprintf(line + n, sizeof(line) - n, " outside %u%%", st.outside * 100 / st.samples);
        if (st.failed) n += std::snprintf(line + n, sizeof(line) - n, " (%u failed, rc 0x%X)", st.failed, st.last_rc);
        std::printf("%s\n", line);
        std::memset(st.buckets, 0, sizeof(st.buckets));
        st.samples = st.outside = st.failed = 0;
    }
}

// The log is buffered and flushed twice a second; fsync commits it to the card (after a hang and
// a hard reboot, a log that was only flushed ended in garbage). Every 2 s a line gives the time
// and the memory, so a hang shows when it happened.
void log_flush_loop(void*) {
    const u64 start = armGetSystemTick();
    u64 last_profile = start;
    for (unsigned tick = 1; log_flush_running; tick++) {
        svcSleepThread(500'000'000LL);
        if (tick % 4 == 0) {
            u64 used = 0;
            svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
            const u64 now = armGetSystemTick();
            std::printf("[nx] %llu s, %llu MB used\n",
                        (unsigned long long)(armTicksToNs(now - start) / 1'000'000'000ULL),
                        (unsigned long long)(used >> 20));
            print_cpu_profile(now - last_profile);
            print_pc_profile();
            {
                // The GPU register window 0x7FC80000 folds onto physical 0xC80000: a guest store that
                // codegen did not recognise as MMIO lands there instead of in the command processor.
                // CP_RB_RPTR is register 0x1C4, CP_RB_WPTR 0x1C5.
                const uint8_t* phys = rex_nx_physical_base();
                if (phys != nullptr) {
                    const u32 rptr = __builtin_bswap32(*reinterpret_cast<const volatile u32*>(phys + 0xC80710));
                    const u32 wptr = __builtin_bswap32(*reinterpret_cast<const volatile u32*>(phys + 0xC80714));
                    std::printf("[nx] mmio probe: RAM behind CP_RB_RPTR 0x%08X, CP_RB_WPTR 0x%08X\n", rptr, wptr);
                }
            }
            {
                // The command processor's WAIT_REG_MEM polls since the last line (command_processor.cpp).
                static u64 last_polls = 0, last_sleep = 0, last_yields = 0, last_slept = 0;
                const u64 polls = rex_nx_wait_polls, sleep = rex_nx_wait_sleep_ms, yields = rex_nx_wait_yields;
                const u64 slept = rex_nx_wait_slept_ticks;
                if (polls != last_polls || yields != last_yields) {
                    std::printf("[nx] gpu waits: %llu sleeping polls (%llu ms asked, %llu ms really slept), %llu yield polls\n",
                                (unsigned long long)(polls - last_polls), (unsigned long long)(sleep - last_sleep),
                                (unsigned long long)(armTicksToNs(slept - last_slept) / 1000000ULL),
                                (unsigned long long)(yields - last_yields));
                }
                last_polls = polls;
                last_sleep = sleep;
                last_yields = yields;
                last_slept = slept;
                static u64 last_fc = 0, last_fp = 0;
                const u64 fc = rex_nx_flush_calls, fp = rex_nx_flush_pages;
                if (fc != last_fc) {
                    std::printf("[nx] dirty pages: %llu flushes, %llu pages re-marked for upload\n",
                                (unsigned long long)(fc - last_fc), (unsigned long long)(fp - last_fp));
                }
                last_fc = fc;
                last_fp = fp;
            }
            last_profile = now;
            keep_clocks();
        }
        std::fflush(stdout);
        fsync(fileno(stdout));
    }
}

void open_log() {
    mkdir("sdmc:/switch", 0777);
    mkdir(kDataDir, 0777);
    char path[256];
    std::snprintf(path, sizeof(path), "%s/gh_log.txt", kDataDir);
    if (std::freopen(path, "w", stdout) == nullptr) {
        return;
    }
    // Buffered on the card (each flush costs); line-buffered in no-GPU runs (Eden, a host file), so
    // an abort keeps every line.
    const bool line_buffered = access("sdmc:/switch/gh/nogpu", F_OK) == 0;
    std::setvbuf(stdout, nullptr, line_buffered ? _IOLBF : _IOFBF, 64 * 1024);
    // stderr goes to the same file, through stdout's buffer, so the lines stay in order.
    std::fclose(stderr);
    stderr = stdout;
    log_flush_running = true;
    // Above the game's threads, on core 0 (never core 3). It sleeps between flushes.
    if (R_SUCCEEDED(threadCreate(&log_flush_thread, log_flush_loop, nullptr, nullptr, 0x4000, 0x20, 0))) {
        threadStart(&log_flush_thread);
    }
    sampler_running = true;
    if (R_SUCCEEDED(threadCreate(&sampler_thread, sampler_loop, nullptr, nullptr, 0x4000, 0x1C, 0))) {
        threadStart(&sampler_thread);
    }
}

}  // namespace

// Thread placement by name (see place_thread below); sdmc:/switch/gh/nopin turns it off.
bool pinning = true;

// An uncaught C++ exception ends in std::terminate -> abort -> libnx's exit, with nothing in the log.
// Name it first.
[[noreturn]] void terminate_handler() {
    std::printf("[nx] std::terminate called");
    if (std::exception_ptr ex = std::current_exception()) {
        try {
            std::rethrow_exception(ex);
        } catch (const std::exception& e) {
            std::printf(": uncaught exception: %s", e.what());
        } catch (...) {
            std::printf(": uncaught exception of an unknown type");
        }
    }
    std::printf("\n");
    std::fflush(stdout);
    fsync(fileno(stdout));
    std::abort();
}

extern "C" void userAppInit(void) {
    open_log();
    std::set_terminate(terminate_handler);
    std::printf("[nx] Guardian Heroes, version " GH_VERSION ", built " GH_BUILD_UTC "\n");
    AppletType type = appletGetAppletType();
    const bool application = type == AppletType_Application || type == AppletType_SystemApplication;
    std::printf("[nx] launch: %s (applet type %d)\n",
                application ? "application (title takeover)" : "APPLET: not the full memory, start with title "
                                                               "takeover (hold R while starting a game)",
                (int)type);
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    // "used" counts libnx's heap, which takes almost all of the pool at start (fake_heap_*).
    std::printf("[nx] memory: pool %llu MB, %llu MB used at start (the heap itself: %llu MB)\n",
                (unsigned long long)(total >> 20), (unsigned long long)(used >> 20),
                (unsigned long long)((fake_heap_end - fake_heap_start) >> 20));
    // The RAM behind the guest memory: the first allocation of the program (memory_nx.cpp).
    rex_nx_reserve_pool();
    appletSetAutoSleepDisabled(true);
    raise_clocks();
    envSetExitFuncPtr(end_process);
    register_thread(threadGetSelf(), "main");
    pinning = access("sdmc:/switch/gh/nopin", F_OK) != 0;
    nx_auto_boost = pinning && access("sdmc:/switch/gh/noboost", F_OK) != 0;  // gh: noboost turns off only the thread boost
    std::printf("[nx] thread placement %s\n", pinning ? "on (sdmc:/switch/gh/nopin turns it off)" : "off (nopin)");
    // Is the guest arena's memory as fast as malloc memory? (membench_nx.cpp, ~1 s, once.)
    rex_nx_membench();
}

// Cores and priorities by thread name (Horizon: lower number = more urgent; only 0x3B is time-sliced,
// in 10 ms slices; svcSleepThread(0) yields only to the same priority). The guest's main thread spins
// on the GPU (the game's own 32x db16cyc loop), so it gets core 0 alone; the command processor polls
// the guest's values in 1 ms sleeps and must preempt the spinner at once, so it sits above every
// guest thread on core 1; the vsync worker and audio go to core 2. sdmc:/switch/gh/nopin turns this
// off (A/B). ponytail: a fixed table; revisit with the cpu line once more guest threads matter.
struct Placement {
    const char* prefix;
    u32 priority;
    int core;
    u64 mask;
};
const Placement placements[] = {
    {"Main XThread", 0x3B, 0, 0x1},
    {"GPU Commands", 0x28, 1, 0x2},
    {"GPU VSync", 0x27, 2, 0x4},
    {"Audio Worker", 0x2A, 2, 0x4},
    {"XMA", 0x2B, 2, 0x4},
    {"Audio Client", 0x2A, 1, 0x6},  // one per guest audio client: mostly waiting on the game's mixer thread
    {"XThread", 0x3B, 1, 0x6},  // the game's other threads, time-sliced on cores 1-2
};

void place_thread(Handle handle, const char* name) {
    if (!pinning) return;
    for (const Placement& p : placements) {
        if (std::strncmp(name, p.prefix, std::strlen(p.prefix)) == 0) {
            svcSetThreadPriority(handle, p.priority);
            svcSetThreadCoreMask(handle, p.core, p.mask);
            std::printf("[nx] thread \"%s\": priority 0x%X, core %d (mask 0x%llX)\n", name, p.priority, p.core,
                        (unsigned long long)p.mask);
            return;
        }
    }
}

// The SDK names its threads through pthread_setname_np (switch_compat.c sends it here).
extern "C" void rex_nx_thread_name(void* pthread, const char* name) {
    Handle handle = INVALID_HANDLE;
    mutexLock(&nx_threads_lock);
    for (NxThread& t : nx_threads) {
        if (t.thread == static_cast<Thread*>(pthread)) {
            std::snprintf(t.name, sizeof(t.name), "%s", name);
            handle = t.handle;
            break;
        }
    }
    mutexUnlock(&nx_threads_lock);
    if (handle != INVALID_HANDLE) place_thread(handle, name);
}

extern "C" void userAppExit(void) {
    sampler_running = false;
    threadWaitForExit(&sampler_thread);
    threadClose(&sampler_thread);
    log_flush_running = false;
    threadWaitForExit(&log_flush_thread);
    threadClose(&log_flush_thread);
    restore_clocks();
    std::fflush(stdout);
    fsync(fileno(stdout));
}

// libnx gives a thread made without a stack size only 128 KB (every std::thread). The linker
// sends pthread_create's call here (CMakeLists.txt, --wrap) to raise it to 2 MB.
constexpr size_t min_thread_stack = 2 * 1024 * 1024;

extern "C" int __real___syscall_thread_create(void** thread, void* (*func)(void*), void* arg, void* stack_addr,
                                              size_t stack_size);

// Every new thread starts on its creator's core (0) in libnx: deal them over cores 0-2 (never 3, the
// system's), as the Xbox port's xbox_nx_spread_thread does. ponytail: round robin; pin the GPU
// command processor and the guest's main thread to their own cores once the profile says where
// the time goes.
struct ThreadStart {
    void* (*func)(void*);
    void* arg;
};

static void* spread_and_run(void* p) {
    ThreadStart start = *static_cast<ThreadStart*>(p);
    delete static_cast<ThreadStart*>(p);
    // Named (and placed) by the creator before this ran? Then keep that placement; otherwise deal
    // the thread over cores 0-2 round robin.
    char name[40] = "thread";
    Thread* self = threadGetSelf();
    mutexLock(&nx_threads_lock);
    for (const NxThread& t : nx_threads) {
        if (t.thread == self) {
            std::snprintf(name, sizeof(name), "%s", t.name);
            break;
        }
    }
    mutexUnlock(&nx_threads_lock);
    if (std::strcmp(name, "thread") != 0 && pinning) {
        place_thread(CUR_THREAD_HANDLE, name);
    } else {
        static std::atomic<int> next{1};
        const int core = next.fetch_add(1, std::memory_order_relaxed) % 3;
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 0x7);
    }
    return start.func(start.arg);
}

extern "C" int __wrap___syscall_thread_create(void** thread, void* (*func)(void*), void* arg, void* stack_addr,
                                              size_t stack_size) {
    if (stack_addr == nullptr && stack_size < min_thread_stack) {
        stack_size = min_thread_stack;
    }
    const int rc = __real___syscall_thread_create(thread, spread_and_run, new ThreadStart{func, arg},
                                                  stack_addr, stack_size);
    if (rc == 0 && thread != nullptr && *thread != nullptr) {
        // libnx's pthread_t is its Thread*; the SDK names it right after this call.
        register_thread(static_cast<Thread*>(*thread), "thread");
    }
    return rc;
}

extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(&_start);
    std::fprintf(stdout,
                 "[nx] crash: exception 0x%X, pc 0x%llX (ELF +0x%llX), lr 0x%llX (ELF +0x%llX), far 0x%llX, "
                 "sp 0x%llX\n",
                 ctx->error_desc, (unsigned long long)ctx->pc.x, (unsigned long long)(ctx->pc.x - base),
                 (unsigned long long)ctx->lr.x, (unsigned long long)(ctx->lr.x - base),
                 (unsigned long long)ctx->far.x, (unsigned long long)ctx->sp.x);
    for (int i = 0; i < 29; i++) {
        std::fprintf(stdout, "  x%-2d 0x%016llX%s", i, (unsigned long long)ctx->cpu_gprs[i].x,
                     (i % 4 == 3) ? "\n" : "");
    }
    std::fprintf(stdout, "\n  fp 0x%016llX\n", (unsigned long long)ctx->fp.x);
    std::fflush(stdout);
    fsync(fileno(stdout));
}
