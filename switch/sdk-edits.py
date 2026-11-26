#!/usr/bin/env python3
"""Edits the ReXGlue SDK checkout for the Nintendo Switch build (devkitA64 GCC, libnx, static NVK).

    python3 switch/sdk-edits.py /root/gh/rexglue-sdk

Idempotent: every edit checks for its own marker first. Run it on the SDK with stuntdouble's
patches/sdk/*.patch already applied; `git -C <sdk> diff > patches/sdk-switch/0001-nintendo-switch.patch`
records the result. The host-side sources (memory, fibers, window, surface, exceptions) live in
switch/host/ of this repo and are handed to the SDK through REXGLUE_SWITCH_* CMake variables.
"""
import os, re, sys

SDK = sys.argv[1]
changed = []
NL = chr(10)


def edit(rel, pairs, must=True):
    path = os.path.join(SDK, rel)
    s = open(path, encoding='utf-8').read()
    orig = s
    for old, new in pairs:
        if new in s:
            continue
        if old not in s:
            # A later corrective edit may have rewritten this spot already (the base text is gone,
            # the corrected text differs from `new`). Warn; STRICT=1 (fresh SDK checkout) makes it fatal.
            if must and os.environ.get('STRICT') == '1':
                raise SystemExit(f'{rel}: anchor not found:\n{old}')
            if must:
                print(f'warning: {rel}: anchor not found (already edited?): {old[:60]!r}')
            continue
        s = s.replace(old, new, 1)
    if s != orig:
        open(path, 'w', encoding='utf-8', newline='\n').write(s)
        changed.append(rel)


def write(rel, text):
    path = os.path.join(SDK, rel)
    if os.path.exists(path) and open(path, encoding='utf-8').read() == text:
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, 'w', encoding='utf-8', newline='\n').write(text)
    changed.append(rel)


# --- CMake: toolchain, platform, library types -------------------------------------------------
edit('CMakeLists.txt', [
    ('if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Apple)?Clang$")',
     'if(NOT NINTENDO_SWITCH AND NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Apple)?Clang$")'),
    ('    -ffp-model=strict\n',
     '    # GCC (devkitA64) has no -ffp-model; keep mul+add unfused for PPC parity.\n'
     '    $<$<NOT:$<BOOL:${NINTENDO_SWITCH}>>:-ffp-model=strict>\n'
     '    $<$<BOOL:${NINTENDO_SWITCH}>:-ffp-contract=off>\n'),
    ('if(WIN32)\n    if(REX_TARGET_PROCESSOR MATCHES "AMD64|x86_64")\n        set(REX_PLATFORM "win-amd64")',
     'if(NINTENDO_SWITCH)\n    set(REX_PLATFORM "switch-arm64")\n'
     '    add_compile_definitions(REX_PLATFORM_SWITCH=1 REX_PLATFORM_LINUX=1)\n'
     'elseif(WIN32)\n    if(REX_TARGET_PROCESSOR MATCHES "AMD64|x86_64")\n        set(REX_PLATFORM "win-amd64")'),
    ('add_subdirectory(src/codegen)\nadd_subdirectory(src/rexglue)\n',
     'if(NOT NINTENDO_SWITCH)\n    # Host tools only.\n    add_subdirectory(src/codegen)\n    add_subdirectory(src/rexglue)\nendif()\n'),
])

edit('thirdparty/CMakeLists.txt', [
    ('add_subdirectory(sdl3)\n',
     'if(NINTENDO_SWITCH)\n    # No SDL3 for libnx: the Switch host layer provides the window, input and audio.\n'
     '    add_library(SDL3 INTERFACE)\n    add_library(SDL3::SDL3 ALIAS SDL3)\nelse()\n    add_subdirectory(sdl3)\nendif()\n'),
])

edit('src/core/CMakeLists.txt', [
    ('elseif(UNIX)\n    # Linux and macOS/Darwin share the POSIX sources;',
     'elseif(UNIX OR NINTENDO_SWITCH)\n    # Linux and macOS/Darwin share the POSIX sources;'),
    ('# Xenos data-format layer (stateless; headers under include/rex/graphics/).',
     'if(NINTENDO_SWITCH)\n'
     '    # Horizon: no mmap, no signals, no ucontext. The replacements come from the port.\n'
     '    get_target_property(_rexcore_sources rexcore SOURCES)\n'
     '    list(FILTER _rexcore_sources EXCLUDE REGEX\n'
     '        "(exception_handler_posix|fiber_posix|mapped_memory_posix|memory_posix|seh_posix|threading_posix)\\\\.cpp$")\n'
     '    set_property(TARGET rexcore PROPERTY SOURCES ${_rexcore_sources})\n'
     '    target_sources(rexcore PRIVATE ${REXGLUE_SWITCH_CORE_SOURCES} threading_switch.cpp)\n'
     '    target_include_directories(rexcore PUBLIC ${REXGLUE_SWITCH_INCLUDE_DIR})\n'
     'endif()\n\n'
     '# Xenos data-format layer (stateless; headers under include/rex/graphics/).'),
    ('elseif(UNIX)\n    target_link_libraries(rexcore PRIVATE pthread rt dl)',
     'elseif(UNIX AND NOT NINTENDO_SWITCH)\n    target_link_libraries(rexcore PRIVATE pthread rt dl)'),
])

edit('src/system/CMakeLists.txt', [
    ('add_library(rexruntime SHARED ${REXSYSTEM_SOURCES})',
     'if(NINTENDO_SWITCH)\n    set(REXGLUE_RUNTIME_LIB_TYPE STATIC)\nelse()\n    set(REXGLUE_RUNTIME_LIB_TYPE SHARED)\nendif()\n'
     'add_library(rexruntime ${REXGLUE_RUNTIME_LIB_TYPE} ${REXSYSTEM_SOURCES})'),
])

edit('src/graphics/CMakeLists.txt', [
    ('add_library(rexgpu-xenos SHARED ${REXGPU_XENOS_SOURCES})',
     'if(NINTENDO_SWITCH)\n    set(REXGLUE_GPU_PLUGIN_LIB_TYPE STATIC)\nelse()\n    set(REXGLUE_GPU_PLUGIN_LIB_TYPE SHARED)\nendif()\n'
     'add_library(rexgpu-xenos ${REXGLUE_GPU_PLUGIN_LIB_TYPE} ${REXGPU_XENOS_SOURCES})'),
])

edit('src/ui/CMakeLists.txt', [
    ('else()\n    set(REXUI_PLATFORM_SOURCES\n        surface_gnulinux.cpp\n    )\nendif()',
     'elseif(NINTENDO_SWITCH)\n    set(REXUI_PLATFORM_SOURCES ${REXGLUE_SWITCH_UI_SOURCES})\n'
     'else()\n    set(REXUI_PLATFORM_SOURCES\n        surface_gnulinux.cpp\n    )\nendif()'),
    ('add_library(rexui OBJECT ${REXUI_SOURCES})',
     'if(NINTENDO_SWITCH)\n    list(FILTER REXUI_SOURCES EXCLUDE REGEX\n'
     '        "(window_sdl|windowed_app_context_sdl|sdl_virtual_key)\\\\.cpp$")\nendif()\n'
     'add_library(rexui OBJECT ${REXUI_SOURCES})'),
    ('if(UNIX AND NOT APPLE)\n    find_package(PkgConfig REQUIRED)',
     'if(UNIX AND NOT APPLE AND NOT NINTENDO_SWITCH)\n    find_package(PkgConfig REQUIRED)'),
])

edit('src/input/CMakeLists.txt', [
    ('else()\n    target_sources(rexinput PRIVATE sdl/sdl_input_driver.cpp)\nendif()',
     'elseif(NOT NINTENDO_SWITCH)\n    target_sources(rexinput PRIVATE sdl/sdl_input_driver.cpp)\nendif()'),
])

edit('src/audio/CMakeLists.txt', [
    ('    # SDL backend\n    sdl/sdl_audio_system.cpp\n    sdl/sdl_audio_driver.cpp\n)',
     ')\nif(NOT NINTENDO_SWITCH)\n    target_sources(rexaudio PRIVATE sdl/sdl_audio_system.cpp sdl/sdl_audio_driver.cpp)\nendif()'),
])

edit('cmake/rexglue_helpers.cmake', [
    ('    target_sources(${target_name} PRIVATE\n        ${REXGLUE_SHARE_DIR}/windowed_app_main_sdl.cpp\n        ${REXGLUE_SHARE_DIR}/rex_app.cpp)',
     '    if(NINTENDO_SWITCH)\n        target_sources(${target_name} PRIVATE\n            ${REXGLUE_SWITCH_MAIN_SOURCE}\n'
     '            ${REXGLUE_SHARE_DIR}/rex_app.cpp)\n    else()\n        target_sources(${target_name} PRIVATE\n'
     '            ${REXGLUE_SHARE_DIR}/windowed_app_main_sdl.cpp\n            ${REXGLUE_SHARE_DIR}/rex_app.cpp)\n    endif()'),
    ('        add_custom_command(TARGET ${target_name} POST_BUILD\n            COMMAND ${CMAKE_COMMAND} -E copy_if_different\n'
     '                $<TARGET_FILE:${_plugin_target}>\n                $<TARGET_FILE_DIR:${target_name}>\n            VERBATIM\n        )\n        unset(_plugin_target)',
     '        if(NINTENDO_SWITCH)\n            # Static plugin: linked in, created by gpu_plugin_loader.cpp directly.\n'
     '            target_link_libraries(${target_name} PRIVATE ${_plugin_target})\n        else()\n'
     '        add_custom_command(TARGET ${target_name} POST_BUILD\n            COMMAND ${CMAKE_COMMAND} -E copy_if_different\n'
     '                $<TARGET_FILE:${_plugin_target}>\n                $<TARGET_FILE_DIR:${target_name}>\n            VERBATIM\n        )\n'
     '        endif()\n        unset(_plugin_target)'),
])

# --- Headers --------------------------------------------------------------------------------------
edit('include/rex/platform.h', [
    ('#elif defined(__gnu_linux__)\n#define REX_PLATFORM_GNU_LINUX 1\n#define REX_PLATFORM_LINUX 1\n',
     '#elif defined(__SWITCH__)\n// Nintendo Switch (libnx, newlib): POSIX-like, not GNU/Linux.\n#define REX_PLATFORM_SWITCH 1\n'
     '#define REX_PLATFORM_LINUX 1\n#elif defined(__gnu_linux__)\n#define REX_PLATFORM_GNU_LINUX 1\n#define REX_PLATFORM_LINUX 1\n'),
    ('#ifndef REX_PLATFORM_GNU_LINUX\n#define REX_PLATFORM_GNU_LINUX 0\n#endif\n',
     '#ifndef REX_PLATFORM_GNU_LINUX\n#define REX_PLATFORM_GNU_LINUX 0\n#endif\n#ifndef REX_PLATFORM_SWITCH\n#define REX_PLATFORM_SWITCH 0\n#endif\n'),
])

edit('include/rex/thread/fiber.h', [
    ('#include <ucontext.h>\n', '#if !REX_PLATFORM_SWITCH\n#include <ucontext.h>\n#endif\n'),
    ('  ucontext_t context_{};\n',
     '#if REX_PLATFORM_SWITCH\n  // sp, lr, x19-x29, d8-d15: fiber_nx.cpp.\n  uint64_t context_[24] = {};\n#else\n  ucontext_t context_{};\n#endif\n'),
])

edit('include/rex/ui/vulkan/api.h', [
    ('#if REX_PLATFORM_GNU_LINUX\n#ifndef VK_USE_PLATFORM_XCB_KHR',
     '#if REX_PLATFORM_SWITCH\n#ifndef VK_USE_PLATFORM_VI_NN\n#define VK_USE_PLATFORM_VI_NN\n#endif\n#endif\n\n'
     '#if REX_PLATFORM_GNU_LINUX\n#ifndef VK_USE_PLATFORM_XCB_KHR'),
])

edit('include/rex/ui/vulkan/instance.h', [
    ('    bool ext_KHR_xcb_surface = false;  // #6' + NL + '    bool ext_NN_vi_surface = false;    // #63, Nintendo Switch' + NL + '#endif' + NL,
     '    bool ext_KHR_xcb_surface = false;  // #6' + NL + '#endif' + NL + '#ifdef VK_USE_PLATFORM_VI_NN' + NL + '    bool ext_NN_vi_surface = false;  // #63, Nintendo Switch' + NL + '#endif' + NL),
    ('    bool ext_KHR_xcb_surface = false;  // #6' + NL + '#endif' + NL,
     '    bool ext_KHR_xcb_surface = false;  // #6' + NL + '#endif' + NL + '#ifdef VK_USE_PLATFORM_VI_NN' + NL + '    bool ext_NN_vi_surface = false;  // #63, Nintendo Switch' + NL + '#endif' + NL),
    ('#ifdef VK_USE_PLATFORM_XCB_KHR\n#include <rex/ui/vulkan/functions/instance_khr_xcb_surface.inc>\n#endif\n',
     '#ifdef VK_USE_PLATFORM_XCB_KHR\n#include <rex/ui/vulkan/functions/instance_khr_xcb_surface.inc>\n#endif\n'
     '#ifdef VK_USE_PLATFORM_VI_NN\n#include <rex/ui/vulkan/functions/instance_nn_vi_surface.inc>\n#endif\n'),
])
write('include/rex/ui/vulkan/functions/instance_nn_vi_surface.inc', 'XE_UI_VULKAN_FUNCTION(vkCreateViSurfaceNN)\n')

edit('include/rex/ui/surface.h', [
    ('    // macOS — CAMetalLayer presented via MoltenVK (VK_EXT_metal_surface).\n    kTypeIndex_CAMetalLayer,\n',
     '    // macOS — CAMetalLayer presented via MoltenVK (VK_EXT_metal_surface).\n    kTypeIndex_CAMetalLayer,\n'
     '    // Nintendo Switch (VK_NN_vi_surface on an nwindow).\n    kTypeIndex_NnVi,\n'),
    ('    kTypeFlag_CAMetalLayer = TypeFlags(1) << kTypeIndex_CAMetalLayer,\n',
     '    kTypeFlag_CAMetalLayer = TypeFlags(1) << kTypeIndex_CAMetalLayer,\n    kTypeFlag_NnVi = TypeFlags(1) << kTypeIndex_NnVi,\n'),
])

# renderdoc_app.h (vendored): no platform branch for the Switch.
edit('thirdparty/renderdoc/renderdoc_app.h', [
    ('#elif defined(__linux__)', '#elif defined(__linux__) || defined(__SWITCH__)'),
])

