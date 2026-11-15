# rexinput - Input system library
# Input system and drivers for Xbox 360 controller emulation

# Microsoft GameInput's redistributable (GameInputRedist.msi, the Microsoft.GameInput NuGet package's redist/), put next
# to the game by rexglue_helpers.cmake: Xbox One guitars and drum kits need GameInput's API 3 (thirdparty/gameinput).
set(REXGLUE_GAMEINPUT_REDIST_MSI "" CACHE FILEPATH
    "Microsoft GameInput's GameInputRedist.msi (the Microsoft.GameInput NuGet package's redist) to put next to the game")

add_library(rexinput OBJECT
    input_system.cpp
    device_assignment.cpp
    state_merge.cpp
    tap_strum.cpp
    mnk/mnk_input_driver.cpp
    mnk/system_shortcuts.cpp
    nop/nop_input_driver.cpp
)
add_library(rex::input ALIAS rexinput)

rex_target_pch(rexinput
    <mutex>
    <rex/math.h>
    <rex/string.h>
    <rex/memory.h>
    <rex/filesystem.h>
    <rex/thread.h>
    <rex/thread/mutex.h>
    <rex/ppc/context.h>
    <rex/system/kernel_state.h>
    <rex/system/xthread.h>
    <rex/ui/window.h>
    <rex/ui/presenter.h>
)

target_include_directories(rexinput PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
if(WIN32)
    target_sources(rexinput PRIVATE sdl/sdl_input_driver.cpp sdl/gameinput_instruments.cpp
                                   xinput/xinput_input_driver.cpp)
    # GameInput's API 3 header, for Xbox One guitars' tilt (as SDL's own GameInput driver is built with it).
    target_include_directories(rexinput PRIVATE "${PROJECT_SOURCE_DIR}/thirdparty/gameinput/include")
else()
    target_sources(rexinput PRIVATE sdl/sdl_input_driver.cpp)
endif()

target_link_libraries(rexinput PUBLIC rexcore rexui SDL3::SDL3)
