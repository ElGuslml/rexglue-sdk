#pragma once
/**
 * @file        rex/audio/wasapi/wasapi_exclusive_driver.h
 * @brief       The sound through WASAPI in exclusive mode (Windows), beside the SDL driver
 *
 * @license     BSD 3-Clause License, as the rest of the SDK (LICENSE in the project root). Added on the
 *              recomp branch of ElGuslml/rexglue-sdk (RECOMP.md).
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <stack>

#include <rex/audio/audio_driver.h>
#include <rex/thread.h>

namespace rex::audio::wasapi {

// audio_wasapi_exclusive: the default output device opened in exclusive mode, its buffer kept a few milliseconds
// ahead (audio_wasapi_exclusive_buffer_ms) from the client's frames (the console's 6 channels at 48 kHz, folded to stereo as the SDL driver does). Windows' audio engine
// and the device's shared path are skipped: on a USB interface that path alone took 43-51 ms. Other programs cannot
// play on the device meanwhile. The device is opened once (Windows lets one stream hold it): a game with more than one
// audio client (Guitar Hero 5 and Band Hero register the voice chat engine's beside the game's) has the frames of
// each client's driver mixed into it.
class WasapiExclusiveDriver : public AudioDriver {
 public:
  WasapiExclusiveDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore);
  ~WasapiExclusiveDriver() override;

  // Joins the output another client opened, or opens the device and starts the render thread; false when the device
  // refuses (another program holds it, exclusive mode not allowed, none of the formats): the caller uses the shared
  // output instead.
  bool Initialize();
  void SubmitFrame(uint32_t frame_ptr) override;
  bool CatchingUp() const override { return owed_frames_.load(std::memory_order_relaxed) != 0; }
  uint32_t QueuedFrames(uint32_t asked) const override;
  void Shutdown();

  // The device and its render thread, shared by the clients' drivers (wasapi_exclusive_driver.cpp).
  class Output;

 private:
  // Called by the output's render thread. Adds `frames` of the client's sound to `out` (interleaved stereo); the
  // rest of them silent when none is queued.
  void Mix(float* out, uint32_t frames);
  // Takes the next client frame into the stereo staging buffer; false when none is queued.
  bool NextFrame();
  void NoteSilence(uint32_t samples);

  static constexpr uint32_t kChannels = 6;
  static constexpr uint32_t kChannelSamples = 256;
  static constexpr uint32_t kFrameSamples = kChannels * kChannelSamples;

  rex::thread::Semaphore* semaphore_ = nullptr;
  std::queue<float*> frames_queued_;
  std::stack<float*> frames_unused_;
  std::mutex frames_mutex_;

  // The frame being played, folded to interleaved stereo, and how much of it has gone to the device (the render
  // thread's).
  float staging_[kChannelSamples * 2] = {};
  uint32_t staging_read_ = kChannelSamples;

  // audio_keep_time, as the SDL driver's.
  std::atomic<uint32_t> owed_frames_{0};
  uint32_t silence_samples_ = 0;
  uint32_t dropped_frames_ = 0;
  std::chrono::steady_clock::time_point last_submit_ = {};

  std::shared_ptr<Output> output_;
};

}  // namespace rex::audio::wasapi