# --- Sources ----------------------------------------------------------------------------------------
edit('src/ui/vulkan/vulkan_instance.cpp', [
    ('#ifdef VK_USE_PLATFORM_XCB_KHR\n    // #6.\n    requested_extensions.emplace("VK_KHR_xcb_surface",\n                                 &vulkan_instance->extensions_.ext_KHR_xcb_surface);\n#endif\n',
     '#ifdef VK_USE_PLATFORM_XCB_KHR\n    // #6.\n    requested_extensions.emplace("VK_KHR_xcb_surface",\n                                 &vulkan_instance->extensions_.ext_KHR_xcb_surface);\n#endif\n'
     '#ifdef VK_USE_PLATFORM_VI_NN\n    // #63.\n    requested_extensions.emplace("VK_NN_vi_surface",\n                                 &vulkan_instance->extensions_.ext_NN_vi_surface);\n#endif\n'),
    ('#else\n  loader_loaded = vulkan_instance->loader_.Load(platform::lib_names::kVulkanLoader);\n  if (!loader_loaded) {\n    REXLOG_ERROR("Failed to load {}", platform::lib_names::kVulkanLoader);\n    return nullptr;\n  }\n#endif\n',
     '#elif REX_PLATFORM_SWITCH\n  // No loader: the driver (mesa-switch NVK, libvulkan.a) is linked in.\n  loader_loaded = true;\n#else\n'
     '  loader_loaded = vulkan_instance->loader_.Load(platform::lib_names::kVulkanLoader);\n  if (!loader_loaded) {\n    REXLOG_ERROR("Failed to load {}", platform::lib_names::kVulkanLoader);\n    return nullptr;\n  }\n#endif\n'),
    ('#define XE_VULKAN_LOAD_LOADER_FUNCTION(name) \\\n  functions_loaded &= (ifn.name = vulkan_instance->loader_.GetSymbol<PFN_##name>(#name)) != nullptr;\n  XE_VULKAN_LOAD_LOADER_FUNCTION(vkGetInstanceProcAddr);\n  XE_VULKAN_LOAD_LOADER_FUNCTION(vkDestroyInstance);\n#undef XE_VULKAN_LOAD_LOADER_FUNCTION\n',
     '#if REX_PLATFORM_SWITCH\n  ifn.vkGetInstanceProcAddr = vk_icdGetInstanceProcAddr;\n'
     '  ifn.vkDestroyInstance = PFN_vkDestroyInstance(vk_icdGetInstanceProcAddr(nullptr, "vkDestroyInstance"));\n'
     '  functions_loaded &= ifn.vkGetInstanceProcAddr != nullptr;\n#else\n'
     '#define XE_VULKAN_LOAD_LOADER_FUNCTION(name) \\\n  functions_loaded &= (ifn.name = vulkan_instance->loader_.GetSymbol<PFN_##name>(#name)) != nullptr;\n  XE_VULKAN_LOAD_LOADER_FUNCTION(vkGetInstanceProcAddr);\n  XE_VULKAN_LOAD_LOADER_FUNCTION(vkDestroyInstance);\n#undef XE_VULKAN_LOAD_LOADER_FUNCTION\n#endif\n'),
    ('#include <rex/ui/vulkan/instance.h>\n',
     '#include <rex/ui/vulkan/instance.h>\n#if REX_PLATFORM_SWITCH\nextern "C" PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char* name);\n#endif\n'),
])

edit('src/ui/vulkan/vulkan_presenter.cpp', [
    ('    type_flags |= Surface::kTypeFlag_XcbWindow;\n',
     '    type_flags |= Surface::kTypeFlag_XcbWindow;\n  }\n#endif\n#ifdef VK_USE_PLATFORM_VI_NN\n  if (instance_extensions.ext_NN_vi_surface) {\n    type_flags |= Surface::kTypeFlag_NnVi;\n'),
    ('#if REX_PLATFORM_MAC\n      case Surface::kTypeIndex_CAMetalLayer: {',
     '#if REX_PLATFORM_SWITCH\n      case Surface::kTypeIndex_NnVi: {\n'
     '        auto& vi_surface = static_cast<const NnViSurface&>(new_surface);\n'
     '        VkViSurfaceCreateInfoNN surface_create_info;\n'
     '        surface_create_info.sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN;\n'
     '        surface_create_info.pNext = nullptr;\n        surface_create_info.flags = 0;\n'
     '        surface_create_info.window = vi_surface.window();\n'
     '        std::printf("[nx] vkCreateViSurfaceNN %p window %p\\n", (void*)ifn.vkCreateViSurfaceNN,\n'
     '                    surface_create_info.window);\n        std::fflush(stdout);\n'
     '        vulkan_surface_create_result = ifn.vkCreateViSurfaceNN(\n'
     '            instance, &surface_create_info, nullptr, &paint_context_.vulkan_surface);\n      } break;\n#endif\n'
     '#if REX_PLATFORM_MAC\n      case Surface::kTypeIndex_CAMetalLayer: {'),
])

edit('src/ui/vulkan/vulkan_presenter.cpp', [
    ('#include <rex/ui/vulkan/presenter.h>' + NL,
     '#include <rex/ui/vulkan/presenter.h>' + NL + '#include <cstdio>' + NL + '#if REX_PLATFORM_SWITCH' + NL +
     '#include <rex/ui/surface_nx.h>' + NL + '#endif' + NL),
])

edit('src/system/gpu_plugin_loader.cpp', [
    ('std::unique_ptr<IGraphicsSystem> LoadGpuPlugin(std::string_view name, std::string_view backend) {\n',
     '#if REX_PLATFORM_SWITCH\n// The plugin is a static library (no dlopen on Horizon): call its factory directly.\n'
     'extern "C" uint32_t rex_gpu_abi_version(void);\n'
     'extern "C" rex::system::IGraphicsSystem* rex_gpu_create(uint32_t abi_version,\n'
     '                                                      const rex::system::GpuCreateInfo* info);\n#endif\n\n'
     'std::unique_ptr<IGraphicsSystem> LoadGpuPlugin(std::string_view name, std::string_view backend) {\n'
     '#if REX_PLATFORM_SWITCH\n  std::string nx_backend(backend);\n  GpuCreateInfo nx_info{};\n  nx_info.struct_size = sizeof(GpuCreateInfo);\n'
     '  nx_info.backend = nx_backend.c_str();\n  IGraphicsSystem* nx_system = rex_gpu_create(kGpuPluginAbiVersion, &nx_info);\n'
     '  if (!nx_system) {\n    REXSYS_ERROR("GPU plugin \'{}\' (static) returned no graphics system (backend \'{}\')", name,\n'
     '                 nx_backend);\n    return nullptr;\n  }\n  return std::unique_ptr<IGraphicsSystem>(nx_system);\n#endif\n'),
])

edit('src/input/input_system.cpp', [
    ('REXCVAR_DEFINE_STRING(input_backend, "sdl", "Input", "Input backend: sdl, xinput")\n    .allowed({"sdl", "xinput"});',
     '#if REX_PLATFORM_SWITCH\nREXCVAR_DEFINE_STRING(input_backend, "nop", "Input", "Input backend: nop")\n    .allowed({"nop"});\n#else\n'
     'REXCVAR_DEFINE_STRING(input_backend, "sdl", "Input", "Input backend: sdl, xinput")\n    .allowed({"sdl", "xinput"});\n#endif'),
])
# Close the #if after the sdl block: the block ends before the MnK driver comment.
edit('src/input/input_system.cpp', [
    ('    // MnK driver (keyboard/mouse -> controller emulation)\n',
     '#endif\n\n    // MnK driver (keyboard/mouse -> controller emulation)\n'),
])

edit('src/ui/rex_app.cpp', [
    ('#include <rex/system/gpu_plugin.h>' + NL,
     '#include <rex/system/gpu_plugin.h>' + NL + '#if REX_PLATFORM_SWITCH' + NL +
     '#include <rex/audio/nop/nop_audio_system.h>' + NL + '#endif' + NL),
    ('  config_.audio_factory = REX_AUDIO_BACKEND(rex::audio::sdl::SDLAudioSystem);',
     '#if REX_PLATFORM_SWITCH\n  config_.audio_factory = REX_AUDIO_BACKEND(rex::audio::nop::NopAudioSystem);\n#else\n'
     '  config_.audio_factory = REX_AUDIO_BACKEND(rex::audio::sdl::SDLAudioSystem);\n#endif'),
])

edit('src/core/filesystem_posix.cpp', [
    ('#if defined(__APPLE__)' + NL + 'using rex_off64_t = off_t;',
     '#if defined(__APPLE__) || defined(__SWITCH__)' + NL + '// newlib: off_t is 64-bit, no *64 forms.' + NL +
     'using rex_off64_t = off_t;'),
])

# FFmpeg: a config for the Switch (the Linux aarch64 one without the glibc-only calls).
cfg = os.path.join(SDK, 'thirdparty/FFmpeg/config_linux_aarch64.h')
sw = os.path.join(SDK, 'thirdparty/FFmpeg/config_switch_aarch64.h')
if os.path.exists(cfg) and not os.path.exists(sw):
    text = open(cfg, encoding='utf-8').read()
    for name in ('HAVE_ARC4RANDOM', 'HAVE_ARC4RANDOM_BUF', 'HAVE_GETAUXVAL', 'HAVE_SCHED_GETAFFINITY', 'HAVE_MMAP',
                 'HAVE_SYSCTL', 'HAVE_PTHREAD_SETNAME_NP', 'HAVE_GETRUSAGE', 'HAVE_MKSTEMP', 'HAVE_DOS_PATHS',
                 'HAVE_ISATTY', 'HAVE_SETMODE', 'HAVE_GETPROCESSMEMORYINFO', 'HAVE_FCNTL', 'HAVE_GLOB', 'HAVE_SYSCONF',
                 'HAVE_STRERROR_R', 'HAVE_SYS_MMAN_H', 'HAVE_SYS_UN_H', 'HAVE_FORK', 'HAVE_MPROTECT', 'HAVE_PTHREAD_CANCEL',
                 'HAVE_SYS_RESOURCE_H', 'HAVE_GETRUSAGE', 'HAVE_SECURE_GETENV'):
        text = re.sub(rf'#define {name} 1', f'#define {name} 0', text)
    open(sw, 'w', encoding='utf-8', newline='\n').write(text)
    changed.append('thirdparty/FFmpeg/config_switch_aarch64.h')
edit('thirdparty/FFmpeg/config.h', [
    ('#elif defined(__gnu_linux__)\n',
     '#elif defined(__SWITCH__)\n#include "config_switch_aarch64.h"\n#elif defined(__gnu_linux__)\n'),
])


# --- Guest memory: one R/W view, the other views folded in software (CLAUDE.md "Switch memory design")
edit('include/rex/system/xmemory.h', [
    ('  inline T TranslateVirtual(uint32_t guest_address) const {\n    uint8_t* host_address = virtual_membase_ + guest_address;\n',
     '  inline T TranslateVirtual(uint32_t guest_address) const {\n'
     '#if REX_PLATFORM_SWITCH\n'
     '    // One R/W view of the physical arena (the 0xA0000000 block) and of the XEX (0x80000000):\n'
     '    // 0xC0000000, 0xE0000000 (+4 KB) and 0x90000000 fold onto them.\n'
     '    if (guest_address >= 0xE0000000u) {\n'
     '      guest_address = 0xA0000000u + (guest_address - 0xE0000000u) + 0x1000u;\n'
     '    } else if (guest_address >= 0xC0000000u) {\n'
     '      guest_address -= 0x20000000u;\n'
     '    } else if (guest_address >= 0x90000000u && guest_address < 0xA0000000u) {\n'
     '      guest_address -= 0x10000000u;\n'
     '    }\n'
     '#endif\n'
     '    uint8_t* host_address = virtual_membase_ + guest_address;\n'),
])

edit('src/system/xmemory.cpp', [
    ('#include <rex/system/mmio_handler.h>\n',
     '#include <rex/system/mmio_handler.h>\n#if REX_PLATFORM_SWITCH\nnamespace rex::memory::nx {\nuint8_t* arena_base();\n}\n#endif\n'),
    ('#else\n  // Attempt to create our views. This may fail at the first address\n  // we pick, so try a few times.\n  mapping_base_ = 0;\n',
     '#elif REX_PLATFORM_SWITCH\n  // The arena was reserved by CreateFileMappingHandle (memory_nx.cpp); the views are blocks in it.\n'
     '  mapping_base_ = rex::memory::nx::arena_base();\n  if (!mapping_base_ || MapViews(mapping_base_)) {\n'
     '    REXSYS_ERROR("Unable to map the guest memory blocks.");\n    assert_always();\n    return false;\n  }\n'
     '#else\n  // Attempt to create our views. This may fail at the first address\n  // we pick, so try a few times.\n  mapping_base_ = 0;\n'),
    ('  physical_membase_ = mapping_base_ + 0x100000000ull;\n',
     '#if REX_PLATFORM_SWITCH\n  // The raw physical view is the 0xA0000000 block.\n  physical_membase_ = mapping_base_ + 0xA0000000ull;\n#else\n'
     '  physical_membase_ = mapping_base_ + 0x100000000ull;\n#endif\n'),
    ('  uint32_t max_base_page_number = high_page_number + 1 - page_count;\n  if (top_down) {\n',
     '  uint32_t max_base_page_number = high_page_number + 1 - page_count;\n'
     '#if REX_PLATFORM_SWITCH\n  // Only the bottom of each virtual range is backed by RAM (memory_nx.cpp blocks).\n'
     '  if (heap_type_ != HeapType::kGuestPhysical) {\n    top_down = false;\n  }\n#endif\n  if (top_down) {\n'),
])

edit('resources/templates/codegen/pch_h.inja', [('static inline s64 rex_nx_host_offset(u32 a) {', 'static inline __attribute__((always_inline)) int64_t rex_nx_host_offset(u32 a) {')], must=False)
# The codegen pch (embedded in the rexglue CLI: rebuild it and rerun codegen after this edit).
edit('resources/templates/codegen/pch_h.inja', [
    ('#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)\n#define REX_PHYS_HOST_OFFSET(addr) (((u32)(addr) >= 0xE0000000u) ? 0x1000u : 0u)\n#else\n',
     '#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)\n#define REX_PHYS_HOST_OFFSET(addr) (((u32)(addr) >= 0xE0000000u) ? 0x1000u : 0u)\n'
     '#elif defined(__SWITCH__)\n'
     '// One R/W view of the physical arena (0xA0000000) and of the XEX (0x80000000): the 0xC0000000,\n'
     '// 0xE0000000 (+4 KB) and 0x90000000 views fold onto them (switch/host/memory_nx.cpp).\n'
     'static inline __attribute__((always_inline)) int64_t rex_nx_host_offset(u32 a) {\n'
     '  return a >= 0xE0000000u ? -0x40000000ll + 0x1000 : a >= 0xC0000000u ? -0x20000000ll\n'
     '       : (a >= 0x90000000u && a < 0xA0000000u) ? -0x10000000ll : 0;\n}\n'
     '#define REX_PHYS_HOST_OFFSET(addr) rex_nx_host_offset((u32)(addr))\n#else\n'),
])

print('changed:', *changed, sep='\n  ') if changed else print('nothing to change')

# --- Added later: SDL include guard, threading without signals (appended by the author) ---------
edit('src/input/input_system.cpp', [
    ('#include <rex/input/sdl/sdl_input_driver.h>' + NL,
     '#if REX_PLATFORM_SWITCH' + NL + '#include <rex/input/nx/nx_input_driver.h>' + NL + '#else' + NL +
     '#include <rex/input/sdl/sdl_input_driver.h>' + NL + '#endif' + NL),
])

