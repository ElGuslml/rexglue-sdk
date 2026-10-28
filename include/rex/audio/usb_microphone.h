#pragma once
/**
 * @file        rex/audio/usb_microphone.h
 * @brief       Xbox 360 USB microphones answered with the host's recording devices
 *
 * @license     BSD 3-Clause License, as the rest of the SDK (LICENSE in the project root). Added on the
 *              recomp branch of ElGuslml/rexglue-sdk (RECOMP.md).
 */

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include <rex/system/function_dispatcher.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/thread.h>

struct SDL_AudioStream;

namespace rex::audio {

// The XDK's USB microphone library (the USB Audio Class microphones of Guitar Hero and Rock Band, a
// Logitech one among them) finds a microphone with XamUserGetDeviceContext(index, 4), whose context
// is the device's class (6, a microphone) and handle, and drives it with MicDeviceRequest: a request
// block with a code, the handle, the code's arguments and a completion routine the kernel calls when
// the request ends. `usb_microphone_1` and `usb_microphone_2` give the game's microphones 1 and 2 a
// recording device of the host each; with both empty (the default) nothing here runs.
class UsbMicrophones {
 public:
  static constexpr uint32_t kCount = 2;
  // XamUserGetDeviceContext's device type for a USB microphone.
  static constexpr uint32_t kDeviceType = 4;

  explicit UsbMicrophones(runtime::FunctionDispatcher* function_dispatcher);
  ~UsbMicrophones();

  void Setup(system::KernelState* kernel_state);
  void Shutdown();

  bool enabled() const { return enabled_; }

  enum class Presence { kNotConfigured, kConnected, kDisconnected };
  // XamUserGetDeviceContext(index, kDeviceType): the device's context while it is connected.
  Presence DeviceContext(uint32_t index, uint32_t* out_context);

  // MicDeviceRequest: X_STATUS_PENDING when the completion routine will be called.
  X_STATUS Request(uint32_t request_ptr);

 private:
  struct Pending {
    uint32_t request_ptr;
    uint32_t slot;
    uint16_t code;
  };

  struct Slot {
    uint32_t index = 0;
    std::string device;  // the cvar's value: a device's name, or "default"
    uint32_t channel = 0;  // 0 = the device's channels mixed, n = channel n only
    float gain = 1.0f;     // usb_microphone_N_gain, as a factor
    bool configured = false;
    bool present = false;
    uint16_t handle = 0;
    bool open = false;
    uint32_t rate = 48000;
    uint32_t packet_bytes = 96;
    int16_t volume = 0;  // 1/256 dB, as the game set it
    uint32_t level_ms = 0;  // read since the open, up to the level's report
    int32_t level_peak = 0;
    SDL_AudioStream* stream = nullptr;
    int stream_channels = 1;
    std::string stream_name;
    std::deque<uint32_t> reads;
  };

  void WorkerThreadMain();
  void RefreshPresence();
  bool OpenStream(Slot& slot);
  void CloseStream(Slot& slot);
  void Complete(const Pending& pending);
  bool CompleteRead(Slot& slot, std::unique_lock<std::mutex>& lock);
  void CallCompletion(uint32_t request_ptr, X_STATUS status, uint32_t statuses_ptr,
                      uint32_t lengths_ptr);
  X_STATUS Control(Slot& slot, uint32_t request_ptr);

  memory::Memory* memory_ = nullptr;
  runtime::FunctionDispatcher* function_dispatcher_ = nullptr;
  bool enabled_ = false;

  std::mutex mutex_;
  std::array<Slot, kCount> slots_;
  std::deque<Pending> pending_;
  uint16_t next_handle_ = 1;
  uint32_t packet_statuses_ptr_ = 0;
  uint32_t packet_lengths_ptr_ = 0;

  std::atomic<bool> worker_running_ = {false};
  std::unique_ptr<rex::thread::Event> work_event_;
  system::object_ref<system::XHostThread> worker_thread_;
};

}  // namespace rex::audio
