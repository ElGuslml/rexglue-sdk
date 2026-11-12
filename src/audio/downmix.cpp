/**
 * @file        audio/downmix.cpp
 * @brief       Output-stage mix parameters: 5.1 fold, 5.1 matrix and master gain
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>

#include <rex/audio/downmix.h>
#include <rex/audio/flags.h>
#include <rex/cvar.h>

// The player's output settings (a launcher's audio page writes them). None changes anything until set.
REXCVAR_DEFINE_STRING(audio_output_device, "", "Audio",
                      "The output device by its name as Windows shows it (\"Speakers (Realtek(R) Audio)\"); empty or "
                      "\"default\": Windows' default output, followed when it changes. A name not found plays on the "
                      "default one");
REXCVAR_DEFINE_STRING(audio_channels, "auto", "Audio",
                      "auto: the game's 5.1 to a device of more than two channels, folded to stereo for the others; "
                      "stereo: always folded to stereo; mono: folded, then the same sound on both sides");
REXCVAR_DEFINE_INT32(audio_volume, 100, "Audio", "The game's volume, 0-100 (%)");
REXCVAR_DEFINE_BOOL(audio_mute_unfocused, false, "Audio",
                    "Silence while the game's window is not in front (another window has the focus)");

namespace rex::audio {
namespace {

// One output device exists, so the mix parameters are process state. The
// reader is the SDL device callback, which runs every 5.33 ms, so a plain
// mutex costs nothing and avoids a torn read across the four weights.
std::mutex g_mutex;
StereoFold g_fold = {};
SurroundMix g_mix = {};
float g_gain = 1.0f;
// The game's window is in the background (SetWindowInBackground).
std::atomic<bool> g_background{false};

}  // namespace

void SetStereoFold(const StereoFold& fold) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_fold = fold;
}

StereoFold GetStereoFold() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_fold;
}

void SetSurroundMix(const SurroundMix& mix) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_mix = mix;
}

SurroundMix GetSurroundMix() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_mix;
}

void SetOutputGain(float linear) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_gain = linear;
}

float GetOutputGain() {
  if (g_background.load(std::memory_order_relaxed) && REXCVAR_GET(audio_mute_unfocused)) {
    return 0.0f;
  }
  const float volume = float(std::clamp(REXCVAR_GET(audio_volume), 0, 100)) / 100.0f;
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_gain * volume;
}

OutputChannels GetOutputChannels() {
  std::string mode = REXCVAR_GET(audio_channels);
  std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  if (mode == "stereo") return OutputChannels::kStereo;
  if (mode == "mono") return OutputChannels::kMono;
  return OutputChannels::kAuto;
}

void SetWindowInBackground(bool background) {
  g_background.store(background, std::memory_order_relaxed);
}

void FoldStereoToMono(float* interleaved, size_t frames) {
  for (size_t i = 0; i < frames; ++i) {
    const float mean = (interleaved[2 * i] + interleaved[2 * i + 1]) * 0.5f;
    interleaved[2 * i] = mean;
    interleaved[2 * i + 1] = mean;
  }
}

std::string GetOutputDeviceName() {
  const std::string& name = REXCVAR_GET(audio_output_device);
  return name == "default" ? std::string() : name;
}

}  // namespace rex::audio
