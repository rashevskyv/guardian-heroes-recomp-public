#!/usr/bin/env python3
"""Guardian Heroes SDK edits (PC and Switch builds), run after switch/sdk-edits.py:

    python3 patches/sdk-edits-gh.py /root/gh/rexglue-sdk

InputSystem (include/rex/input/input_system.h, src/input/input_system.cpp):
 1. SetGameInputBlocked: while the in-game settings window (or an SDK ImGui popup) is open the guest
    sees a neutral pad and an empty keystroke queue; GetStateRaw reads the live pad for the window itself.
 2. One recursive mutex over the entry points: the settings window polls the pad on the UI thread while
    guest threads call GetState, and RefreshDevices mutates the device vectors (heap corruption otherwise).
Idempotent: the .cpp carries the marker `gh-edits` once the edit is in.
"""
import os, sys

SDK = sys.argv[1]
NL = chr(10)
MARK = '// gh-edits: input blocking and locking'


def rw(rel, fn):
    path = os.path.join(SDK, rel)
    s = open(path, encoding='utf-8').read()
    t = fn(s)
    if t != s:
        open(path, 'w', encoding='utf-8', newline=NL).write(t)
        print('changed:', rel)


def header(s):
    if 'GetStateRaw' in s:
        return s
    old = '  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state);' + NL
    assert old in s, 'input_system.h anchor'
    return s.replace(old, old +
                     '  /// The live pad, also while the guest is blocked (the in-game settings window reads it).' + NL +
                     '  X_RESULT GetStateRaw(uint32_t user_index, X_INPUT_STATE* out_state);' + NL +
                     '  /// While true the guest sees a neutral pad and no keystrokes (gh: settings window open).' + NL +
                     '  static void SetGameInputBlocked(bool blocked);' + NL, 1)


def source(s):
    if MARK in s:
        return s
    # statics before the first function of the namespace
    anchor = 'namespace rex::input {' + NL
    assert anchor in s, 'input_system.cpp namespace'
    s = s.replace(anchor, anchor + MARK + NL +
                  'static std::atomic<bool> g_game_input_blocked{false};' + NL +
                  'static std::recursive_mutex g_input_mutex;' + NL +
                  'void InputSystem::SetGameInputBlocked(bool blocked) { g_game_input_blocked.store(blocked); }' + NL + NL, 1)
    if '#include <atomic>' not in s:
        s = s.replace('#include <rex/input/input_system.h>' + NL,
                      '#include <rex/input/input_system.h>' + NL + '#include <atomic>' + NL + '#include <mutex>' + NL, 1)
    lock = '  std::lock_guard<std::recursive_mutex> input_lock(g_input_mutex);' + NL

    def after_open_brace(src, sig):
        i = src.index(sig)
        j = src.index('{' + NL, i) + 2
        return src[:j], src[j:]

    # GetState -> GetStateRaw (the old body, now locked) + a wrapper that blanks the pad while blocked
    sig = 'X_RESULT InputSystem::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {' + NL
    assert sig in s, 'GetState'
    s = s.replace(sig,
                  sig + '  X_RESULT result = GetStateRaw(user_index, out_state);' + NL +
                  '  if (result == X_ERROR_SUCCESS && g_game_input_blocked.load()) {' + NL +
                  '    out_state->gamepad = X_INPUT_GAMEPAD{};' + NL + '  }' + NL +
                  '  return result;' + NL + '}' + NL + NL +
                  'X_RESULT InputSystem::GetStateRaw(uint32_t user_index, X_INPUT_STATE* out_state) {' + NL + lock, 1)
    for name in ('X_RESULT InputSystem::GetCapabilities(', 'X_RESULT InputSystem::SetState('):
        head, tail = after_open_brace(s, name)
        s = head + lock + tail
    head, tail = after_open_brace(s, 'X_RESULT InputSystem::GetKeystroke(')
    s = head + '  if (g_game_input_blocked.load()) {' + NL + '    return X_ERROR_EMPTY;' + NL + '  }' + NL + lock + tail
    return s


rw('include/rex/input/input_system.h', header)
rw('src/input/input_system.cpp', source)