# threading_switch.cpp: threading_posix.cpp without signals, robust mutexes and Linux thread ids.
src = open(os.path.join(SDK, 'src/core/threading_posix.cpp'), encoding='utf-8').read()
reps = [
    ('  auto result = SIGRTMIN + static_cast<int>(num);' + NL + '  assert_true(result < SIGRTMAX);' + NL + '  return result;',
     '  (void)num;' + NL + '  return 0;  // Horizon: no signals'),
    ('  return static_cast<SignalType>(num - SIGRTMIN);', '  (void)num;' + NL + '  return SignalType::kThreadSuspend;'),
    ('  struct sigaction action{};' + NL + '  action.sa_flags = SA_SIGINFO | SA_RESTART;' + NL +
     '  action.sa_sigaction = signal_handler;' + NL + '  sigemptyset(&action.sa_mask);' + NL +
     '  if (sigaction(GetSystemSignal(type), &action, nullptr) != 0) {' + NL +
     '    signal_handler_installed[static_cast<size_t>(type)] = false;' + NL + '  }',
     '  // Horizon: no signals, so thread suspend and user callbacks are not delivered.' + NL + '  (void)signal_handler;'),
    ('  return static_cast<uint32_t>(syscall(SYS_gettid));',
     '  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pthread_self()));'),
    ('  uint32_t system_id() const { return static_cast<uint32_t>(thread_); }',
     '  uint32_t system_id() const { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(thread_)); }'),
    ('#if REX_PLATFORM_LINUX' + NL, '#if REX_PLATFORM_LINUX && !REX_PLATFORM_SWITCH' + NL),
    # WaitMultiple polled every 1 ms (try-lock all handles, check, sleep): on Horizon that cost the
    # audio worker half a core and every multi-object wait of the guest the same (console 0.65).
    # Every signal now also notifies one shared condition variable, and the waiter sleeps on it
    # (capped at 20 ms against a wake-up lost between its check and its wait).
    ('  virtual void* native_handle() const {' + NL + '    return const_cast<std::condition_variable&>(cond_).native_handle();' + NL + '  }' + NL,
     '  virtual void* native_handle() const {' + NL + '    return const_cast<std::condition_variable&>(cond_).native_handle();' + NL + '  }' + NL +
     '  static std::mutex& multi_mutex() {' + NL + '    static std::mutex m;' + NL + '    return m;' + NL + '  }' + NL +
     '  static std::condition_variable& multi_cond() {' + NL + '    static std::condition_variable c;' + NL + '    return c;' + NL + '  }' + NL +
     '  static void NotifyMulti() { multi_cond().notify_all(); }' + NL),
    ('cond_.notify_all();', 'cond_.notify_all();' + NL + '    NotifyMulti();'),
    ('      if (timeout == std::chrono::milliseconds::max()) {' + NL +
     '        std::this_thread::sleep_for(std::chrono::milliseconds(1));' + NL +
     '      } else {' + NL +
     '        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - now);' + NL +
     '        auto sleep_time = std::min(remaining, std::chrono::milliseconds(1));' + NL +
     '        std::this_thread::sleep_for(sleep_time);' + NL +
     '      }' + NL,
     '      {' + NL +
     '        const auto cap = std::chrono::milliseconds(20);' + NL +
     '        const auto wait_time =' + NL +
     '            (timeout == std::chrono::milliseconds::max())' + NL +
     '                ? cap' + NL +
     '                : std::min(std::chrono::duration_cast<std::chrono::milliseconds>(end_time - now), cap);' + NL +
     '        std::unique_lock<std::mutex> multi_lock(multi_mutex());' + NL +
     '        multi_cond().wait_for(multi_lock, wait_time);' + NL +
     '      }' + NL),
]
for old, new in reps:
    if old not in src and new not in src:
        raise SystemExit('threading_posix.cpp: anchor not found: ' + old[:60])
    src = src.replace(old, new)
write('src/core/threading_switch.cpp',
      '// Generated by switch/sdk-edits.py from threading_posix.cpp for Horizon: no signals, no robust' + NL +
      '// mutexes, pthread_t-based thread ids. Do not edit.' + NL + src)
print('changed (2):', *changed, sep=NL + '  ')

# glslang builds its OSDependent (Unix) library only when UNIX is set; libnx has the pthreads it needs.
edit('thirdparty/CMakeLists.txt', [
    ('    add_subdirectory(glslang)', '    if(NINTENDO_SWITCH)' + NL + '        set(UNIX 1)' + NL + '    endif()' + NL + '    add_subdirectory(glslang)'),
])

# GCC ignores GNU attributes placed before `extern "C"` on a declaration (clang accepts them), so
# every sub_* alias was dropped on the Switch: 34k undefined references. Trailing attributes work
# for both compilers.
BS = chr(92)
edit('resources/templates/codegen/pch_h.inja', [
    ('#define DEFINE_REX_FUNC(name)                                       ' + BS + NL +
     '  __attribute__((alias("__imp__" #name)))                           ' + BS + NL +
     '  __attribute__((weak, noinline)) extern "C" REX_FUNC(name);        ' + BS + NL,
     '#define DEFINE_REX_FUNC(name)                                                          ' + BS + NL +
     '  extern "C" REX_FUNC(name) __attribute__((alias("__imp__" #name), weak, noinline)); ' + BS + NL),
])

# --- GPU write tracking: dirty bytes from the store macros, flushed before uploads (Switch only) -----
edit('resources/templates/codegen/pch_h.inja', [
    ('#define REX_RAW_ADDR(x) (base + (u32)(x) + REX_PHYS_HOST_OFFSET(x))' + NL,
     '#define REX_RAW_ADDR(x) (base + (u32)(x) + REX_PHYS_HOST_OFFSET(x))' + NL + NL +
     '#if defined(__SWITCH__)' + NL +
     '// One byte per 4 KB page of the 512 MB physical arena, set by every guest store there and read by' + NL +
     '// SharedMemory before an upload (switch/host/memory_nx.cpp). Replaces the page-fault watches.' + NL +
     'extern "C" volatile u8 rex_nx_dirty[0x20000];' + NL + 'extern "C" volatile u8 rex_nx_dirty_coarse[0x200];' + NL +
     'static inline __attribute__((always_inline)) void rex_nx_mark(u32 a) {' + NL +
     '  u32 f = a + (u32)rex_nx_host_offset(a);' + NL +
     '  if (f >= 0xA0000000u) {' + NL +
     '    rex_nx_dirty[(f - 0xA0000000u) >> 12] = 1;' + NL +
     '    rex_nx_dirty_coarse[(f - 0xA0000000u) >> 20] = 1;' + NL + '  }' + NL + '}' + NL +
     '#define REX_NX_MARK(x) rex_nx_mark((u32)(x))' + NL +
     '#else' + NL +
     '#define REX_NX_MARK(x) ((void)0)' + NL +
     '#endif' + NL +
     '// Address of a vector store (stvx and friends): marks the page on the Switch.' + NL +
     '#define REX_RAW_STORE_ADDR(x) (REX_NX_MARK(x), REX_RAW_ADDR(x))' + NL),
    ('#define REX_STORE_U8(x, y)  (*(volatile u8*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = (y))',
     '#define REX_STORE_U8(x, y)  (REX_NX_MARK(x), *(volatile u8*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = (y))'),
    ('#define REX_STORE_U16(x, y) (*(volatile u16*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap16(y))',
     '#define REX_STORE_U16(x, y) (REX_NX_MARK(x), *(volatile u16*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap16(y))'),
    ('#define REX_STORE_U32(x, y) (*(volatile u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap32(y))',
     '#define REX_STORE_U32(x, y) (REX_NX_MARK(x), *(volatile u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap32(y))'),
    ('#define REX_STORE_U64(x, y) (*(volatile u64*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap64(y))',
     '#define REX_STORE_U64(x, y) (REX_NX_MARK(x), *(volatile u64*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap64(y))'),
])
# The MMIO-checked stores fall back to the plain host store: mark there too.
tpl = os.path.join(SDK, 'resources/templates/codegen/pch_h.inja')
t = open(tpl, encoding='utf-8').read()
import re as _re
t2 = _re.sub(r'(\s)\*\(volatile (u8|u16|u32|u64)\*\)\(base \+ \(_mmio_addr\)',
             lambda m: m.group(1) + 'REX_NX_MARK(_mmio_addr), *(volatile ' + m.group(2) + '*)(base + (_mmio_addr)', t)
if t2 != t:
    open(tpl, 'w', encoding='utf-8', newline=NL).write(t2)
    changed.append('resources/templates/codegen/pch_h.inja (MM stores)')
# Vector stores: the builders emit simde stores to REX_RAW_ADDR(ea).
vec = os.path.join(SDK, 'src/codegen/builders/vector.cpp')
v = open(vec, encoding='utf-8').read()
v2 = v.replace('_si128((simde__m128i*)REX_RAW_ADDR(', '_si128((simde__m128i*)REX_RAW_STORE_ADDR(')
# Only stores: undo the replacement inside loads (simde_mm_load*).
v2 = v2.replace('simde_mm_load_si128((simde__m128i*)REX_RAW_STORE_ADDR(', 'simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(')
v2 = v2.replace('simde_mm_loadu_si128((simde__m128i*)REX_RAW_STORE_ADDR(', 'simde_mm_loadu_si128((simde__m128i*)REX_RAW_ADDR(')
if v2 != v:
    open(vec, 'w', encoding='utf-8', newline=NL).write(v2)
    changed.append('src/codegen/builders/vector.cpp')
edit('src/graphics/shared_memory.cpp', [
    ('#include <rex/memory.h>' + NL,
     '#include <rex/memory.h>' + NL + '#if REX_PLATFORM_SWITCH' + NL + 'namespace rex::memory::nx {' + NL +
     'void FlushDirtyPages(rex::memory::Memory& memory);' + NL + '}' + NL + '#endif' + NL),
    ('bool SharedMemory::RequestRanges(const std::pair<uint32_t, uint32_t>* ranges, size_t count) {' + NL +
     '  if (ranges == nullptr || !count) {' + NL + '    return true;' + NL + '  }' + NL,
     'bool SharedMemory::RequestRanges(const std::pair<uint32_t, uint32_t>* ranges, size_t count) {' + NL +
     '  if (ranges == nullptr || !count) {' + NL + '    return true;' + NL + '  }' + NL +
     '#if REX_PLATFORM_SWITCH' + NL + '  // No page-fault watches on Horizon: fold the guest stores in now.' + NL +
     '  rex::memory::nx::FlushDirtyPages(memory_);' + NL + '#endif' + NL),
])

# MMIO-checked stores, vector stores (stvx), reservation stores (stwcx/stdcx) and dcbz: mark too.
mm = []
for ty in ('u8', 'u16', 'u32', 'u64'):
    mm.append(('*(volatile ' + ty + '*)(base + _mmio_addr + REX_PHYS_HOST_OFFSET(_mmio_addr))',
               'REX_NX_MARK(_mmio_addr), *(volatile ' + ty + '*)(base + _mmio_addr + REX_PHYS_HOST_OFFSET(_mmio_addr))'))
edit('resources/templates/codegen/pch_h.inja', mm)
edit('src/codegen/builders/memory.cpp', [
    ('simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR({}), "', 'simde_mm_store_si128((simde__m128i*)REX_RAW_STORE_ADDR({}), "'),
])
edit('src/codegen/builders/helpers.h', [
    ('__sync_bool_compare_and_swap(reinterpret_cast<{}*>(REX_RAW_ADDR({})), ',
     '__sync_bool_compare_and_swap(reinterpret_cast<{}*>(REX_RAW_STORE_ADDR({})), '),
])
edit('src/codegen/builders/system.cpp', [
    ('memset((void*)REX_RAW_ADDR({}), 0, 32);', 'memset((void*)REX_RAW_STORE_ADDR({}), 0, 32);'),
    # dcbzl (128 bytes, the guest memset's zero fill): it had no mark, a texture cleared by memset stayed stale on the GPU.
    ('memset((void*)REX_RAW_ADDR({}), 0, 128);', 'memset((void*)REX_RAW_STORE_ADDR({}), 0, 128);'),
])
print('changed (3):', *changed, sep=NL + '  ')

# --- Corrections after the advisor review (0x7F000000 is the GPU writeback window = physical 0-16 MB,
# not its own block; stores mark after writing; IPC must never see guest memory) ---------------------
edit('include/rex/system/xmemory.h', [
    ('    } else if (guest_address >= 0x90000000u && guest_address < 0xA0000000u) {' + NL +
     '      guest_address -= 0x10000000u;' + NL + '    }' + NL + '#endif' + NL,
     '    } else if (guest_address >= 0x90000000u && guest_address < 0xA0000000u) {' + NL +
     '      guest_address -= 0x10000000u;' + NL +
     '    } else if (guest_address >= 0x7F000000u && guest_address < 0x80000000u) {' + NL +
     '      // The GPU writeback window: physical 0-16 MB.' + NL +
     '      guest_address += 0x21000000u;' + NL + '    }' + NL + '#endif' + NL),
])
edit('resources/templates/codegen/pch_h.inja', [
    ('static inline __attribute__((always_inline)) int64_t rex_nx_host_offset(u32 a) {' + NL +
     '  return a >= 0xE0000000u ? -0x40000000ll + 0x1000 : a >= 0xC0000000u ? -0x20000000ll' + NL +
     '       : (a >= 0x90000000u && a < 0xA0000000u) ? -0x10000000ll : 0;' + NL + '}' + NL,
     'static inline __attribute__((always_inline)) int64_t rex_nx_host_offset(u32 a) {' + NL +
     '  if (a >= 0xE0000000u) return -0x40000000ll + 0x1000;' + NL +
     '  if (a >= 0xC0000000u) return -0x20000000ll;' + NL +
     '  if (a >= 0x90000000u && a < 0xA0000000u) return -0x10000000ll;' + NL +
     '  if (a >= 0x7F000000u && a < 0x80000000u) return 0x21000000ll;  // GPU writeback window: physical 0-16 MB' + NL +
     '  return 0;' + NL + '}' + NL),
    ('extern "C" volatile u8 rex_nx_dirty[0x20000];' + NL + 'static inline __attribute__((always_inline)) void rex_nx_mark(u32 a) {' + NL +
     '  u32 f = a + (u32)rex_nx_host_offset(a);' + NL +
     '  if (f >= 0xA0000000u) rex_nx_dirty[(f - 0xA0000000u) >> 12] = 1;' + NL + '}' + NL,
     'extern "C" volatile u8 rex_nx_dirty[0x20000];' + NL + 'extern "C" volatile u8 rex_nx_dirty_coarse[0x200];' + NL +
     'static inline __attribute__((always_inline)) void rex_nx_mark(u32 a) {' + NL +
     '  u32 f = a + (u32)rex_nx_host_offset(a);' + NL +
     '  if (f >= 0xA0000000u) {' + NL +
     '    rex_nx_dirty[(f - 0xA0000000u) >> 12] = 1;' + NL +
     '    rex_nx_dirty_coarse[(f - 0xA0000000u) >> 20] = 1;' + NL + '  }' + NL + '}' + NL),
    ('#define REX_STORE_U8(x, y)  (REX_NX_MARK(x), *(volatile u8*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = (y))',
     '#define REX_STORE_U8(x, y)  ((*(volatile u8*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = (y)), REX_NX_MARK(x))'),
    ('#define REX_STORE_U16(x, y) (REX_NX_MARK(x), *(volatile u16*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap16(y))',
     '#define REX_STORE_U16(x, y) ((*(volatile u16*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap16(y)), REX_NX_MARK(x))'),
    ('#define REX_STORE_U32(x, y) (REX_NX_MARK(x), *(volatile u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap32(y))',
     '#define REX_STORE_U32(x, y) ((*(volatile u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap32(y)), REX_NX_MARK(x))'),
    ('#define REX_STORE_U64(x, y) (REX_NX_MARK(x), *(volatile u64*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap64(y))',
     '#define REX_STORE_U64(x, y) ((*(volatile u64*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap64(y)), REX_NX_MARK(x))'),
])
edit('src/system/xfile.cpp', [
    ('#include <rex/system/xfile.h>' + NL,
     '#include <rex/system/xfile.h>' + NL + '#include <cstring>' + NL + '#include <vector>' + NL +
     '#if REX_PLATFORM_SWITCH' + NL +
     '// GPU dirty-page tracking of host writes into guest memory (switch/host/memory_nx.cpp).' + NL +
     'extern "C" void rex_nx_mark_range(uint32_t guest, uint32_t length);' + NL + '#endif' + NL),
    ('          result = file_->ReadSync(' + NL +
     '              std::span<uint8_t>(' + NL +
     '                  buffer_physical_heap' + NL +
     '                      ? memory()->TranslatePhysical(' + NL +
     '                            buffer_physical_heap->GetPhysicalAddress(buffer_guest_address))' + NL +
     '                      : memory()->TranslateVirtual(buffer_guest_address),' + NL +
     '                  buffer_length),' + NL +
     '              size_t(byte_offset), &bytes_read);' + NL,
     '          uint8_t* guest_buffer =' + NL +
     '              buffer_physical_heap' + NL +
     '                  ? memory()->TranslatePhysical(' + NL +
     '                        buffer_physical_heap->GetPhysicalAddress(buffer_guest_address))' + NL +
     '                  : memory()->TranslateVirtual(buffer_guest_address);' + NL +
     '#if REX_PLATFORM_SWITCH' + NL +
     '          // Horizon IPC must never see guest memory (code-memory mappings jam the fs service):' + NL +
     '          // bounce through the heap.' + NL +
     '          std::vector<uint8_t> nx_bounce(buffer_length);' + NL +
     '          result = file_->ReadSync(std::span<uint8_t>(nx_bounce.data(), buffer_length),' + NL +
     '                                   size_t(byte_offset), &bytes_read);' + NL +
     '          if (XSUCCEEDED(result) && bytes_read) {' + NL +
     '            std::memcpy(guest_buffer, nx_bounce.data(), bytes_read);' + NL +
     '            rex_nx_mark_range(buffer_guest_address, uint32_t(bytes_read));' + NL + '          }' + NL +
     '#else' + NL +
     '          result = file_->ReadSync(std::span<uint8_t>(guest_buffer, buffer_length),' + NL +
     '                                   size_t(byte_offset), &bytes_read);' + NL +
     '#endif' + NL),
    ('  X_STATUS result = file_->WriteSync(' + NL +
     '      std::span<const uint8_t>(memory()->TranslateVirtual(buffer_guest_address), buffer_length),' + NL +
     '      size_t(byte_offset), &bytes_written);' + NL,
     '#if REX_PLATFORM_SWITCH' + NL +
     '  std::vector<uint8_t> nx_bounce(memory()->TranslateVirtual(buffer_guest_address),' + NL +
     '                                 memory()->TranslateVirtual(buffer_guest_address) + buffer_length);' + NL +
     '  X_STATUS result = file_->WriteSync(std::span<const uint8_t>(nx_bounce.data(), buffer_length),' + NL +
     '                                     size_t(byte_offset), &bytes_written);' + NL +
     '#else' + NL +
     '  X_STATUS result = file_->WriteSync(' + NL +
     '      std::span<const uint8_t>(memory()->TranslateVirtual(buffer_guest_address), buffer_length),' + NL +
     '      size_t(byte_offset), &bytes_written);' + NL +
     '#endif' + NL),
])
print('changed (4):', *changed, sep=NL + '  ')

