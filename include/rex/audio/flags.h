/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstdint>
#include <string>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, audio_mute);
REXCVAR_DECLARE(bool, ffmpeg_verbose);
// The player's output settings (downmix.cpp): the output device by name ("" = Windows' default), the channels
// ("auto", "stereo", "mono"), the volume (0-100) and the silence while the game's window is not in front.
REXCVAR_DECLARE(std::string, audio_output_device);
REXCVAR_DECLARE(std::string, audio_channels);
REXCVAR_DECLARE(int32_t, audio_volume);
REXCVAR_DECLARE(bool, audio_mute_unfocused);
