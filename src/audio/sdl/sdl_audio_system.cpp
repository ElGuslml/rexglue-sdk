/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/audio/flags.h>
#include <rex/audio/sdl/sdl_audio_driver.h>
#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/cvar.h>
#include <rex/platform.h>
#if REX_PLATFORM_WIN32
#include <rex/audio/wasapi/wasapi_exclusive_driver.h>
REXCVAR_DECLARE(bool, audio_wasapi_exclusive);
#endif

namespace rex::audio::sdl {

std::unique_ptr<AudioSystem> SDLAudioSystem::Create(
    runtime::FunctionDispatcher* function_dispatcher) {
  return std::make_unique<SDLAudioSystem>(function_dispatcher);
}

SDLAudioSystem::SDLAudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : AudioSystem(function_dispatcher) {}

SDLAudioSystem::~SDLAudioSystem() {}

void SDLAudioSystem::Initialize() {
  AudioSystem::Initialize();
}

X_STATUS SDLAudioSystem::CreateDriver([[maybe_unused]] size_t index,
                                      rex::thread::Semaphore* semaphore, AudioDriver** out_driver) {
  assert_not_null(out_driver);
#if REX_PLATFORM_WIN32
  // audio_wasapi_exclusive: the device in exclusive mode, else (busy, refused) the shared output below.
  if (REXCVAR_GET(audio_wasapi_exclusive)) {
    auto exclusive = new wasapi::WasapiExclusiveDriver(memory_, semaphore);
    if (exclusive->Initialize()) {
      *out_driver = exclusive;
      return X_STATUS_SUCCESS;
    }
    delete exclusive;
  }
#endif
  auto driver = new SDLAudioDriver(memory_, semaphore);
  if (!driver->Initialize()) {
    driver->Shutdown();
    delete driver;
    return X_STATUS_UNSUCCESSFUL;
  }

  *out_driver = driver;
  return X_STATUS_SUCCESS;
}

void SDLAudioSystem::DestroyDriver(AudioDriver* driver) {
  assert_not_null(driver);
#if REX_PLATFORM_WIN32
  if (auto exclusive = dynamic_cast<wasapi::WasapiExclusiveDriver*>(driver)) {
    exclusive->Shutdown();
    delete exclusive;
    return;
  }
#endif
  auto sdldriver = dynamic_cast<SDLAudioDriver*>(driver);
  assert_not_null(sdldriver);
  sdldriver->Shutdown();
  delete sdldriver;
}

}  // namespace rex::audio::sdl
