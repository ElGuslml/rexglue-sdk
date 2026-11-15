/**
 * @file        input/sdl/gameinput_instruments.h
 * @brief       Xbox One guitars' raw reports, read through a GameInput of the SDL driver's own
 *
 * SDL reads Xbox One guitars (GIP devices such as the PDP Riffmaster, neither XInput nor HID) through GameInput's raw
 * device reports, and gives their tilt as a button only (its report's byte past a fixed point). The tilt itself, the
 * report's third byte, is read here, from the device SDL opened, so that a guitar's tilt is an axis as on an Xbox 360
 * guitar. Windows only; elsewhere nothing is there.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace rex::input::sdl::gameinput {

#if defined(_WIN32)

// GameInput's API 3 created as SDL creates it (System32's gameinput.dll, whose object answers for API 3 when the
// Microsoft GameInput redistributable is installed). False, and why in `status`, when it is not there.
bool Start(std::string& status);
void Stop();

// The GameInput device SDL opened at `sdl_path` (SDL_GetGamepadPath of a GameInput device: its APP_LOCAL_DEVICE_ID in
// hex), held until CloseDevice; null if GameInput is not started or the device is gone.
void* OpenDevice(const char* sdl_path);
void CloseDevice(void* device);

// A Rock Band 4 guitar's tilt, its raw report's third byte, if the device has a reading.
std::optional<uint8_t> ReadGuitarTilt(void* device);

// The Xbox One instruments connected now that SDL reads through GameInput's raw reports (SDL_gameinputjoystick.cpp's
// GAMEINPUT_GetDeviceRawType: the Rock Band 4 guitars and drum kits, the Mad Catz legacy adapter, the Guitar Hero Live
// guitar), each "vendor:product name" in `names`, comma-separated; 0 without GameInput. SDL's raw reports are for them
// only: with them on, SDL's GameInput driver, ahead of its XInput one, also takes Xbox 360 instruments and controllers,
// as gamepads (a guitar's XInput subtype lost) or as well as XInput (one guitar in two slots).
int CountRawInstruments(std::string* names);

#else

inline bool Start(std::string& status) {
  status = "not on this platform";
  return false;
}
inline void Stop() {}
inline void* OpenDevice(const char*) {
  return nullptr;
}
inline void CloseDevice(void*) {}
inline std::optional<uint8_t> ReadGuitarTilt(void*) {
  return std::nullopt;
}
inline int CountRawInstruments(std::string*) {
  return 0;
}

#endif

}  // namespace rex::input::sdl::gameinput