# The instance functions of VK_NN_vi_surface were declared but never loaded: vkCreateViSurfaceNN stayed
# null and the presenter called it (crash at pc 0 after "[nx] window 1280x720", 2026-10-21).
edit('src/ui/vulkan/vulkan_instance.cpp', [
    ('#ifdef VK_USE_PLATFORM_WAYLAND_KHR' + NL + '  if (vulkan_instance->extensions_.ext_KHR_wayland_surface) {' + NL +
     '#include <rex/ui/vulkan/functions/instance_khr_wayland_surface.inc>' + NL + '  }' + NL + '#endif' + NL,
     '#ifdef VK_USE_PLATFORM_WAYLAND_KHR' + NL + '  if (vulkan_instance->extensions_.ext_KHR_wayland_surface) {' + NL +
     '#include <rex/ui/vulkan/functions/instance_khr_wayland_surface.inc>' + NL + '  }' + NL + '#endif' + NL +
     '#ifdef VK_USE_PLATFORM_VI_NN' + NL + '  if (vulkan_instance->extensions_.ext_NN_vi_surface) {' + NL +
     '#include <rex/ui/vulkan/functions/instance_nn_vi_surface.inc>' + NL + '  }' + NL + '#endif' + NL),
])
print('changed (5):', *changed, sep=NL + '  ')

# Diagnostics for the pc=0 crash (2026-10-21): what the presenter is about to call.
edit('src/ui/presenter.cpp', [
    ('#include <rex/ui/presenter.h>' + NL, '#include <rex/ui/presenter.h>' + NL + '#include <cstdio>' + NL),
    ('      bool is_vsync_implicit = false;' + NL +
     '      SurfacePaintConnectResult connect_result = ConnectOrReconnectPaintingToSurfaceFromUIThread(' + NL,
     '      bool is_vsync_implicit = false;' + NL +
     '#if REX_PLATFORM_SWITCH' + NL +
     '      std::printf("[nx] presenter %p vptr %p slot4 %p surface %p type %d %ux%u' + BS + 'n", (void*)this,' + NL +
     '                  *(void**)this, (*(void***)this)[4], (void*)surface_, int(surface_->GetType()),' + NL +
     '                  surface_width, surface_height);' + NL +
     '      std::fflush(stdout);' + NL + '#endif' + NL +
     '      SurfacePaintConnectResult connect_result = ConnectOrReconnectPaintingToSurfaceFromUIThread(' + NL),
])
edit('src/ui/vulkan/vulkan_presenter.cpp', [
    ('#include <rex/ui/vulkan/presenter.h>' + NL, '#include <rex/ui/vulkan/presenter.h>' + NL + '#include <cstdio>' + NL),
])
print('changed (6):', *changed, sep=NL + '  ')

# --- The libnx pad driver (switch/host/input) ------------------------------------------------------
edit('src/input/CMakeLists.txt', [
    ('elseif(NOT NINTENDO_SWITCH)' + NL + '    target_sources(rexinput PRIVATE sdl/sdl_input_driver.cpp)' + NL + 'endif()',
     'elseif(NOT NINTENDO_SWITCH)' + NL + '    target_sources(rexinput PRIVATE sdl/sdl_input_driver.cpp)' + NL +
     'else()' + NL + '    target_sources(rexinput PRIVATE ${REXGLUE_SWITCH_INPUT_SOURCES})' + NL +
     '    target_include_directories(rexinput PUBLIC ${REXGLUE_SWITCH_INCLUDE_DIR})' + NL + 'endif()'),
])
edit('src/input/input_system.cpp', [
    ('#if !REX_PLATFORM_SWITCH' + NL + '#include <rex/input/sdl/sdl_input_driver.h>' + NL + '#endif' + NL,
     '#if REX_PLATFORM_SWITCH' + NL + '#include <rex/input/nx/nx_input_driver.h>' + NL + '#else' + NL +
     '#include <rex/input/sdl/sdl_input_driver.h>' + NL + '#endif' + NL),
    ('    if (REXCVAR_GET(input_backend) == "sdl") {' + NL + '      auto sdl_driver',
     '#if REX_PLATFORM_SWITCH' + NL + '    {' + NL +
     '      auto nx_driver = std::make_unique<nx::NXInputDriver>(nullptr, 0);' + NL +
     '      if (nx_driver->Setup() == X_STATUS_SUCCESS) {' + NL +
     '        input->AddDriver(std::move(nx_driver));' + NL + '      }' + NL + '    }' + NL +
     '#else' + NL + '    if (REXCVAR_GET(input_backend) == "sdl") {' + NL + '      auto sdl_driver'),
])
print('changed (7):', *changed, sep=NL + '  ')

# --- A VkPipelineCache on the SD card (Switch only): NVK compiles a pipeline in ~60 ms, and the SDK
# creates every pipeline from scratch at each run (no vkCreatePipelineCache anywhere). --------------
edit('include/rex/ui/vulkan/functions/device_1_0.inc', [
    ('XE_UI_VULKAN_FUNCTION(vkCreateGraphicsPipelines)' + NL,
     'XE_UI_VULKAN_FUNCTION(vkCreateGraphicsPipelines)' + NL +
     'XE_UI_VULKAN_FUNCTION(vkCreatePipelineCache)' + NL +
     'XE_UI_VULKAN_FUNCTION(vkDestroyPipelineCache)' + NL +
     'XE_UI_VULKAN_FUNCTION(vkGetPipelineCacheData)' + NL),
])
edit('src/graphics/vulkan/pipeline_cache.cpp', [
    ('#include <rex/graphics/vulkan/pipeline_cache.h>' + NL,
     '#include <rex/graphics/vulkan/pipeline_cache.h>' + NL +
     '#if REX_PLATFORM_SWITCH' + NL +
     '#include <cstdio>' + NL + '#include <mutex>' + NL + '#include <vector>' + NL +
     'namespace {' + NL +
     '// Persistent VkPipelineCache: sdmc:/switch/gh/user/vkpipecache.bin, written after every 16 new' + NL +
     '// pipelines (HOME -> close may skip Shutdown) and at Shutdown; .new then rename, so a crash mid-write' + NL +
     '// keeps the old file.' + NL +
     'const char* const kNxPipelineCachePath = "sdmc:/switch/gh/user/vkpipecache.bin";' + NL +
     'VkPipelineCache nx_pipeline_cache = VK_NULL_HANDLE;' + NL +
     'std::mutex nx_pipeline_cache_mutex;' + NL +
     'uint32_t nx_pipelines_since_save = 0;' + NL +
     'void NxOpenPipelineCache(const rex::ui::vulkan::VulkanDevice* dev) {' + NL +
     '  std::vector<uint8_t> data;' + NL +
     '  if (FILE* f = std::fopen(kNxPipelineCachePath, "rb")) {' + NL +
     '    std::fseek(f, 0, SEEK_END);' + NL + '    long n = std::ftell(f);' + NL +
     '    if (n > 0) { data.resize(size_t(n)); std::fseek(f, 0, SEEK_SET); if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear(); }' + NL +
     '    std::fclose(f);' + NL + '  }' + NL +
     '  VkPipelineCacheCreateInfo info{};' + NL +
     '  info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;' + NL +
     '  info.initialDataSize = data.size();' + NL + '  info.pInitialData = data.empty() ? nullptr : data.data();' + NL +
     '  if (dev->functions().vkCreatePipelineCache(dev->device(), &info, nullptr, &nx_pipeline_cache) != VK_SUCCESS) {' + NL +
     '    nx_pipeline_cache = VK_NULL_HANDLE;' + NL + '  }' + NL +
     '  std::printf("[nx] pipeline cache: %zu bytes read, handle %p' + chr(92) + 'n", data.size(), (void*)nx_pipeline_cache);' + NL +
     '  std::fflush(stdout);' + NL + '}' + NL +
     'void NxSavePipelineCache(const rex::ui::vulkan::VulkanDevice* dev) {' + NL +
     '  if (nx_pipeline_cache == VK_NULL_HANDLE) return;' + NL +
     '  std::lock_guard<std::mutex> lock(nx_pipeline_cache_mutex);' + NL +
     '  size_t size = 0;' + NL +
     '  if (dev->functions().vkGetPipelineCacheData(dev->device(), nx_pipeline_cache, &size, nullptr) != VK_SUCCESS || !size) return;' + NL +
     '  std::vector<uint8_t> data(size);' + NL +
     '  if (dev->functions().vkGetPipelineCacheData(dev->device(), nx_pipeline_cache, &size, data.data()) != VK_SUCCESS) return;' + NL +
     '  std::string tmp = std::string(kNxPipelineCachePath) + ".new";' + NL +
     '  if (FILE* f = std::fopen(tmp.c_str(), "wb")) {' + NL +
     '    bool ok = std::fwrite(data.data(), 1, size, f) == size;' + NL + '    std::fclose(f);' + NL +
     '    if (ok) { std::remove(kNxPipelineCachePath); std::rename(tmp.c_str(), kNxPipelineCachePath); }' + NL +
     '  }' + NL +
     '  std::printf("[nx] pipeline cache: %zu bytes written' + chr(92) + 'n", size);' + NL + '  std::fflush(stdout);' + NL +
     '}' + NL + '}  // namespace' + NL + '#endif' + NL),
    ('bool VulkanPipelineCache::Initialize() {' + NL +
     '  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();' + NL,
     'bool VulkanPipelineCache::Initialize() {' + NL +
     '  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();' + NL +
     '#if REX_PLATFORM_SWITCH' + NL + '  NxOpenPipelineCache(vulkan_device);' + NL + '#endif' + NL),
    ('void VulkanPipelineCache::Shutdown() {' + NL,
     'void VulkanPipelineCache::Shutdown() {' + NL +
     '#if REX_PLATFORM_SWITCH' + NL +
     '  if (nx_pipeline_cache != VK_NULL_HANDLE) {' + NL +
     '    const ui::vulkan::VulkanDevice* const nx_dev = command_processor_.GetVulkanDevice();' + NL +
     '    NxSavePipelineCache(nx_dev);' + NL +
     '    nx_dev->functions().vkDestroyPipelineCache(nx_dev->device(), nx_pipeline_cache, nullptr);' + NL +
     '    nx_pipeline_cache = VK_NULL_HANDLE;' + NL + '  }' + NL + '#endif' + NL),
    ('  VkResult create_result = dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,' + NL +
     '                                                         &pipeline_create_info, nullptr, &pipeline);' + NL,
     '#if REX_PLATFORM_SWITCH' + NL +
     '  VkResult create_result = dfn.vkCreateGraphicsPipelines(device, nx_pipeline_cache, 1,' + NL +
     '                                                         &pipeline_create_info, nullptr, &pipeline);' + NL +
     '  if (create_result == VK_SUCCESS && ++nx_pipelines_since_save >= 16) {' + NL +
     '    nx_pipelines_since_save = 0;' + NL + '    NxSavePipelineCache(vulkan_device);' + NL + '  }' + NL +
     '#else' + NL +
     '  VkResult create_result = dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,' + NL +
     '                                                         &pipeline_create_info, nullptr, &pipeline);' + NL +
     '#endif' + NL),
])
print('changed (8):', *changed, sep=NL + '  ')

# --- Step marks in Runtime::Setup / LoadModule / LaunchModule (Switch only, flushed) ----------------
edit('src/system/runtime.cpp', [
    ('#include <rex/system/runtime.h>' + NL, '#include <rex/system/runtime.h>' + NL + '#include <cstdio>' + NL),
    ('  if (!memory_->Initialize()) {' + NL,
     '#if REX_PLATFORM_SWITCH\n  std::printf("[nx] runtime: memory init\\n");\n  std::fflush(stdout);\n#endif\n' + '  if (!memory_->Initialize()) {' + NL),
    ('  file_system_ = std::make_unique<rex::filesystem::VirtualFileSystem>();' + NL,
     '#if REX_PLATFORM_SWITCH\n  std::printf("[nx] runtime: memory ok, VFS and kernel state\\n");\n  std::fflush(stdout);\n#endif\n' + '  file_system_ = std::make_unique<rex::filesystem::VirtualFileSystem>();' + NL),
    ('  REXSYS_DEBUG("Runtime initialized successfully");' + NL,
     '#if REX_PLATFORM_SWITCH\n  std::printf("[nx] runtime: runtime initialized (input, audio, VFS, GPU done)\\n");\n  std::fflush(stdout);\n#endif\n' + '  REXSYS_DEBUG("Runtime initialized successfully");' + NL),
    ('  REXSYS_DEBUG("Loading XEX image: {}", std::string(module_path));' + NL,
     '#if REX_PLATFORM_SWITCH\n  std::printf("[nx] runtime: loading the XEX\\n");\n  std::fflush(stdout);\n#endif\n' + '  REXSYS_DEBUG("Loading XEX image: {}", std::string(module_path));' + NL),
    ('  REXSYS_DEBUG("  XEX image loaded successfully");' + NL,
     '#if REX_PLATFORM_SWITCH\n  std::printf("[nx] runtime: XEX loaded\\n");\n  std::fflush(stdout);\n#endif\n' + '  REXSYS_DEBUG("  XEX image loaded successfully");' + NL),
    ('system::object_ref<system::XThread> Runtime::LaunchModule() {' + NL,
     'system::object_ref<system::XThread> Runtime::LaunchModule() {' + NL + '#if REX_PLATFORM_SWITCH\n  std::printf("[nx] runtime: launching the module (guest main thread)\\n");\n  std::fflush(stdout);\n#endif\n'),
])
print('changed (9):', *changed, sep=NL + '  ')