# Windows build with MSYS2 clang (mingw): no SEH __try/__except; the debugger thread-name trick is optional.
def threading_win(s):
    if 'gh-edits: no SEH' in s:
        return s
    old = '  __try {' + NL
    assert old in s, 'threading_win.cpp __try'
    i = s.index(old)
    j = s.index('  }' + NL, s.index('__except', i)) + 4
    block = s[i:j]
    return (s[:i] + '#ifdef _MSC_VER  // gh-edits: no SEH on mingw' + NL + block + '#else' + NL +
            '  (void)info;' + NL + '#endif' + NL + s[j:])


if os.path.exists(os.path.join(SDK, 'src/core/threading_win.cpp')):
    rw('src/core/threading_win.cpp', threading_win)


# mingw clang: FARPROC -> void* needs a cast (xinput driver keeps its entry points as void*).
def xinput_cast(s):
    import re
    return re.sub(r'^(  XInput\w+_ = )(xi\w+);', r'\1reinterpret_cast<void*>(\2);', s, flags=re.M)


if os.path.exists(os.path.join(SDK, 'src/input/xinput/xinput_input_driver.cpp')):
    rw('src/input/xinput/xinput_input_driver.cpp', xinput_cast)


# MSYS2 leaves CMAKE_SYSTEM_PROCESSOR empty: thirdparty/ then skips the x86 FFmpeg sources (link errors).
def win_processor(s):
    if 'gh-edits: win processor' in s:
        return s
    old = 'set(REX_TARGET_PROCESSOR "${CMAKE_SYSTEM_PROCESSOR}")' + NL
    assert old in s, 'CMakeLists REX_TARGET_PROCESSOR'
    return s.replace(old, 'if(WIN32 AND NOT CMAKE_SYSTEM_PROCESSOR)  # gh-edits: win processor' + NL +
                     '    set(CMAKE_SYSTEM_PROCESSOR AMD64)' + NL + 'endif()' + NL + old, 1)


rw('CMakeLists.txt', win_processor)


# Switch audio: the guest frames go to libnx audout (src/nx_audout_driver.h) instead of the silent driver.
def nx_audio(s):
    if 'gh-edits: audout' in s:
        return s
    anchor = '#include <rex/thread.h>' + NL
    assert anchor in s, 'nop_audio_system.cpp include anchor'
    s = s.replace(anchor, anchor + '#ifdef __SWITCH__  // gh-edits: audout' + NL +
                  '#include "nx_audout_driver.h"' + NL + '#endif' + NL, 1)
    old = '  *out_driver = new NopAudioDriver(memory(), semaphore);' + NL
    assert old in s, 'nop CreateDriver'
    return s.replace(old, '#ifdef __SWITCH__' + NL + '  *out_driver = new NxAudoutDriver(memory(), semaphore);' + NL +
                     '#else' + NL + old + '#endif' + NL, 1)


nop = os.path.join(SDK, 'src/audio/nop/nop_audio_system.cpp')
if os.path.exists(nop):
    import shutil
    shutil.copyfile(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'nx_audout_driver.h'),
                    os.path.join(SDK, 'src/audio/nop/nx_audout_driver.h'))
    rw('src/audio/nop/nop_audio_system.cpp', nx_audio)


# Older switch/sdk-edits.py runs left a block in AudioSystem::WorkerThreadMain that ends the worker at once on the
# Switch ("no audio backend"): the guest then never gets its render-driver callbacks. Take it out (a no-op in a
# fresh SDK copy).
def keep_audio_worker(s):
    marker = 'no audio backend, the audio worker leaves'
    if marker not in s:
        return s
    start = s.rindex('#if REX_PLATFORM_SWITCH' + NL, 0, s.index(marker))
    end = s.index('#endif' + NL, s.index(marker)) + len('#endif' + NL)
    return s[:start] + s[end:]


if os.path.exists(os.path.join(SDK, 'src/audio/audio_system.cpp')):
    rw('src/audio/audio_system.cpp', keep_audio_worker)


