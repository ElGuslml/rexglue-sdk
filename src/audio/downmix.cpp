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
                      "stereo: always folded to stereo; mono: folded, then the same sound on both sides; surround: as "
                      "auto, the game told it plays 5.1 whatever the device (with audio_speaker_config_from_output)");
REXCVAR_DEFINE_BOOL(audio_speaker_config_from_output, false, "Audio",
                    "The game is told the speakers it plays on (XAudioGetSpeakerConfig): Dolby Digital 5.1 only with "
                    "audio_channels surround, or auto on a device of more than two channels, and two speakers "
                    "otherwise, so that it mixes its own stereo. false: always Dolby Digital, as before (the game "
                    "mixes 5.1 and a stereo device gets the SDK's fold)");
REXCVAR_DEFINE_INT32(audio_volume, 100, "Audio",
                     "The game's volume, 0-200 (%). Above 100 the loudest moments can clip");
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
// The output plays 5.1 (SetOutputPlaysSurround): -1 until an output opens, then 0 or 1.
std::atomic<int> g_output_surround{-1};
std::atomic<bool (*)()> g_surround_probe{nullptr};

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
  const float volume = float(std::clamp(REXCVAR_GET(audio_volume), 0, 200)) / 100.0f;
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_gain * volume;
}

OutputChannels GetOutputChannels() {
  std::string mode = REXCVAR_GET(audio_channels);
  std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  if (mode == "stereo") return OutputChannels::kStereo;
  if (mode == "mono") return OutputChannels::kMono;
  if (mode == "surround" || mode == "5.1") return OutputChannels::kSurround;
  return OutputChannels::kAuto;
}

void SetOutputPlaysSurround(bool surround) {
  g_output_surround.store(surround ? 1 : 0, std::memory_order_relaxed);
}

void SetOutputSurroundProbe(bool (*probe)()) {
  g_surround_probe.store(probe, std::memory_order_relaxed);
}

bool OutputPlaysSurround() {
  const int known = g_output_surround.load(std::memory_order_relaxed);
  if (known >= 0) return known != 0;
  bool (*probe)() = g_surround_probe.load(std::memory_order_relaxed);
  return probe && probe();
}

bool TitleMixesSurround() {
  if (!REXCVAR_GET(audio_speaker_config_from_output)) return true;
  switch (GetOutputChannels()) {
    case OutputChannels::kSurround:
      return true;
    case OutputChannels::kAuto:
      return OutputPlaysSurround();
    default:
      return false;
  }
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