# --- More step marks: kernel state, input, audio (the XMA decoder brings FFmpeg up), VFS ----------
edit('src/system/runtime.cpp', [
    ('  kernel_state_ = std::make_unique<system::KernelState>(this);' + NL,
     '#if REX_PLATFORM_SWITCH\n  std::printf("[nx] runtime: kernel state\\n");\n  std::fflush(stdout);\n#endif\n' + '  kernel_state_ = std::make_unique<system::KernelState>(this);' + NL),
    ('      X_STATUS input_status = input_system_->Setup();' + NL,
     '#if REX_PLATFORM_SWITCH\n  std::printf("[nx] runtime: input setup\\n");\n  std::fflush(stdout);\n#endif\n' + '      X_STATUS input_status = input_system_->Setup();' + NL),
    ('      X_STATUS audio_status = audio_system_->Setup(kernel_state_.get());' + NL,
     '#if REX_PLATFORM_SWITCH\n  std::printf("[nx] runtime: audio setup (XMA decoder, FFmpeg)\\n");\n  std::fflush(stdout);\n#endif\n' + '      X_STATUS audio_status = audio_system_->Setup(kernel_state_.get());' + NL),
    ('    return fail(X_STATUS_UNSUCCESSFUL, "VFS setup failed");' + NL,
     '    return fail(X_STATUS_UNSUCCESSFUL, "VFS setup failed");' + NL + '  }' + NL +
     '#if REX_PLATFORM_SWITCH\n  std::printf("[nx] runtime: VFS ok\\n");\n  std::fflush(stdout);\n#endif\n' + '  {' + NL),
])
print('changed (10):', *changed, sep=NL + '  ')

# --- Step marks in the XMA decoder setup (the Eden run died inside the audio setup) ------------------
edit('src/audio/xma_decoder.cpp', [
    ('#include <rex/audio/xma/decoder.h>' + NL, '#include <rex/audio/xma/decoder.h>' + NL + '#include <cstdio>' + NL),
    ('  // Setup XMA contexts.' + NL, '#if REX_PLATFORM_SWITCH\n  std::printf("[nx] audio: XMA context data allocated, setting up the contexts (FFmpeg)\\n");\n  std::fflush(stdout);\n#endif\n' + '  // Setup XMA contexts.' + NL),
    ('  context_bitmap_.Resize(kContextCount);' + NL, '#if REX_PLATFORM_SWITCH\n  std::printf("[nx] audio: contexts ready, creating the decoder worker thread\\n");\n  std::fflush(stdout);\n#endif\n' + '  context_bitmap_.Resize(kContextCount);' + NL),
])
edit('src/audio/audio_system.cpp', [
    ('#include <rex/audio/xma/decoder.h>' + NL, '#include <rex/audio/xma/decoder.h>' + NL + '#include <cstdio>' + NL),
    ('  worker_running_ = true;' + NL + '  worker_thread_ = system::object_ref<system::XHostThread>(' + NL,
     '#if REX_PLATFORM_SWITCH\n  std::printf("[nx] audio: XMA decoder up, creating the audio worker thread\\n");\n  std::fflush(stdout);\n#endif\n' + '  worker_running_ = true;' + NL + '  worker_thread_ = system::object_ref<system::XHostThread>(' + NL),
    ('  worker_thread_->set_name("Audio Worker");' + NL + '  worker_thread_->Create();' + NL,
     '  worker_thread_->set_name("Audio Worker");' + NL + '  worker_thread_->Create();' + NL + '#if REX_PLATFORM_SWITCH\n  std::printf("[nx] audio: audio worker thread created\\n");\n  std::fflush(stdout);\n#endif\n'),
])
print('changed (11):', *changed, sep=NL + '  ')

# --- std::filesystem::absolute turns "sdmc:/x" into "/sdmc:/x" on newlib and status() then throws
# (Eden run 0.41: "filesystem error: status: error [/sdmc:/switch/gh/game]"). Keep sdmc paths as given.
edit('src/system/runtime.cpp', [
    ('  auto abs_game_root = std::filesystem::absolute(game_data_root_);' + NL,
     '#if REX_PLATFORM_SWITCH' + NL + '  auto abs_game_root = game_data_root_;  // sdmc:/... is absolute already' + NL +
     '#else' + NL + '  auto abs_game_root = std::filesystem::absolute(game_data_root_);' + NL + '#endif' + NL),
    ('    auto abs_update_root = std::filesystem::absolute(update_data_root_);' + NL,
     '#if REX_PLATFORM_SWITCH' + NL + '    auto abs_update_root = update_data_root_;' + NL +
     '#else' + NL + '    auto abs_update_root = std::filesystem::absolute(update_data_root_);' + NL + '#endif' + NL),
])
edit('src/system/kernel_state.cpp', [
    ('    user_data_root = std::filesystem::absolute(user_data_root);' + NL,
     '#if !REX_PLATFORM_SWITCH' + NL + '    user_data_root = std::filesystem::absolute(user_data_root);' + NL + '#endif' + NL),
])
print('changed (12):', *changed, sep=NL + '  ')

# --- Null GPU backend (--gpu_plugin=xenos --gpu_backend=null): the command processor runs, nothing is
# drawn. Eden runs (no NVK there) reach the guest's frames with it; the swap count is the frame counter.
write('include/rex/graphics/null/graphics_system.h', '#pragma once\n// Null GPU backend: the Xenos command processor runs (ring buffer consumed, read pointer written\n// back, vblank interrupts fired, swaps counted), nothing is drawn. For emulator runs (Eden cannot\n// run NVK) and for profiling the guest without a renderer. cvhd-recomp, BSD-3 like the SDK.\n#include <memory>\n\n#include <rex/graphics/command_processor.h>\n#include <rex/graphics/graphics_system.h>\n\nnamespace rex::graphics::null {\n\nclass NullGraphicsSystem : public GraphicsSystem {\n public:\n  NullGraphicsSystem() = default;\n  ~NullGraphicsSystem() override = default;\n\n  std::string name() const override { return "Null"; }\n  // No provider, no presenter: the window stays without a presenter (detached mode\'s window).\n  X_STATUS SetupPresentation(::rex::ui::WindowedAppContext*) override { return X_STATUS_SUCCESS; }\n\n protected:\n  void CreateProvider(bool) override {}\n\n private:\n  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;\n};\n\n}  // namespace rex::graphics::null\n')
write('src/graphics/null/graphics_system.cpp', '#include <rex/graphics/null/graphics_system.h>\n\n#include <cstdio>\n\n#include <rex/logging.h>\n\nnamespace rex::graphics::null {\n\nclass NullCommandProcessor : public CommandProcessor {\n public:\n  using CommandProcessor::CommandProcessor;\n\n protected:\n  bool SetupContext() override { return true; }\n  void ShutdownContext() override {}\n  // The first register writes show that the guest\'s MMIO stores reach the command processor\n  // (CP_RB_WPTR is the one that wakes it); after that the swap count is the frame counter.\n  void WriteRegister(uint32_t index, uint32_t value) override {\n    if (register_writes_ < 5) {\n      register_writes_++;\n      std::printf("[nx] gpu: register write #%u: index 0x%X value 0x%08X" "%s", register_writes_, index,\n                  value, "\\n");\n      std::fflush(stdout);\n    }\n    CommandProcessor::WriteRegister(index, value);\n  }\n  void IssueSwap(uint32_t, uint32_t, uint32_t) override {\n    swaps_++;\n    if (swaps_ <= 3 || swaps_ % 60 == 0) {\n      std::printf("[nx] gpu: swap #%u%s", swaps_, "\\n");\n      std::fflush(stdout);\n    }\n  }\n  Shader* LoadShader(xenos::ShaderType, uint32_t, const uint32_t*, uint32_t) override {\n    return nullptr;\n  }\n  bool IssueDraw(xenos::PrimitiveType, uint32_t, IndexBufferInfo*, bool) override { return true; }\n  bool IssueCopy() override { return true; }\n\n private:\n  uint32_t register_writes_ = 0;\n  uint32_t swaps_ = 0;\n};\n\nstd::unique_ptr<CommandProcessor> NullGraphicsSystem::CreateCommandProcessor() {\n  return std::make_unique<NullCommandProcessor>(this, kernel_state_);\n}\n\n}  // namespace rex::graphics::null\n')
edit('src/graphics/CMakeLists.txt', [
    ('    plugin_main.cpp' + NL, '    plugin_main.cpp' + NL + '    null/graphics_system.cpp' + NL),
])
edit('src/graphics/plugin_main.cpp', [
    ('#include <rex/logging.h>' + NL, '#include <rex/logging.h>' + NL + '#include <rex/graphics/null/graphics_system.h>' + NL),
    ('  std::string_view backend = info->backend ? info->backend : "any";' + NL,
     '  std::string_view backend = info->backend ? info->backend : "any";' + NL +
     '  if (backend == "null") {' + NL + '    return new rex::graphics::null::NullGraphicsSystem();' + NL + '  }' + NL),
])
edit('src/ui/rex_app.cpp', [
    ('    .lifecycle(rex::cvar::Lifecycle::kInitOnly);' + NL,
     '    .lifecycle(rex::cvar::Lifecycle::kInitOnly);' + NL +
     'REXCVAR_DEFINE_STRING(gpu_backend, "any", "GPU",' + NL +
     '                      "Backend inside the GPU plugin: any, vulkan, d3d12, or null (command processor without drawing)")' + NL +
     '    .lifecycle(rex::cvar::Lifecycle::kInitOnly);' + NL),
    ('    config_.graphics = rex::system::LoadGpuPlugin(config_.gpu_plugin);' + NL,
     '    config_.graphics = rex::system::LoadGpuPlugin(config_.gpu_plugin, REXCVAR_GET(gpu_backend));' + NL),
])
print('changed (13):', *changed, sep=NL + '  ')

# --- GuestPtr fold: every kernel export receives its pointer arguments through
# rex::memory::GuestPtr(base, addr) = base + addr + PhysicalHostOffset(addr), which had no Switch branch,
# so a KEVENT at 0xFFCAB000 (the E0 system heap) was read unfolded (Eden 0.48: unmapped reads, no swaps).
# The offset is signed now (the folds subtract). ---------------------------------------------------
edit('include/rex/system/xmemory.h', [
    ('constexpr u32 PhysicalHostOffset([[maybe_unused]] u32 guest_addr) noexcept {' + NL +
     '#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && defined(__aarch64__))' + NL +
     '  return (guest_addr >= 0xE0000000u) ? 0x1000u : 0u;' + NL +
     '#else' + NL + '  return 0u;' + NL + '#endif' + NL + '}' + NL,
     'constexpr long long PhysicalHostOffset([[maybe_unused]] u32 guest_addr) noexcept {' + NL +
     '#if REX_PLATFORM_SWITCH' + NL +
     '  // The same fold as the codegen pch (rex_nx_host_offset) and Memory::TranslateVirtual.' + NL +
     '  if (guest_addr >= 0xE0000000u) return -0x40000000ll + 0x1000;' + NL +
     '  if (guest_addr >= 0xC0000000u) return -0x20000000ll;' + NL +
     '  if (guest_addr >= 0x90000000u && guest_addr < 0xA0000000u) return -0x10000000ll;' + NL +
     '  if (guest_addr >= 0x7F000000u && guest_addr < 0x80000000u) return 0x21000000ll;' + NL +
     '  return 0;' + NL +
     '#elif REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && defined(__aarch64__))' + NL +
     '  return (guest_addr >= 0xE0000000u) ? 0x1000 : 0;' + NL +
     '#else' + NL + '  return 0;' + NL + '#endif' + NL + '}' + NL),
])
print('changed (14):', *changed, sep=NL + '  ')

# --- Vblank / interrupt dispatch marks (Switch only): Eden 0.49 stalled after swap #1 with the vblank
# dispatches stopping 1 s later; these lines show the vsync cadence and whether the guest's interrupt
# callback returns. ----------------------------------------------------------------------------------
edit('src/graphics/graphics_system.cpp', [
    ('#include <rex/graphics/graphics_system.h>' + NL, '#include <rex/graphics/graphics_system.h>' + NL + '#include <cstdio>' + NL),
    ('  uint64_t args[] = {source, interrupt_callback_data_};' + NL +
     '  function_dispatcher_->ExecuteInterrupt(thread->thread_state(), interrupt_callback_, args,' + NL +
     '                                         rex::countof(args));' + NL,
     '  uint64_t args[] = {source, interrupt_callback_data_};' + NL +
     '#if REX_PLATFORM_SWITCH' + NL +
     '  static uint32_t nx_dispatches[2] = {0, 0};' + NL +
     '  const uint32_t nx_n = ++nx_dispatches[source ? 1 : 0];' + NL +
     '  const bool nx_say = nx_n <= 5 || nx_n % 300 == 0;' + NL +
     '  if (nx_say) {' + NL +
     '    std::printf("[nx] gpu: interrupt source %u #%u: dispatching 0x%08X on cpu %u" "%s", source, nx_n,' + NL +
     '                interrupt_callback_, cpu, "' + BS + 'n");' + NL +
     '    std::fflush(stdout);' + NL + '  }' + NL + '#endif' + NL +
     '  function_dispatcher_->ExecuteInterrupt(thread->thread_state(), interrupt_callback_, args,' + NL +
     '                                         rex::countof(args));' + NL +
     '#if REX_PLATFORM_SWITCH' + NL +
     '  if (nx_say) {' + NL +
     '    std::printf("[nx] gpu: interrupt source %u #%u returned" "%s", source, nx_n, "' + BS + 'n");' + NL +
     '    std::fflush(stdout);' + NL + '  }' + NL + '#endif' + NL),
    ('        uint64_t last_frame_time = chrono::Clock::QueryGuestTickCount();' + NL,
     '        uint64_t last_frame_time = chrono::Clock::QueryGuestTickCount();' + NL +
     '#if REX_PLATFORM_SWITCH' + NL +
     '        std::printf("[nx] gpu: vsync worker: %.1f Hz, guest ticks %llu/s, host ticks %llu/s, interval %llu ticks" "%s",' + NL +
     '                    refresh_rate_hz, (unsigned long long)guest_tick_frequency,' + NL +
     '                    (unsigned long long)chrono::Clock::QueryHostTickFrequency(),' + NL +
     '                    (unsigned long long)vsync_interval_ticks, "' + BS + 'n");' + NL +
     '        std::fflush(stdout);' + NL + '#endif' + NL),
])
print('changed (15):', *changed, sep=NL + '  ')

