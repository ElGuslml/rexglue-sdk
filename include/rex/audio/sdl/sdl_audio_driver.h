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

#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <queue>
#include <stack>

#include <rex/audio/audio_driver.h>
#include <rex/thread.h>

#include <SDL3/SDL.h>

namespace rex::audio::sdl {

class SDLAudioDriver : public AudioDriver {
 public:
  SDLAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore);
  ~SDLAudioDriver() override;

  bool Initialize();
  // Whether the output the driver would open (audio_output_device's, else Windows' default) has more than two
  // channels, before it opens: OutputPlaysSurround's probe.
  static bool ProbeOutputSurround();
  void SubmitFrame(uint32_t frame_ptr) override;
  bool CatchingUp() const override { return owed_frames_.load(std::memory_order_relaxed) != 0; }
  void Shutdown();

 protected:
  static void SDLCallback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                          int total_amount);

  rex::thread::Semaphore* semaphore_ = nullptr;

  SDL_AudioStream* sdl_stream_ = nullptr;
  bool sdl_initialized_ = false;
  uint8_t sdl_device_channels_ = 0;
  bool mono_ = false;  // audio_channels = mono: the stereo fold played on both sides

  static const uint32_t frame_frequency_ = 48000;
  static const uint32_t frame_channels_ = 6;
  static const uint32_t channel_samples_ = 256;
  static const uint32_t frame_samples_ = frame_channels_ * channel_samples_;
  static const uint32_t frame_size_ = sizeof(float) * frame_samples_;
  std::queue<float*> frames_queued_ = {};
  std::stack<float*> frames_unused_ = {};
  std::mutex frames_mutex_ = {};

  // audio_keep_time: frames of silence played for want of the client's (while it submits), each owing one of its
  // next frames to drop; the frames dropped so far in this catch-up; the client's last frame.
  std::atomic<uint32_t> owed_frames_{0};
  uint32_t dropped_frames_ = 0;
  // The most frames SDL has taken in one call (2 on a 10 ms WASAPI period): as many stay queued while catching up.
  std::atomic<uint32_t> burst_frames_{1};
  std::chrono::steady_clock::time_point last_submit_ = {};

  // audio_keep_time_device_gaps: the frames given to the device against the steady clock, read and written by the
  // device's callback only. Each call's time less the time its frames so far last; its least value over a window is
  // where the device stands, and a rise from one window to the next is a gap in the device's output (WatchDeviceGaps).
  void WatchDeviceGaps(std::chrono::steady_clock::time_point now);
  uint64_t given_frames_ = 0;
  std::chrono::steady_clock::time_point given_since_ = {};
  std::chrono::steady_clock::time_point gap_window_since_ = {};
  uint32_t gap_window_calls_ = 0;
  double gap_window_least_ = 0.0;
  double gap_last_least_ = 0.0;
  bool gap_last_valid_ = false;
  double gap_carry_ms_ = 0.0;  // of the gaps made up so far, the part not in whole frames
};

}  // namespace rex::audio::sdl