# Switch audio diagnostics: time the guest callback per client in the audio worker (printed every 5 s).
def worker_timing(s):
    if 'gh-edits: callback timing' in s:
        return s
    old = '        function_dispatcher_->Execute(worker_thread_->thread_state(), client_callback, args,' + NL +           '                                      rex::countof(args));' + NL
    assert old in s, 'audio_system.cpp Execute anchor'
    new = '        auto gh_t0 = std::chrono::steady_clock::now();  // gh-edits: callback timing' + NL + old +           '        {' + NL +           '          static uint64_t calls[8], total_us[8], max_us[8], over5[8];' + NL +           '          static auto last_print = gh_t0;' + NL +           '          const uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - gh_t0).count();' + NL +           '          const size_t ci = index < 8 ? index : 7;' + NL +           '          ++calls[ci]; total_us[ci] += us; if (us > max_us[ci]) max_us[ci] = us; if (us > 5000) ++over5[ci];' + NL +           '          if (std::chrono::steady_clock::now() - last_print > std::chrono::seconds(5)) {' + NL +           '            last_print = std::chrono::steady_clock::now();' + NL +           '            for (size_t k = 0; k < 8; ++k) if (calls[k]) {' + NL +           '              std::printf("[nx] audio worker: client %zu callbacks %llu, mean %llu us, max %llu us, %llu over 5 ms\\n", k, (unsigned long long)calls[k], (unsigned long long)(total_us[k] / calls[k]), (unsigned long long)max_us[k], (unsigned long long)over5[k]);' + NL +           '              calls[k] = total_us[k] = max_us[k] = over5[k] = 0;' + NL +           '            }' + NL +           '            std::fflush(stdout);' + NL +           '          }' + NL +           '        }' + NL
    s = s.replace(old, new, 1)
    if '#include <chrono>' not in s:
        s = s.replace('#include <rex/audio/audio_system.h>' + NL, '#include <rex/audio/audio_system.h>' + NL + '#include <chrono>' + NL + '#include <cstdio>' + NL, 1)
    return s


if os.path.exists(os.path.join(SDK, 'src/audio/audio_system.cpp')):
    rw('src/audio/audio_system.cpp', worker_timing)


# Switch audio: one host thread per client (patches/audio_client_threads.inc) instead of the single audio worker
# running every guest callback in turn; the worker itself only waits for the shutdown event.
def client_threads(s):
    if 'gh-edits: client threads' in s:
        return s
    inc = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'audio_client_threads.inc'), encoding='utf-8').read()

    def once(src, anchor, text, after=True):
        assert anchor in src, 'audio_system.cpp anchor: ' + anchor[:40]
        return src.replace(anchor, anchor + text if after else text + anchor, 1)

    s = once(s, 'namespace rex::audio {' + NL,
             '#if REX_PLATFORM_SWITCH' + NL + 'static system::KernelState* gh_kernel_state = nullptr;' + NL +
             'static uint32_t gh_client_gen[8];' + NL + '#endif' + NL)
    s = once(s, 'X_STATUS AudioSystem::Setup(system::KernelState* kernel_state) {' + NL,
             '#if REX_PLATFORM_SWITCH' + NL + '  gh_kernel_state = kernel_state;' + NL + '#endif' + NL)
    s = once(s, '  clients_[index] = {driver, callback, callback_arg, ptr, true};' + NL, inc)
    s = once(s, '  clients_[index] = {nullptr, 0, 0, 0, false};' + NL,
             '#if REX_PLATFORM_SWITCH' + NL + '  ++gh_client_gen[index];' + NL + '#endif' + NL)
    s = once(s, '  while (worker_running_) {' + NL,
             '#if REX_PLATFORM_SWITCH  // gh-edits: the clients have their own threads' + NL +
             '    rex::thread::Wait(shutdown_event_.get(), true, std::chrono::milliseconds(500));' + NL +
             '    continue;' + NL + '#endif' + NL)
    for inc_line in ('#include <chrono>', '#include <memory>', '#include <string>'):
        if inc_line not in s:
            s = s.replace('#include <rex/audio/audio_system.h>' + NL, '#include <rex/audio/audio_system.h>' + NL + inc_line + NL, 1)
    return s


if os.path.exists(os.path.join(SDK, 'src/audio/audio_system.cpp')):
    rw('src/audio/audio_system.cpp', client_threads)