# --- Host clock on the Switch: libnx has no CLOCK_MONOTONIC_RAW (clock_getres fails, the asserts are
# compiled out), so host_tick_frequency_platform() returned 0, the guest tick count never advanced and
# the vsync worker never fired a vblank (Eden 0.51: "host ticks 0/s", swap #1 only). The ARM generic
# counter (armGetSystemTick, 19.2 MHz) is the host clock now. ---------------------------------------
edit('src/core/clock_posix.cpp', [
    ('namespace rex::chrono {' + NL + NL + 'uint64_t Clock::host_tick_frequency_platform() {' + NL + '#ifdef __APPLE__' + NL,
     '#if REX_PLATFORM_SWITCH' + NL + '// The ARM generic counter (libnx armGetSystemTick is an inline): 19.2 MHz on the Tegra X1.' + NL +
     'static inline uint64_t nx_system_tick() {' + NL + '  uint64_t v;' + NL +
     '  __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(v));' + NL + '  return v;' + NL + '}' + NL + '#endif' + NL + NL +
     'namespace rex::chrono {' + NL + NL + 'uint64_t Clock::host_tick_frequency_platform() {' + NL +
     '#if REX_PLATFORM_SWITCH' + NL + '  return 19200000ull;  // armGetSystemTickFreq()' + NL + '#elif defined(__APPLE__)' + NL),
    ('uint64_t Clock::host_tick_count_platform() {' + NL + '#ifdef __APPLE__' + NL,
     'uint64_t Clock::host_tick_count_platform() {' + NL + '#if REX_PLATFORM_SWITCH' + NL + '  return nx_system_tick();' + NL + '#elif defined(__APPLE__)' + NL),
])
print('changed (16):', *changed, sep=NL + '  ')

# --- XThread::SetActiveCpu warns "Too few processor cores" on every call when the host has < 6 cores;
# every vblank dispatch calls it: 60 log lines per second on the Switch (Eden 0.54: 15,796 lines in
# 3 min). The Switch has 3 usable cores: no affinity and no warning. ------------------------------
edit('src/system/xthread.cpp', [
    ('  if (rex::thread::logical_processor_count() >= 6) {' + NL +
     '    if (!REXCVAR_GET(ignore_thread_affinities)) {' + NL +
     '      thread_->set_affinity_mask(uint64_t(1) << cpu_index);' + NL +
     '    }' + NL + '  } else {' + NL +
     '    REXSYS_WARN("Too few processor cores - scheduling will be wonky");' + NL + '  }' + NL,
     '#if REX_PLATFORM_SWITCH' + NL +
     '  // 3 cores for the game (switch_nx.cpp deals the threads over them): no per-thread affinity.' + NL +
     '#else' + NL +
     '  if (rex::thread::logical_processor_count() >= 6) {' + NL +
     '    if (!REXCVAR_GET(ignore_thread_affinities)) {' + NL +
     '      thread_->set_affinity_mask(uint64_t(1) << cpu_index);' + NL +
     '    }' + NL + '  } else {' + NL +
     '    REXSYS_WARN("Too few processor cores - scheduling will be wonky");' + NL + '  }' + NL +
     '#endif' + NL),
])
print('changed (17):', *changed, sep=NL + '  ')

# --- Extra GPU backends from the game tree (gh src/gpu/*: the trace backend on every platform, the
# deko3d backend on the Switch): sources appended to the plugin, and a weak hook plugin_main asks for
# an unknown backend name before failing. Platform-independent edit.
edit('src/graphics/CMakeLists.txt', [
    ('    pipeline/shader/translator_disasm.cpp' + NL + ')' + NL,
     '    pipeline/shader/translator_disasm.cpp' + NL + ')' + NL +
     '# Backends from the game tree (gh): appended by the game CMakeLists before add_subdirectory.' + NL +
     'list(APPEND REXGPU_XENOS_SOURCES ${REXGPU_XENOS_EXTRA_SOURCES})' + NL),
])
edit('src/graphics/plugin_main.cpp', [
    ('extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version(void) {' + NL,
     '// Backends from the game tree (gh src/gpu/trace_backend.cpp) answer names the plugin does not' + NL +
     '// know (nullptr = unknown). A required symbol, not a weak default: in the static plugin the' + NL +
     '// linker would keep a weak default and never pull the game object in (Eden, 2026-10-22).' + NL +
     'extern "C" rex::system::IGraphicsSystem* rex_gpu_extra_backend(const char* name);' + NL + NL +
     'extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version(void) {' + NL),
    ("  REXLOG_ERROR(\"rexgpu-xenos: requested backend '{}' is not compiled into this plugin\", backend);" + NL,
     '  if (rex::system::IGraphicsSystem* extra = rex_gpu_extra_backend(std::string(backend).c_str())) {' + NL +
     '    return extra;' + NL + '  }' + NL +
     "  REXLOG_ERROR(\"rexgpu-xenos: requested backend '{}' is not compiled into this plugin\", backend);" + NL),
])

# --- vkDestroyInstance was taken from vk_icdGetInstanceProcAddr(nullptr, ...), which NVK answers with
# null (instance-level function): every shutdown, also the one after a failed init, ended at pc 0
# (console 0.55, ~VulkanInstance). Load it with the instance handle like the other instance functions.
edit('src/ui/vulkan/vulkan_instance.cpp', [
    ('  // Vulkan 1.0.' + NL + '#include <rex/ui/vulkan/functions/instance_1_0.inc>' + NL,
     '  // Vulkan 1.0.' + NL + '#include <rex/ui/vulkan/functions/instance_1_0.inc>' + NL +
     '#if REX_PLATFORM_SWITCH' + NL +
     '  // The ICD gives instance-level functions only for a real instance (null above, at load time).' + NL +
     '  ifn.vkDestroyInstance = PFN_vkDestroyInstance(' + NL +
     '      ifn.vkGetInstanceProcAddr(vulkan_instance->instance_, "vkDestroyInstance"));' + NL +
     '  functions_loaded &= ifn.vkDestroyInstance != nullptr;' + NL + '#endif' + NL),
])
print('changed (18):', *changed, sep=NL + '  ')

# --- FPS: the perf counters stay on in the Switch Release build, the swap packet prints one line per
# second to gh_log.txt, and the SDK's debug overlay (ImGui: fps, frame-time graph, draws) opens at
# start when the cvar debug_overlay is true (sdmc:/switch/gh/gh.toml: debug_overlay = false). ---
edit('CMakeLists.txt', [
    ('    add_compile_definitions($<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PERF_COUNTERS>)' + NL + 'endif()' + NL,
     '    add_compile_definitions($<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PERF_COUNTERS>)' + NL +
     '    if(NINTENDO_SWITCH)' + NL + '        add_compile_definitions(REXGLUE_ENABLE_PERF_COUNTERS)  # the fps line and overlay on the console' + NL +
     '    endif()' + NL + 'endif()' + NL),
])
edit('src/graphics/command_processor.cpp', [
    ('      PROFILE_FRAME_TIME_US(dt_us);' + NL + '      PROFILE_FPS(freq / (now - last_frame_tick));' + NL + '    }' + NL,
     '      PROFILE_FRAME_TIME_US(dt_us);' + NL + '      PROFILE_FPS(freq / (now - last_frame_tick));' + NL +
     '#if REX_PLATFORM_SWITCH' + NL +
     '      // One line per second in gh_log.txt: fps, mean and worst frame time (swap to swap).' + NL +
     '      static uint64_t nx_report_tick = 0, nx_frames = 0, nx_sum_us = 0, nx_max_us = 0;' + NL +
     '      if (!nx_report_tick) nx_report_tick = now;' + NL +
     '      nx_frames++;' + NL + '      nx_sum_us += uint64_t(dt_us);' + NL +
     '      if (uint64_t(dt_us) > nx_max_us) nx_max_us = uint64_t(dt_us);' + NL +
     '      if (now - nx_report_tick >= freq) {' + NL +
     '        const double secs = double(now - nx_report_tick) / double(freq);' + NL +
     '        std::printf("[nx] fps %.1f, frame %.1f ms mean, %.1f ms max (%llu frames)" "%s", double(nx_frames) / secs,' + NL +
     '                    double(nx_sum_us) / double(nx_frames) / 1000.0, double(nx_max_us) / 1000.0,' + NL +
     '                    (unsigned long long)nx_frames, "' + BS + 'n");' + NL +
     '        nx_report_tick = now;' + NL + '        nx_frames = nx_sum_us = nx_max_us = 0;' + NL + '      }' + NL +
     '#endif' + NL + '    }' + NL),
])
edit('src/ui/rex_app.cpp', [
    ('                      "Backend inside the GPU plugin: any, vulkan, d3d12, or null (command processor without drawing)")' + NL +
     '    .lifecycle(rex::cvar::Lifecycle::kInitOnly);' + NL,
     '                      "Backend inside the GPU plugin: any, vulkan, d3d12, or null (command processor without drawing)")' + NL +
     '    .lifecycle(rex::cvar::Lifecycle::kInitOnly);' + NL +
     '#if REX_PLATFORM_SWITCH' + NL + '#define REX_DEBUG_OVERLAY_DEFAULT true  // on-screen fps (the user wants it); gh.toml debug_overlay = false hides it' + NL + '#else' + NL +
     '#define REX_DEBUG_OVERLAY_DEFAULT false' + NL + '#endif' + NL +
     'REXCVAR_DEFINE_BOOL(debug_overlay, REX_DEBUG_OVERLAY_DEFAULT, "UI",' + NL +
     '                    "Open the debug overlay (fps, frame time, draws) at start");' + NL),
    ('  imgui_drawer_->SetPresenterAndImmediateDrawer(presenter, drawer);' + NL,
     '  imgui_drawer_->SetPresenterAndImmediateDrawer(presenter, drawer);' + NL +
     '  if (presenter && REXCVAR_GET(debug_overlay)) {' + NL +
     '    debug_overlay_ = std::make_unique<ui::DebugOverlayDialog>(imgui_drawer_.get(), frame_stats_provider_);' + NL +
     '  }' + NL),
])
print('changed (19):', *changed, sep=NL + '  ')

# --- Missing-file opens cost 2 s each on the console (25 optional files = 50 s of the boot): a miss in
# the in-memory tree falls back to ListFiles(directory) = opendir + one stat IPC per entry. sdmc: is
# FAT, case-insensitive, so the exact-name stat above already answered; the scan finds nothing new.
edit('src/filesystem/devices/host_path_device.cpp', [
    ('      auto child_infos = rex::filesystem::ListFiles(current_entry->host_path());' + NL +
     '      auto match = std::find_if(child_infos.begin(), child_infos.end(), [&](const auto& info) {' + NL,
     '#if REX_PLATFORM_SWITCH' + NL +
     '      // sdmc: (FAT) is case-insensitive: the exact-name stat was the final answer.' + NL +
     '      return nullptr;' + NL + '#endif' + NL +
     '      auto child_infos = rex::filesystem::ListFiles(current_entry->host_path());' + NL +
     '      auto match = std::find_if(child_infos.begin(), child_infos.end(), [&](const auto& info) {' + NL),
])
# --- Directory opens (the game opens "game:\data\player_dll\" when it leaves the title, to list its
# character DLLs): a trailing separator left an empty file name (STATUS_NO_SUCH_FILE: 2026-10-23), and
# libnx cannot open() a directory; a directory handle only serves enumeration through the entry tree.
edit('src/filesystem/virtual_file_system.cpp', [
    ('X_STATUS VirtualFileSystem::OpenFile(Entry* root_entry, const std::string_view path,' + NL,
     'X_STATUS VirtualFileSystem::OpenFile(Entry* root_entry, const std::string_view path_in,' + NL),
    ('                                     FileAction* out_action) {' + NL +
     "  // TODO(gibbed): should 'is_directory' remain as a bool or should it be" + NL,
     '                                     FileAction* out_action) {' + NL +
     '  // A directory opened with a trailing separator ("data\\player_dll\\"): without this the name' + NL +
     '  // is empty and the open fails.' + NL +
     '  std::string_view path = path_in;' + NL +
     "  while (!path.empty() && (path.back() == '\\\\' || path.back() == '/')) {" + NL +
     '    path.remove_suffix(1);' + NL + '  }' + NL +
     "  // TODO(gibbed): should 'is_directory' remain as a bool or should it be" + NL),
])
edit('src/filesystem/devices/host_path_entry.cpp', [
    ('  auto file_handle = rex::filesystem::FileHandle::OpenExisting(' + NL +
     '      host_path_, desired_access, static_cast<HostPathDevice*>(device_)->allow_share_delete());' + NL,
     '#if REX_PLATFORM_SWITCH' + NL +
     '  if (attributes_ & kFileAttributeDirectory) {' + NL +
     '    // libnx cannot open() a directory; the handle serves enumeration (the entry tree) only.' + NL +
     '    *out_file = new HostPathFile(desired_access, this, nullptr);' + NL +
     '    return X_STATUS_SUCCESS;' + NL + '  }' + NL + '#endif' + NL +
     '  auto file_handle = rex::filesystem::FileHandle::OpenExisting(' + NL +
     '      host_path_, desired_access, static_cast<HostPathDevice*>(device_)->allow_share_delete());' + NL),
])
edit('src/filesystem/devices/host_path_file.cpp', [
    ('  if (file_handle_->Read(byte_offset, buffer.data(), buffer.size(), out_bytes_read)) {' + NL,
     '  if (!file_handle_) {' + NL + '    return X_STATUS_ACCESS_DENIED;  // a directory handle (Switch)' + NL + '  }' + NL +
     '  if (file_handle_->Read(byte_offset, buffer.data(), buffer.size(), out_bytes_read)) {' + NL),
    ('  if (file_handle_->Write(byte_offset, buffer.data(), buffer.size(), out_bytes_written)) {' + NL,
     '  if (!file_handle_) {' + NL + '    return X_STATUS_ACCESS_DENIED;' + NL + '  }' + NL +
     '  if (file_handle_->Write(byte_offset, buffer.data(), buffer.size(), out_bytes_written)) {' + NL),
    ('  if (file_handle_->SetLength(length)) {' + NL,
     '  if (!file_handle_) {' + NL + '    return X_STATUS_ACCESS_DENIED;' + NL + '  }' + NL +
     '  if (file_handle_->SetLength(length)) {' + NL),
])
print('changed (20):', *changed, sep=NL + '  ')

# gh 0.112: the poll sleep is a setting (gpu_wait_us, nx_input_driver.cpp) instead of a fixed millisecond. The first
# edit below migrates a tree that already has the old insert, so the insert itself keeps one text (see edit()).
NX_POLL_SLEEP_OLD = '          rex::thread::Sleep(std::chrono::milliseconds(1));' + NL
NX_POLL_SLEEP = ('          {' + NL +
                 '            extern volatile int rex_nx_gpu_wait_us __asm__("rex_nx_gpu_wait_us");  // nx_input_driver.cpp' + NL +
                 '            const int nx_us = rex_nx_gpu_wait_us;' + NL +
                 '            rex::thread::Sleep(std::chrono::microseconds(nx_us > 0 ? nx_us : 1000));' + NL +
                 '          }' + NL)
NX_SLEPT = '          rex_nx_wait_slept_ticks += rex::chrono::Clock::QueryHostTickCount() - nx_t0;' + NL
edit('src/graphics/command_processor.cpp', [(NX_POLL_SLEEP_OLD + NX_SLEPT, NX_POLL_SLEEP + NX_SLEPT)], must=False)
# The same for the wait time by kind (the [dk] perf line of the deko3d backend).
NX_WAIT_NS = '    if (freq) cp_stats.wait_poll_ns += (rex::chrono::Clock::QueryHostTickCount() - nx_wait_start) * 1000000000ull / freq;' + NL
NX_WAIT_LOG = '    if (nx_log_this && freq) {' + NL
NX_WAIT_KIND_OLD = ('    if (freq) {' + NL +
                '      extern volatile uint64_t rex_nx_wait_kind_ns[4] __asm__("rex_nx_wait_kind_ns");  // switch_nx.cpp' + NL +
                '      const uint32_t nx_low = poll_reg_addr & 0xFF;' + NL +
                '      const int nx_kind = !is_memory ? 0 : nx_low == 0x02 ? 1 : nx_low == 0x06 ? 2 : 3;' + NL +
                '      rex_nx_wait_kind_ns[nx_kind] = rex_nx_wait_kind_ns[nx_kind] +' + NL +
                '          (rex::chrono::Clock::QueryHostTickCount() - nx_wait_start) * 1000000000ull / freq;' + NL +
                '    }' + NL)
