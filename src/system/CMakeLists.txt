set(REXSYSTEM_SOURCES
    runtime.cpp
    embedded_metadata.cpp
    achievement_cache.cpp
    achievement_manager.cpp
    achievements.cpp
    kernel_state.cpp
    xmemory.cpp
    kernel_module.cpp
    user_module.cpp
    xthread.cpp
    xevent.cpp
    xfile.cpp
    xmutant.cpp
    xsemaphore.cpp
    xtimer.cpp
    xiocompletion.cpp
    xnotifylistener.cpp
    xenumerator.cpp
    xsocket.cpp
    xsymboliclink.cpp
    xobject.cpp
    xmodule.cpp
    guest_path.cpp

    # Utilities
    util/object_table.cpp
    util/native_list.cpp
    util/xdbf_utils.cpp
    util/gameinfo_utils.cpp

    # XAM services
    xam/app_manager.cpp
    xam/content_manager.cpp
    xam/user_profile.cpp

    # Online play (online_play)
    online/virtual_net.cpp
    online/udp_transport.cpp
    online/session_service.cpp
    online/steam_bridge.cpp

    ppc_types.cpp
    elf_module.cpp
    entry_table.cpp
    export_resolver.cpp
    lzx.cpp
    mmio_handler.cpp
    map_parser.cpp
    module.cpp
    function_dispatcher.cpp
    gpu_plugin_loader.cpp
    thread.cpp
    thread_state.cpp
    xex_module.cpp
)

add_library(rexruntime SHARED ${REXSYSTEM_SOURCES})
add_library(rex::runtime ALIAS rexruntime)
add_library(rex::system ALIAS rexruntime)
add_library(rex::kernel ALIAS rexruntime)

set_target_properties(rexruntime PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
rexglue_add_version_resource(rexruntime DESCRIPTION "ReXGlue runtime")

rex_target_pch(rexruntime
    <mutex>
    <unordered_map>
    <rex/math.h>
    <rex/string.h>
    <rex/memory.h>
    <rex/filesystem.h>
    <rex/thread.h>
    <rex/thread/mutex.h>
    <rex/ppc/context.h>
    <rex/system/kernel_state.h>
)

target_include_directories(rexruntime PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/thirdparty/imgui>
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/thirdparty/xxHash>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(rexruntime PRIVATE
    ${REXGLUE_ROOT}/thirdparty
)

target_link_libraries(rexruntime
    PUBLIC
        simde
        fmt::fmt
        spdlog::spdlog
        tomlplusplus
        SDL3::SDL3
    PRIVATE
        aes128
        mspack
        xxHash::xxhash
)

if(REXGLUE_ENABLE_TRACY)
    # TARGET_NAME is what tells install(EXPORT) this text is a target to
    # namespace. Without it the plain name survives into the installed config
    # and consumers link a bare TracyClient they have no search path for.
    target_link_libraries(rexruntime PUBLIC
        $<$<NOT:$<CONFIG:Release>>:$<TARGET_NAME:TracyClient>>)
    target_compile_definitions(rexruntime PUBLIC
        $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PROFILING>)
endif()

if(REXGLUE_ENABLE_PERF_COUNTERS)
    target_compile_definitions(rexruntime PUBLIC
        $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PERF_COUNTERS>)
endif()

if(WIN32)
    target_link_libraries(rexruntime PRIVATE ws2_32)
    target_link_libraries(rexruntime PUBLIC dxgi dxguid)
    target_compile_definitions(rexruntime PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(REXGLUE_USE_D3D12)
    target_link_libraries(rexruntime PUBLIC dxc-headers)
    target_compile_definitions(rexruntime PUBLIC REX_HAS_D3D12=1)
endif()
if(REXGLUE_USE_VULKAN)
    target_link_libraries(rexruntime PUBLIC
        Vulkan::Headers
        GPUOpen::VulkanMemoryAllocator
        spirv-tools-headers
    )
    target_compile_definitions(rexruntime PUBLIC REX_HAS_VULKAN=1)
endif()

# The Steam helper (online play through Steam, rex/system/online/steam_link.h): a small process of
# its own, so that Steam (as app 480) never takes the game's controllers or hooks its renderer. It
# loads steam_api64.dll at run time; the Steamworks SDK is not needed to build it.
set(REXGLUE_STEAM_API_DLL "" CACHE FILEPATH
    "Steam's steam_api64.dll (the Steamworks SDK's redistributable_bin/win64) to put next to the game")
add_library(rexsteam_core STATIC online/steam_helper/helper.cpp)
target_include_directories(rexsteam_core PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/online/steam_helper>)
target_compile_features(rexsteam_core PUBLIC cxx_std_20)
if(WIN32)
    target_link_libraries(rexsteam_core PUBLIC ws2_32)
    add_executable(rexsteam
        online/steam_helper/main.cpp
        online/steam_helper/steam_backend.cpp)
    target_link_libraries(rexsteam PRIVATE rexsteam_core winmm)
    rexglue_add_version_resource(rexsteam DESCRIPTION "ReXGlue Steam helper")
endif()
