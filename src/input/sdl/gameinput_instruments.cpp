/**
 * @file        input/sdl/gameinput_instruments.cpp
 * @brief       Xbox One guitars' raw reports, read through a GameInput of the SDL driver's own (Windows)
 *
 * Without the SDK's other libraries (Windows and GameInput's header only): RecompLauncher compiles it too, for its
 * Controls page.
 */

#include "gameinput_instruments.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <GameInput.h>

#include <cstdio>

namespace rex::input::sdl::gameinput {

namespace {

using namespace ::GameInput::v3;

HMODULE g_dll = nullptr;
IGameInput* g_game_input = nullptr;

}  // namespace

bool Start(std::string& status) {
  if (g_game_input) {
    return true;
  }
  // As SDL_InitGameInput does, so that the two find the same GameInput: SDL's raw reports and this reader work or
  // fail together.
  g_dll = LoadLibraryW(L"gameinput.dll");
  if (!g_dll) {
    status = "gameinput.dll not found";
    return false;
  }
  using CreateFn = HRESULT(WINAPI*)(IUnknown**);
  auto create = reinterpret_cast<CreateFn>(GetProcAddress(g_dll, "GameInputCreate"));
  IUnknown* unknown = nullptr;
  HRESULT hr = create ? create(&unknown) : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
  if (SUCCEEDED(hr) && unknown) {
    hr = unknown->QueryInterface(IID_IGameInput, reinterpret_cast<void**>(&g_game_input));
    unknown->Release();
  }
  if (FAILED(hr) || !g_game_input) {
    // E_NOINTERFACE: Windows' own GameInput, API 0 only, without the redistributable.
    char text[64];
    std::snprintf(text, sizeof(text), "GameInput's API 3 not answered: 0x%08lX", static_cast<unsigned long>(hr));
    status = text;
    g_game_input = nullptr;
    FreeLibrary(g_dll);
    g_dll = nullptr;
    return false;
  }
  // As SDL sets its own: readings go on without the window's focus (the driver decides what the game sees then).
  g_game_input->SetFocusPolicy(GameInputEnableBackgroundInput);
  status = "on";
  return true;
}

void Stop() {
  if (g_game_input) {
    g_game_input->Release();
    g_game_input = nullptr;
  }
  if (g_dll) {
    FreeLibrary(g_dll);
    g_dll = nullptr;
  }
}

void* OpenDevice(const char* sdl_path) {
  if (!g_game_input || !sdl_path) {
    return nullptr;
  }
  // SDL's GameInput path is the device's APP_LOCAL_DEVICE_ID, two hex digits a byte.
  APP_LOCAL_DEVICE_ID id = {};
  for (size_t i = 0; i < APP_LOCAL_DEVICE_ID_SIZE; ++i) {
    unsigned value = 0;
    for (int digit = 0; digit < 2; ++digit) {
      const char c = sdl_path[i * 2 + digit];
      const int nibble = c >= '0' && c <= '9'   ? c - '0'
                         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                                : -1;
      if (nibble < 0) {
        return nullptr;
      }
      value = value * 16 + static_cast<unsigned>(nibble);
    }
    id.value[i] = static_cast<BYTE>(value);
  }
  IGameInputDevice* device = nullptr;
  if (FAILED(g_game_input->FindDeviceFromId(&id, &device))) {
    return nullptr;
  }
  return device;
}

void CloseDevice(void* device) {
  if (device) {
    static_cast<IGameInputDevice*>(device)->Release();
  }
}

std::optional<uint8_t> ReadGuitarTilt(void* device) {
  if (!g_game_input || !device) {
    return std::nullopt;
  }
  IGameInputReading* reading = nullptr;
  if (FAILED(g_game_input->GetCurrentReading(GameInputKindRawDeviceReport, static_cast<IGameInputDevice*>(device),
                                             &reading)) ||
      !reading) {
    return std::nullopt;
  }
  std::optional<uint8_t> tilt;
  IGameInputRawDeviceReport* report = nullptr;
  if (reading->GetRawReport(&report) && report) {
    // The report SDL reads (SDL_gameinputjoystick.cpp, GAMEINPUT_GuitarUpdate): buttons in bytes 0-1, the tilt in 2,
    // the whammy bar in 3, the pickup selector in 4's high nibble, the upper and lower frets in 5 and 6.
    uint8_t data[40] = {};
    if (report->GetRawData(sizeof(data), data) >= 10) {
      tilt = data[2];
    }
    report->Release();
  }
  reading->Release();
  return tilt;
}

}  // namespace rex::input::sdl::gameinput