# gh 0.118: Switch only (0.112-0.117 had it unguarded: the Linux and Windows libraries did not link or load).
NX_WAIT_KIND = '#if REX_PLATFORM_SWITCH' + NL + NX_WAIT_KIND_OLD + '#endif' + NL
edit('src/graphics/command_processor.cpp', [(NX_WAIT_NS + NX_WAIT_KIND_OLD + NX_WAIT_LOG, NX_WAIT_NS + NX_WAIT_KIND + NX_WAIT_LOG)], must=False)
edit('src/graphics/command_processor.cpp', [(NX_WAIT_NS + NX_WAIT_LOG, NX_WAIT_NS + NX_WAIT_KIND + NX_WAIT_LOG)], must=False)

# --- WAIT_REG_MEM statistics (Switch): how often the command processor polls a guest value and how
# long it sleeps for it (the game's vblank/fence waits go through here). switch_nx.cpp prints them.
edit('src/graphics/command_processor.cpp', [
    ('#include <rex/graphics/command_processor.h>' + NL,
     '#include <rex/graphics/command_processor.h>' + NL +
     '#if REX_PLATFORM_SWITCH' + NL +
     'extern "C" volatile uint64_t rex_nx_wait_polls, rex_nx_wait_sleep_ms, rex_nx_wait_yields, rex_nx_wait_slept_ticks;  // switch_nx.cpp' + NL +
     '#endif' + NL),
    ('          rex::thread::Sleep(std::chrono::milliseconds(wait / 0x100));' + NL,
     '#if REX_PLATFORM_SWITCH' + NL +
     '          rex_nx_wait_polls++;' + NL + '          rex_nx_wait_sleep_ms += wait / 0x100;' + NL +
     '          // The blind sleep per poll capped at 1 ms (the asked value is counted above): the' + NL +
     '          // vblank worker and the guest callback pace the frame, not this sleep.' + NL +
     '          const uint64_t nx_t0 = rex::chrono::Clock::QueryHostTickCount();' + NL +
     NX_POLL_SLEEP +
     '          rex_nx_wait_slept_ticks += rex::chrono::Clock::QueryHostTickCount() - nx_t0;' + NL +
     '#else' + NL +
     '          rex::thread::Sleep(std::chrono::milliseconds(wait / 0x100));' + NL +
     '#endif' + NL),
    ('      } else {' + NL + '        rex::thread::MaybeYield();' + NL + '      }' + NL + '    }' + NL + '  } while (!matched);' + NL,
     '      } else {' + NL +
     '#if REX_PLATFORM_SWITCH' + NL + '        rex_nx_wait_yields++;' + NL + '#endif' + NL +
     '        rex::thread::MaybeYield();' + NL + '      }' + NL + '    }' + NL + '  } while (!matched);' + NL),
])
print('changed (21):', *changed, sep=NL + '  ')

# --- The audio worker on the Switch: 0.86-0.111 made it leave at once (no backend; its loop woke on every
# signal of every wait object, 49% of a core on the console 0.65-0.84). Since 2026-10-23 it stays: the nop
# system below creates a driver that consumes frames at the real rate, so the guest gets its render-driver
# callbacks (the game's attract transition after the title waited on its audio engine forever without them).
edit('src/audio/audio_system.cpp', [
    ('void AudioSystem::WorkerThreadMain() {' + NL + '  // Initialize driver and ringbuffer.' + NL + '  Initialize();' + NL,
     'void AudioSystem::WorkerThreadMain() {' + NL + '  // Initialize driver and ringbuffer.' + NL + '  Initialize();' + NL +
     '#if REX_PLATFORM_SWITCH' + NL + '  std::printf("[nx] audio: audio worker running (frames consumed at 5.33 ms, no device)\\n");' + NL +
     '  std::fflush(stdout);' + NL + '#endif' + NL),
])
# The nop audio system: a driver per client that "plays" the submitted frames at 256 samples / 48 kHz
# (5.33 ms each) by releasing the client's semaphore, instead of refusing to create a driver
# (X_STATUS_NOT_IMPLEMENTED failed XAudioRegisterRenderDriverClient). Written whole: idempotent by content.
write('src/audio/nop/nop_audio_system.cpp', '''/**
 * Nop audio system (cvhd-recomp edit of the ReXGlue SDK file, BSD-3): no device, but the frames
 * the guest submits are consumed at the real rate so its render-driver callbacks keep coming.
 */
#include <rex/audio/flags.h>
#include <rex/audio/nop/nop_audio_system.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <rex/audio/audio_driver.h>
#include <rex/thread.h>

namespace rex::audio::nop {

namespace {
// One XAudio render-driver frame: 256 samples per channel at 48 kHz.
constexpr auto kFrameDuration = std::chrono::microseconds(5333);

class NopAudioDriver : public AudioDriver {
 public:
  NopAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
      : AudioDriver(memory), semaphore_(semaphore), thread_([this] { Run(); }) {}
  ~NopAudioDriver() override {
    running_ = false;
    thread_.join();
  }
  void SubmitFrame(uint32_t /*samples_ptr*/) override { queued_.fetch_add(1); }

 private:
  void Run() {
    while (running_) {
      std::this_thread::sleep_for(kFrameDuration);
      if (queued_.load() > 0) {
        queued_.fetch_sub(1);
        semaphore_->Release(1, nullptr);  // the frame "played": the worker asks the guest for the next
      }
    }
  }
  rex::thread::Semaphore* semaphore_;
  std::atomic<bool> running_{true};
  std::atomic<int> queued_{0};
  std::thread thread_;
};
}  // namespace

std::unique_ptr<AudioSystem> NopAudioSystem::Create(
    runtime::FunctionDispatcher* function_dispatcher) {
  return std::make_unique<NopAudioSystem>(function_dispatcher);
}

NopAudioSystem::NopAudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : AudioSystem(function_dispatcher) {}

NopAudioSystem::~NopAudioSystem() = default;

X_STATUS NopAudioSystem::CreateDriver(size_t /*index*/, rex::thread::Semaphore* semaphore,
                                      AudioDriver** out_driver) {
  *out_driver = new NopAudioDriver(memory(), semaphore);
  return X_STATUS_SUCCESS;
}

void NopAudioSystem::DestroyDriver(AudioDriver* driver) { delete driver; }

}  // namespace rex::audio::nop
''')
print('changed (22):', *changed, sep=NL + '  ')

# --- Command-processor phase timers and statistics once per second, every platform (REXGPU_INFO, so the Linux log and
# the console's gh_rex_log.txt carry the same line): ring kicks (CP_RB_WPTR writes), batches
# executed and dwords consumed, WAIT_REG_MEM packets and their polls, swaps, idle waits. The Linux
# run is the reference for the console (console 0.84: 2.1 fps, the guest waiting for the ring).
edit('src/graphics/command_processor.cpp', [
    ('void CommandProcessor::UpdateWritePointer(uint32_t value) {' + NL + '  write_ptr_index_ = value;' + NL,
     'void CommandProcessor::UpdateWritePointer(uint32_t value) {' + NL +
     '  cp_stats.kicks++;' + NL +
     '  write_ptr_index_ = value;' + NL),
    ('    read_ptr_index_ = ExecutePrimaryBuffer(read_ptr_index_, write_ptr_index);' + NL,
     '    {' + NL +
     '      const uint32_t nx_before = read_ptr_index_;' + NL +
     '      const uint64_t nx_b0 = rex::chrono::Clock::QueryHostTickCount();' + NL +
     '      read_ptr_index_ = ExecutePrimaryBuffer(read_ptr_index_, write_ptr_index);' + NL +
     '      cp_stats.batch_ns += (rex::chrono::Clock::QueryHostTickCount() - nx_b0) * 1000000000ull /' + NL +
     '                           std::max<uint64_t>(1, rex::chrono::Clock::QueryHostTickFrequency());' + NL +
     '      cp_stats.batches++;' + NL +
     '      cp_stats.dwords += (read_ptr_index_ >= nx_before) ? (read_ptr_index_ - nx_before)' + NL +
     '                                                        : (primary_buffer_size_ / 4 - nx_before + read_ptr_index_);' + NL +
     '    }' + NL),
    ('  while (worker_running_) {' + NL + '    while (!pending_fns_.empty()) {' + NL,
     '  while (worker_running_) {' + NL +
     '    {' + NL +
     '      const uint64_t now = rex::chrono::Clock::QueryHostTickCount();' + NL +
     '      const uint64_t freq = rex::chrono::Clock::QueryHostTickFrequency();' + NL +
     '      if (cp_stats.last_report_tick == 0) cp_stats.last_report_tick = now;' + NL +
     '      if (freq && now - cp_stats.last_report_tick >= freq) {' + NL +
     '        REXGPU_INFO("cp/s: kicks {}, batches {} ({} dwords), waitregmem {} packets {} polls {} ms, swaps {}, idle waits {}",' + NL +
     '                    cp_stats.kicks.exchange(0), cp_stats.batches.exchange(0), cp_stats.dwords.exchange(0),' + NL +
     '                    cp_stats.wait_packets.exchange(0), cp_stats.wait_polls.exchange(0),' + NL +
     '                    cp_stats.wait_poll_ns.exchange(0) / 1000000, cp_stats.swaps.exchange(0),' + NL +
     '                    cp_stats.idle_waits.exchange(0));' + NL +
     '        REXGPU_INFO("cp/s time: batches {} ms (draws {} in {} ms, swaps {} ms), idle waits {} ms",' + NL +
     '                    cp_stats.batch_ns.exchange(0) / 1000000, cp_stats.draws.exchange(0),' + NL +
     '                    cp_stats.draw_ns.exchange(0) / 1000000, cp_stats.swap_ns.exchange(0) / 1000000,' + NL +
     '                    cp_stats.idle_ns.exchange(0) / 1000000);' + NL +
     '        cp_stats.last_report_tick = now;' + NL +
     '      }' + NL + '    }' + NL +
     '    while (!pending_fns_.empty()) {' + NL),
    ('          rex::thread::Wait(write_ptr_index_event_.get(), true,' + NL,
     '          cp_stats.idle_waits++;' + NL +
     '          const uint64_t nx_i0 = rex::chrono::Clock::QueryHostTickCount();' + NL +
     '          rex::thread::Wait(write_ptr_index_event_.get(), true,' + NL),
    ('  bool is_memory = (wait_info & 0x10) != 0;' + NL,
     '  bool is_memory = (wait_info & 0x10) != 0;' + NL +
     '  cp_stats.wait_packets++;' + NL +
     '  const uint64_t nx_wait_start = rex::chrono::Clock::QueryHostTickCount();' + NL +
     '  const bool nx_log_this = cp_stats.logged++ < 60;' + NL +
     '  if (nx_log_this) {' + NL +
     '    REXGPU_INFO("WAIT_REG_MEM {} 0x{:08X} ref 0x{:08X} mask 0x{:08X} func {} wait 0x{:X}", is_memory ? "mem" : "reg",' + NL +
     '                poll_reg_addr, ref, mask, wait_info & 0x7, wait);' + NL + '  }' + NL),
    ('    if (!matched) {' + NL + '      // Wait.' + NL + '      if (wait >= 0x100) {' + NL,
     '    if (!matched) {' + NL + '      // Wait.' + NL + '      cp_stats.wait_polls++;' + NL + '      if (wait >= 0x100) {' + NL),
    ('  } while (!matched);' + NL + NL + '  return true;' + NL + '}' + NL + NL + 'bool CommandProcessor::ExecutePacketType3_REG_RMW(',
     '  } while (!matched);' + NL +
     '  {' + NL +
     '    const uint64_t freq = rex::chrono::Clock::QueryHostTickFrequency();' + NL +
     '    if (freq) cp_stats.wait_poll_ns += (rex::chrono::Clock::QueryHostTickCount() - nx_wait_start) * 1000000000ull / freq;' + NL +
     NX_WAIT_KIND +
     '    if (nx_log_this && freq) {' + NL +
     '      REXGPU_INFO("  ... satisfied after {} ms", (rex::chrono::Clock::QueryHostTickCount() - nx_wait_start) * 1000 / freq);' + NL +
     '    }' + NL +
     '  }' + NL + NL + '  return true;' + NL + '}' + NL + NL + 'bool CommandProcessor::ExecutePacketType3_REG_RMW('),
    ('  IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);' + NL,
     '  cp_stats.swaps++;' + NL +
     '  const uint64_t nx_s0 = rex::chrono::Clock::QueryHostTickCount();' + NL +
     '  IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);' + NL +
     '  cp_stats.swap_ns += (rex::chrono::Clock::QueryHostTickCount() - nx_s0) * 1000000000ull /' + NL +
     '                      std::max<uint64_t>(1, rex::chrono::Clock::QueryHostTickFrequency());' + NL),
    ('                            std::chrono::milliseconds(wait_time_ms));' + NL,
     '                            std::chrono::milliseconds(wait_time_ms));' + NL +
     '          cp_stats.idle_ns += (rex::chrono::Clock::QueryHostTickCount() - nx_i0) * 1000000000ull /' + NL +
     '                              std::max<uint64_t>(1, rex::chrono::Clock::QueryHostTickFrequency());' + NL),
    ('      draw_succeeded = IssueDraw(vgt_draw_initiator.prim_type, vgt_draw_initiator.num_indices,' + NL,
     '      const uint64_t nx_d0 = rex::chrono::Clock::QueryHostTickCount();' + NL +
     '      cp_stats.draws++;' + NL +
     '      draw_succeeded = IssueDraw(vgt_draw_initiator.prim_type, vgt_draw_initiator.num_indices,' + NL),
    ('                                 is_indexed ? &index_buffer_info : nullptr, major_mode_explicit);' + NL,
     '                                 is_indexed ? &index_buffer_info : nullptr, major_mode_explicit);' + NL +
     '      cp_stats.draw_ns += (rex::chrono::Clock::QueryHostTickCount() - nx_d0) * 1000000000ull /' + NL +
     '                          std::max<uint64_t>(1, rex::chrono::Clock::QueryHostTickFrequency());' + NL),
])
edit('src/graphics/command_processor.cpp', [
    # Anchored on the WAIT_REG_MEM edit's result (the include + the Switch extern block) so that a
    # rerun finds this text verbatim: an insert must never be modified by a later edit.
    ('#include <rex/graphics/command_processor.h>' + NL +
     '#if REX_PLATFORM_SWITCH' + NL +
     'extern "C" volatile uint64_t rex_nx_wait_polls, rex_nx_wait_sleep_ms, rex_nx_wait_yields, rex_nx_wait_slept_ticks;  // switch_nx.cpp' + NL +
     '#endif' + NL,
     '#include <rex/graphics/command_processor.h>' + NL +
     '#if REX_PLATFORM_SWITCH' + NL +
     'extern "C" volatile uint64_t rex_nx_wait_polls, rex_nx_wait_sleep_ms, rex_nx_wait_yields, rex_nx_wait_slept_ticks;  // switch_nx.cpp' + NL +
     '#endif' + NL + '#include <algorithm>' + NL + '#include <atomic>' + NL + '#include <rex/chrono/clock.h>' + NL +
     'namespace {' + NL +
     'struct CpStats {' + NL +
     '  std::atomic<uint64_t> kicks{0}, batches{0}, dwords{0}, wait_packets{0}, wait_polls{0}, wait_poll_ns{0},' + NL +
     '      swaps{0}, idle_waits{0}, batch_ns{0}, draw_ns{0}, draws{0}, swap_ns{0}, idle_ns{0}, logged{0};' + NL +
     '  uint64_t last_report_tick = 0;' + NL +
     '};' + NL + 'CpStats cp_stats;' + NL +
     '}  // namespace' + NL),
])
print('changed (23):', *changed, sep=NL + '  ')

