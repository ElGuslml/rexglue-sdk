/**
 * @file        audio/downmix.h
 * @brief       Output-stage mix parameters: 5.1 fold, 5.1 matrix and master gain
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstddef>
#include <string>

namespace rex::audio {

/**
 * Weights applied when the guest's 5.1 render is folded to a stereo device.
 * Guest channel order is the XAudio default: fl fr fc lf bl br. The front
 * channels carry an implicit weight of 1.0, so `scale` alone sets the output
 * level.
 *
 * Defaults follow ITU-R BS.775 and Dolby Lo/Ro: center and surround at -3 dB,
 * LFE dropped. No published stereo downmix folds LFE, which is authored around
 * 10 dB hot by convention, so a title that wants it audible sets a weight that
 * undoes that offset rather than folding it at unity.
 */
struct StereoFold {
  float center = 0.70710678f;
  float surround = 0.70710678f;
  float lfe = 0.0f;
  float scale = 0.58578644f;  // 1/(1+0.707)
};

/**
 * Weights applied when the guest's 5.1 render reaches a device with more than
 * two channels, which the output stage passes through rather than folding.
 * Same channel order and the same implicit front weight of 1.0 as StereoFold,
 * so the two structs describe one mix in two destinations.
 *
 * Defaults are unity, a passthrough, because a title whose render really is
 * 5.1 wants its own mix reproduced. A title that packs something other than an
 * LFE into the LFE slot sets `lfe` to drop or attenuate it: that slot is
 * reproduced around 10 dB hot, so full-band content placed there arrives far
 * louder than it was authored, and a downstream downmix that folds it without
 * low-passing carries the whole band into the other channels.
 */
struct SurroundMix {
  float center = 1.0f;
  float surround = 1.0f;
  float lfe = 1.0f;
};

/// Safe to call from any thread. The output stage picks the new values up on
/// its next device callback.
void SetStereoFold(const StereoFold& fold);
StereoFold GetStereoFold();

void SetSurroundMix(const SurroundMix& mix);
SurroundMix GetSurroundMix();

/// Linear master gain applied to the stereo fold and to 5.1 passthrough alike.
/// 1.0 is unity. Above unity can clip, and the output stage clamps.
/// GetOutputGain returns the title's gain times the player's volume
/// (audio_volume), and 0 while the game's window is in the background with
/// audio_mute_unfocused on.
void SetOutputGain(float linear);
float GetOutputGain();

/// The player's choice of channels (audio_channels): kAuto passes the 5.1 render
/// to a device of more than two channels and folds it for the others; kStereo
/// folds it whatever the device; kMono folds it, then plays the same signal on
/// both sides (one earphone, or one speaker).
enum class OutputChannels { kAuto, kStereo, kMono };
OutputChannels GetOutputChannels();

/// The game's window lost or got the focus: with audio_mute_unfocused the output
/// stage plays silence while it is in the background. Safe from any thread.
void SetWindowInBackground(bool background);

/// Interleaved stereo frames made mono: each frame's two samples their mean.
void FoldStereoToMono(float* interleaved, size_t frames);

/// The output device the player chose (audio_output_device), empty for the
/// system's default one.
std::string GetOutputDeviceName();

}  // namespace rex::audio