# --- Phases inside the Vulkan IssueDraw (every platform; printed once per second from
# OnPrimaryBufferEnd): Linux spends the whole second in IssueDraw, ~4.8 ms per draw, with the thread
# mostly blocked; the console ~4 ms per draw. Which step blocks?
edit('src/graphics/vulkan/command_processor.cpp', [
    ('#include <rex/graphics/vulkan/command_processor.h>' + NL,
     '#include <rex/graphics/vulkan/command_processor.h>' + NL + '#include <atomic>' + NL + '#include <rex/chrono/clock.h>' + NL +
     'namespace {' + NL +
     'std::atomic<uint64_t> nx_phase_ticks[8];' + NL +
     'std::atomic<uint64_t> nx_sub_ticks[8];  // 0 analyze vs, 1 analyze ps, 2 BeginSubmission, 3 fence wait, 4 texture begin, 5 prim begin' + NL +
     'std::atomic<uint64_t> nx_sub_counts[8];  // 0 BeginSubmission calls, 1 opening frames, 2 new submissions, 3 fence waits' + NL +
     '#define NX_SUB(i, call)                                                              \\' + NL +
     '  do {                                                                               \\' + NL +
     '    const uint64_t nx_s0 = rex::chrono::Clock::QueryHostTickCount();                 \\' + NL +
     '    call;                                                                            \\' + NL +
     '    nx_sub_ticks[i] += rex::chrono::Clock::QueryHostTickCount() - nx_s0;             \\' + NL +
     '  } while (0)' + NL +
     'uint64_t nx_phase_report_tick = 0;' + NL +
     '}  // namespace' + NL +
     'namespace rex::graphics::vulkan {' + NL +
     '// 0 transfer calls, 1 transfers, 2 resolve clears, 3 texture loads, 4 texture bytes, 5 upload bytes, 6 resolves (IssueCopy)' + NL +
     'std::atomic<uint64_t> nx_gpu_work[8];' + NL +
     '}  // namespace rex::graphics::vulkan' + NL +
     '#define NX_PHASE(i) do { const uint64_t nx_now = rex::chrono::Clock::QueryHostTickCount(); nx_phase_ticks[i] += nx_now - nx_t; nx_t = nx_now; } while (0)' + NL),
    ('  memexport_ranges_.clear();' + NL,
     '  uint64_t nx_t = rex::chrono::Clock::QueryHostTickCount();' + NL + '  memexport_ranges_.clear();' + NL),
    ('    if (!primitive_processor_->Process(primitive_processing_result)) {' + NL,
     '    NX_PHASE(0);' + NL + '    if (!primitive_processor_->Process(primitive_processing_result)) {' + NL),
    ('    vertex_shader_modification = pipeline_cache_->GetCurrentVertexShaderModification(' + NL,
     '    NX_PHASE(1);' + NL + '    vertex_shader_modification = pipeline_cache_->GetCurrentVertexShaderModification(' + NL),
    ('  texture_cache_->RequestTextures(used_texture_mask);' + NL,
     '  NX_PHASE(2);' + NL + '  texture_cache_->RequestTextures(used_texture_mask);' + NL + '  NX_PHASE(3);' + NL),
    ('  if (!pipeline_cache_->ConfigurePipeline(vertex_shader_translation, pixel_shader_translation,' + NL,
     '  NX_PHASE(4);' + NL + '  if (!pipeline_cache_->ConfigurePipeline(vertex_shader_translation, pixel_shader_translation,' + NL),
    ('  UpdateDynamicState(viewport_info, primitive_polygonal, normalized_depth_control);' + NL,
     '  NX_PHASE(5);' + NL + '  UpdateDynamicState(viewport_info, primitive_polygonal, normalized_depth_control);' + NL),
    ('  if (!UpdateBindings(vertex_shader, pixel_shader)) {' + NL,
     '  NX_PHASE(6);' + NL + '  if (!UpdateBindings(vertex_shader, pixel_shader)) {' + NL),
    ('      shared_memory_->RangeWrittenByGpu(memexport_range.base_address_dwords << 2,' + NL,
     '      NX_PHASE(7);' + NL + '      shared_memory_->RangeWrittenByGpu(memexport_range.base_address_dwords << 2,' + NL),
    ('void VulkanCommandProcessor::OnPrimaryBufferEnd() {' + NL,
     'void VulkanCommandProcessor::OnPrimaryBufferEnd() {' + NL +
     '  {' + NL +
     '    const uint64_t now = rex::chrono::Clock::QueryHostTickCount();' + NL +
     '    const uint64_t freq = rex::chrono::Clock::QueryHostTickFrequency();' + NL +
     '    if (nx_phase_report_tick == 0) nx_phase_report_tick = now;' + NL +
     '    if (freq && now - nx_phase_report_tick >= freq) {' + NL +
     '      REXGPU_INFO("draw phases ms: pre {} prim {} shaders {} textures {} rt {} pipeline {} state {} bind+vb+draw {}",' + NL +
     '                  nx_phase_ticks[0].exchange(0) * 1000 / freq, nx_phase_ticks[1].exchange(0) * 1000 / freq,' + NL +
     '                  nx_phase_ticks[2].exchange(0) * 1000 / freq, nx_phase_ticks[3].exchange(0) * 1000 / freq,' + NL +
     '                  nx_phase_ticks[4].exchange(0) * 1000 / freq, nx_phase_ticks[5].exchange(0) * 1000 / freq,' + NL +
     '                  nx_phase_ticks[6].exchange(0) * 1000 / freq, nx_phase_ticks[7].exchange(0) * 1000 / freq);' + NL +
     '      nx_phase_report_tick = now;' + NL +
     '    }' + NL + '  }' + NL),
])
print('changed (25):', *changed, sep=NL + '  ')

# --- Sub-timers of the "pre" phase (every platform): the shader analyses, BeginSubmission and its
# parts (the fence wait when a frame opens, the texture cache's and primitive processor's submission
# setup), with counts of draws that found no open submission or frame.
edit('src/graphics/vulkan/command_processor.cpp', [
    # (the sub-timer definitions are part of the phase-timer insert above)
    ('  pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);' + NL + '  bool memexport_used_vertex = vertex_shader->memexport_eM_written() != 0;' + NL,
     '  NX_SUB(0, pipeline_cache_->AnalyzeShaderUcode(*vertex_shader));' + NL + '  bool memexport_used_vertex = vertex_shader->memexport_eM_written() != 0;' + NL),
    ('        pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);' + NL + '        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {' + NL,
     '        NX_SUB(1, pipeline_cache_->AnalyzeShaderUcode(*pixel_shader));' + NL + '        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {' + NL),
    ('  for (uint32_t i = 0; i < 2; ++i) {' + NL + '    if (!BeginSubmission(true)) {' + NL + '      return draw_fail("begin_submission");' + NL + '    }' + NL,
     '  for (uint32_t i = 0; i < 2; ++i) {' + NL + '    const uint64_t nx_bs0 = rex::chrono::Clock::QueryHostTickCount();' + NL +
     '    const bool nx_bs_ok = BeginSubmission(true);' + NL +
     '    nx_sub_ticks[2] += rex::chrono::Clock::QueryHostTickCount() - nx_bs0;' + NL +
     '    if (!nx_bs_ok) {' + NL + '      return draw_fail("begin_submission");' + NL + '    }' + NL),
    ('  bool is_opening_frame = is_guest_command && !frame_open_;' + NL + '  if (submission_open_ && !is_opening_frame) {' + NL + '    return true;' + NL + '  }' + NL,
     '  bool is_opening_frame = is_guest_command && !frame_open_;' + NL + '  nx_sub_counts[0]++;' + NL +
     '  if (submission_open_ && !is_opening_frame) {' + NL + '    return true;' + NL + '  }' + NL +
     '  if (is_opening_frame) nx_sub_counts[1]++;' + NL + '  if (!submission_open_) nx_sub_counts[2]++;' + NL),
    ('  CheckSubmissionFenceAndDeviceLoss(await_submission);' + NL + '  if (device_lost_ || submission_completed_ < await_submission) {' + NL,
     '  NX_SUB(3, CheckSubmissionFenceAndDeviceLoss(await_submission));' + NL + '  if (device_lost_ || submission_completed_ < await_submission) {' + NL),
    ('    primitive_processor_->BeginSubmission();' + NL + NL + '    texture_cache_->BeginSubmission(GetCurrentSubmission());' + NL,
     '    NX_SUB(5, primitive_processor_->BeginSubmission());' + NL + NL + '    NX_SUB(4, texture_cache_->BeginSubmission(GetCurrentSubmission()));' + NL),
    ('  if (await_submission > submission_completed_) {' + NL + '    VkResult wait_result =' + NL,
     '  if (await_submission > submission_completed_) {' + NL + '    nx_sub_counts[3]++;' + NL + '    VkResult wait_result =' + NL),
    ('      nx_phase_report_tick = now;' + NL,
     '      REXGPU_INFO("pre sub ms: analyze vs {} ps {}, BeginSubmission {} (calls {}, opening frames {}, new submissions {}, fence waits {} in {} ms, texture begin {} ms, prim begin {} ms)",' + NL +
     '                  nx_sub_ticks[0].exchange(0) * 1000 / freq, nx_sub_ticks[1].exchange(0) * 1000 / freq,' + NL +
     '                  nx_sub_ticks[2].exchange(0) * 1000 / freq, nx_sub_counts[0].exchange(0), nx_sub_counts[1].exchange(0),' + NL +
     '                  nx_sub_counts[2].exchange(0), nx_sub_counts[3].exchange(0), nx_sub_ticks[3].exchange(0) * 1000 / freq,' + NL +
     '                  nx_sub_ticks[4].exchange(0) * 1000 / freq, nx_sub_ticks[5].exchange(0) * 1000 / freq);' + NL +
     '      nx_phase_report_tick = now;' + NL),
])
print('changed (26):', *changed, sep=NL + '  ')

# --- GPU work generators per second (every platform): render-target transfer passes, texture loads
# (count and guest bytes), shared-memory upload bytes, resolves. Linux: the command processor waits
# ~500 ms per frame for the previous frame's GPU work in BeginSubmission; the console shows 478 ms.
edit('src/graphics/vulkan/command_processor.cpp', [
    # (nx_gpu_work is defined by the phase-timer insert above)
    ('bool VulkanCommandProcessor::IssueCopy() {' + NL,
     'bool VulkanCommandProcessor::IssueCopy() {' + NL + '  nx_gpu_work[6]++;' + NL),
    ('      REXGPU_INFO("pre sub ms: analyze vs {} ps {}, BeginSubmission {} (calls {}, opening frames {}, new submissions {}, fence waits {} in {} ms, texture begin {} ms, prim begin {} ms)",' + NL,
     '      REXGPU_INFO("gpu work/s: rt transfer passes {} ({} transfers, {} resolve clears), texture loads {} ({} KB), shared memory uploads {} KB, resolves {}",' + NL +
     '                  nx_gpu_work[0].exchange(0), nx_gpu_work[1].exchange(0), nx_gpu_work[2].exchange(0), nx_gpu_work[3].exchange(0),' + NL +
     '                  nx_gpu_work[4].exchange(0) >> 10, nx_gpu_work[5].exchange(0) >> 10, nx_gpu_work[6].exchange(0));' + NL +
     '      REXGPU_INFO("pre sub ms: analyze vs {} ps {}, BeginSubmission {} (calls {}, opening frames {}, new submissions {}, fence waits {} in {} ms, texture begin {} ms, prim begin {} ms)",' + NL),
])
edit('src/graphics/vulkan/render_target_cache.cpp', [
    ('    const Transfer::Rectangle* resolve_clear_rectangle) {' + NL + '  assert_true(GetPath() == Path::kHostRenderTargets);' + NL,
     '    const Transfer::Rectangle* resolve_clear_rectangle) {' + NL + '  assert_true(GetPath() == Path::kHostRenderTargets);' + NL +
     '  {' + NL +
     '    extern std::atomic<uint64_t> nx_gpu_work[8];' + NL +
     '    nx_gpu_work[0]++;' + NL +
     '    if (render_target_transfers) {' + NL +
     '      for (uint32_t nx_i = 0; nx_i < render_target_count; ++nx_i) nx_gpu_work[1] += render_target_transfers[nx_i].size();' + NL +
     '    }' + NL +
     '    if (render_target_resolve_clear_values && resolve_clear_rectangle) nx_gpu_work[2]++;' + NL +
     '  }' + NL),
])
edit('src/graphics/vulkan/texture_cache.cpp', [
    ('  VulkanTexture& vulkan_texture = static_cast<VulkanTexture&>(texture);' + NL + '  TextureKey texture_key = vulkan_texture.key();' + NL + NL + '  // Get the pipeline.' + NL,
     '  VulkanTexture& vulkan_texture = static_cast<VulkanTexture&>(texture);' + NL + '  TextureKey texture_key = vulkan_texture.key();' + NL +
     '  {' + NL + '    extern std::atomic<uint64_t> nx_gpu_work[8];' + NL + '    nx_gpu_work[3]++;' + NL +
     '    nx_gpu_work[4] += (load_base ? texture.GetGuestBaseSize() : 0) + (load_mips ? texture.GetGuestMipsSize() : 0);' + NL + '  }' + NL + NL +
     '  // Get the pipeline.' + NL),
])
edit('src/graphics/vulkan/shared_memory.cpp', [
    ('    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {' + NL + '  if (upload_page_ranges.empty()) {' + NL + '    return true;' + NL + '  }' + NL,
     '    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {' + NL + '  if (upload_page_ranges.empty()) {' + NL + '    return true;' + NL + '  }' + NL +
     '  {' + NL + '    extern std::atomic<uint64_t> nx_gpu_work[8];' + NL +
     '    for (const auto& nx_r : upload_page_ranges) nx_gpu_work[5] += uint64_t(nx_r.second) << page_size_log2();' + NL + '  }' + NL),
])
for f in ('src/graphics/vulkan/render_target_cache.cpp', 'src/graphics/vulkan/texture_cache.cpp', 'src/graphics/vulkan/shared_memory.cpp'):
    edit(f, [('#include <rex/graphics/vulkan/' + f.split('/')[-1].replace('.cpp', '.h') + '>' + NL,
              '#include <rex/graphics/vulkan/' + f.split('/')[-1].replace('.cpp', '.h') + '>' + NL + '#include <atomic>' + NL)], must=False)
print('changed (27):', *changed, sep=NL + '  ')

# --- Swap counter for the clock governor (switch_nx.cpp govern(): frames lost in the last second). A separate edit on the
# FPS edit's result, so a rerun finds both texts. The asm label keeps the plain C symbol from inside a namespace. ---
edit('src/graphics/command_processor.cpp', [
    ('      nx_frames++;' + NL,
     '      nx_frames++;' + NL +
     '      extern volatile uint64_t rex_nx_swaps __asm__("rex_nx_swaps");  // switch_nx.cpp' + NL +
     '      rex_nx_swaps = rex_nx_swaps + 1;' + NL),
])
