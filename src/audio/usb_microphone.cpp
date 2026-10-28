/**
 * @file        audio/usb_microphone.cpp
 * @brief       Xbox 360 USB microphones answered with the host's recording devices
 *
 * @license     BSD 3-Clause License, as the rest of the SDK (LICENSE in the project root). Added on the
 *              recomp branch of ElGuslml/rexglue-sdk (RECOMP.md).
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <rex/audio/usb_microphone.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <SDL3/SDL.h>

REXCVAR_DEFINE_STRING(usb_microphone_1, "", "Audio",
                      "Recording device that answers for the game's USB microphone 1: a device's name as "
                      "the system lists it, \"default\" for the system's default recording device, empty "
                      "for none");
REXCVAR_DEFINE_STRING(usb_microphone_2, "", "Audio",
                      "Recording device that answers for the game's USB microphone 2, as usb_microphone_1");
REXCVAR_DEFINE_INT32(usb_microphone_1_channel, 0, "Audio",
                     "The channel of microphone 1's recording device the game hears: 0 = all of them "
                     "mixed, 1 = the first (an interface's input 1), 2 = the second, ...")
    .range(0, 8);
REXCVAR_DEFINE_INT32(usb_microphone_2_channel, 0, "Audio",
                     "The channel of microphone 2's recording device the game hears, as "
                     "usb_microphone_1_channel")
    .range(0, 8);
REXCVAR_DEFINE_DOUBLE(usb_microphone_1_gain, 0.0, "Audio",
                      "Gain applied to microphone 1's recording, in dB (negative: quieter)")
    .range(-40.0, 20.0);
REXCVAR_DEFINE_DOUBLE(usb_microphone_2_gain, 0.0, "Audio",
                      "Gain applied to microphone 2's recording, in dB (negative: quieter)")
    .range(-40.0, 20.0);

namespace rex::audio {

namespace {

// MicDeviceRequest's request block (big-endian):
//   +0 u16 code, +2 u16 handle, +4 u32 the device's state (code 0), +12 u32 the completion routine's
//   context, +16 u32 the completion routine, +20 u32 a buffer, a sample rate or a result, +24 u32 a
//   count of packets or the bytes in each, +28 the USB setup packet of a control transfer.
constexpr uint32_t kOffsetCode = 0;
constexpr uint32_t kOffsetHandle = 2;
constexpr uint32_t kOffsetState = 4;
constexpr uint32_t kOffsetRoutine = 16;
constexpr uint32_t kOffsetArgument = 20;
constexpr uint32_t kOffsetCount = 24;
constexpr uint32_t kOffsetSetup = 28;

constexpr uint16_t kCodeState = 0;
constexpr uint16_t kCodeOpen = 1;
constexpr uint16_t kCodeControl = 2;
constexpr uint16_t kCodeSetRate = 4;
constexpr uint16_t kCodeRead = 7;
constexpr uint16_t kCodeRates = 8;

// Code 0's answer: the library opens a device in states 1-3 and reads one in state 5.
constexpr uint32_t kStateNone = 0;
constexpr uint32_t kStateAttached = 1;
constexpr uint32_t kStateOpen = 5;

// XamUserGetDeviceContext's context: the device's class in the top nibble (6, a microphone; 5 is the
// wireless one, whose packets are compressed), its handle in the low 16 bits.
constexpr uint32_t kClassMicrophone = 0x60000000;

// Code 8's answer, the sample rates the device takes: 64 = 32 kHz, 256 = 48 kHz.
constexpr uint32_t kRatesMask = 64 | 256;
// The packets a read asks for at most (the library's limit), 1 ms of sound each.
constexpr uint32_t kMaxPackets = 8;

constexpr X_STATUS kStatusDeviceNotConnected = 0xC000009D;

// USB Audio Class 1.0: a feature unit's volume control, int16 in 1/256 dB. A fixed one: Guitar Hero turns
// its USB microphone down to the bottom of its range when a song starts (Vocals_SetMicGain with a gain of
// 0, 0.5 or 0.6 by the kind of microphone), as its Logitech microphone needs; how far down a PC microphone
// should go depends on the microphone, so the player sets it (usb_microphone_N_gain, with a launcher's level
// meter) and the device's range is 0 to 0 dB.
constexpr uint8_t kSelectorMute = 1;
constexpr uint8_t kSelectorVolume = 2;
constexpr uint8_t kRequestSetCur = 0x01;
constexpr uint8_t kRequestGetCur = 0x81;
constexpr uint8_t kRequestGetMin = 0x82;
constexpr uint8_t kRequestGetMax = 0x83;
constexpr uint8_t kRequestGetRes = 0x84;
constexpr int16_t kVolumeMin = 0;
constexpr int16_t kVolumeMax = 0;
constexpr int16_t kVolumeRes = 256;

constexpr auto kPresenceInterval = std::chrono::seconds(1);
// The log's look at what the game hears: the loudest sample of its first seconds of reading.
constexpr uint32_t kLevelReportMs = 5000;

const std::string& DeviceSetting(uint32_t index) {
  return index == 0 ? REXCVAR_GET(usb_microphone_1) : REXCVAR_GET(usb_microphone_2);
}

uint32_t ChannelSetting(uint32_t index) {
  return static_cast<uint32_t>(index == 0 ? REXCVAR_GET(usb_microphone_1_channel)
                                          : REXCVAR_GET(usb_microphone_2_channel));
}

float GainSetting(uint32_t index) {
  const double db = index == 0 ? REXCVAR_GET(usb_microphone_1_gain) : REXCVAR_GET(usb_microphone_2_gain);
  return static_cast<float>(std::pow(10.0, db / 20.0));
}

bool IsDefault(const std::string& device) {
  return device == "default";
}

// The recording device a setting names, 0 if the system has none of that name.
SDL_AudioDeviceID FindDevice(const std::string& device) {
  if (IsDefault(device)) {
    int count = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&count);
    SDL_free(ids);
    return count > 0 ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING : 0;
  }
  int count = 0;
  SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&count);
  SDL_AudioDeviceID found = 0;
  for (int i = 0; ids && i < count; ++i) {
    const char* name = SDL_GetAudioDeviceName(ids[i]);
    if (name && device == name) {
      found = ids[i];
      break;
    }
  }
  SDL_free(ids);
  return found;
}

void SDLCALL OnRecorded(void* userdata, SDL_AudioStream*, int, int) {
  static_cast<rex::thread::Event*>(userdata)->Set();
}

}  // namespace

UsbMicrophones::UsbMicrophones(runtime::FunctionDispatcher* function_dispatcher)
    : memory_(function_dispatcher->memory()), function_dispatcher_(function_dispatcher) {}

UsbMicrophones::~UsbMicrophones() {
  Shutdown();
}

void UsbMicrophones::Setup(system::KernelState* kernel_state) {
  for (uint32_t i = 0; i < kCount; ++i) {
    Slot& slot = slots_[i];
    slot.index = i;
    slot.device = DeviceSetting(i);
    slot.channel = ChannelSetting(i);
    slot.gain = GainSetting(i);
    slot.configured = !slot.device.empty();
    enabled_ = enabled_ || slot.configured;
  }
  if (!enabled_) {
    return;
  }
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    REXAPU_ERROR("USB microphones: SDL_InitSubSystem(SDL_INIT_AUDIO) failed: {}", SDL_GetError());
    enabled_ = false;
    return;
  }

  // The packets' statuses (u32) and lengths (u16) a read's completion routine takes.
  packet_statuses_ptr_ = memory_->SystemHeapAlloc(kMaxPackets * 4);
  packet_lengths_ptr_ = memory_->SystemHeapAlloc(kMaxPackets * 2);
  std::memset(memory_->TranslateVirtual<uint8_t*>(packet_statuses_ptr_), 0, kMaxPackets * 4);

  RefreshPresence();

  work_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  worker_running_ = true;
  worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state, 128 * 1024, 0, [this]() {
        WorkerThreadMain();
        return 0;
      }));
  worker_thread_->set_name("USB Microphones");
  worker_thread_->Create();
}

void UsbMicrophones::Shutdown() {
  if (!enabled_) {
    return;
  }
  if (worker_thread_) {
    worker_running_ = false;
    work_event_->Set();
    auto result =
        rex::thread::Wait(worker_thread_->thread(), false, std::chrono::milliseconds(2000));
    if (result == rex::thread::WaitResult::kTimeout) {
      REXAPU_WARN("USB microphones: the worker thread did not exit within 2 s, abandoning it");
    }
    worker_thread_.reset();
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Slot& slot : slots_) {
      CloseStream(slot);
      slot.reads.clear();
    }
    pending_.clear();
  }
  if (packet_statuses_ptr_) {
    memory_->SystemHeapFree(packet_statuses_ptr_);
    memory_->SystemHeapFree(packet_lengths_ptr_);
    packet_statuses_ptr_ = 0;
    packet_lengths_ptr_ = 0;
  }
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
  enabled_ = false;
}

UsbMicrophones::Presence UsbMicrophones::DeviceContext(uint32_t index, uint32_t* out_context) {
  if (!enabled_ || index >= kCount || !slots_[index].configured) {
    return Presence::kNotConfigured;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const Slot& slot = slots_[index];
  if (!slot.present) {
    return Presence::kDisconnected;
  }
  *out_context = kClassMicrophone | slot.handle;
  return Presence::kConnected;
}

X_STATUS UsbMicrophones::Request(uint32_t request_ptr) {
  if (!enabled_ || !request_ptr) {
    return kStatusDeviceNotConnected;
  }
  auto* block = memory_->TranslateVirtual<uint8_t*>(request_ptr);
  const uint16_t code = memory::load_and_swap<uint16_t>(block + kOffsetCode);
  const uint16_t handle = memory::load_and_swap<uint16_t>(block + kOffsetHandle);

  std::lock_guard<std::mutex> lock(mutex_);
  Slot* slot = nullptr;
  for (Slot& candidate : slots_) {
    if (candidate.present && candidate.handle == handle) {
      slot = &candidate;
    }
  }

  // The library asks every frame; the only request it waits for in place.
  if (code == kCodeState) {
    const uint32_t state = !slot ? kStateNone : slot->open ? kStateOpen : kStateAttached;
    memory::store_and_swap<uint32_t>(block + kOffsetState, state);
    return X_STATUS_SUCCESS;
  }
  if (!slot) {
    return kStatusDeviceNotConnected;
  }

  switch (code) {
    case kCodeOpen:
    case kCodeControl:
    case kCodeSetRate:
    case kCodeRates:
      pending_.push_back({request_ptr, slot->index, code});
      break;
    case kCodeRead: {
      const uint32_t count = memory::load_and_swap<uint32_t>(block + kOffsetCount);
      if (!slot->open || count == 0 || count > kMaxPackets) {
        return kStatusDeviceNotConnected;
      }
      slot->reads.push_back(request_ptr);
      break;
    }
    default:
      REXAPU_WARN("USB microphone {}: request code {} is not supported", slot->index + 1, code);
      return X_STATUS_NOT_SUPPORTED;
  }
  // The other requests end in the completion routine, called from the worker thread as the console's
  // USB stack calls it once the transfer is done: the routine of an open goes on with the next request
  // of the library's sequence (the sample rate, then the volume control's range and value).
  work_event_->Set();
  return X_STATUS_PENDING;
}

void UsbMicrophones::WorkerThreadMain() {
  auto next_presence = std::chrono::steady_clock::now() + kPresenceInterval;
  std::vector<uint32_t> failed_reads;
  while (worker_running_) {
    rex::thread::Wait(work_event_.get(), false, std::chrono::milliseconds(20));
    if (!worker_running_) {
      break;
    }

    if (std::chrono::steady_clock::now() >= next_presence) {
      RefreshPresence();
      next_presence = std::chrono::steady_clock::now() + kPresenceInterval;
    }

    {
      // Reads of a device that went away end with an error, as on the console.
      std::lock_guard<std::mutex> lock(mutex_);
      for (Slot& slot : slots_) {
        if (!slot.present || !slot.open) {
          failed_reads.insert(failed_reads.end(), slot.reads.begin(), slot.reads.end());
          slot.reads.clear();
        }
      }
    }
    for (uint32_t request_ptr : failed_reads) {
      CallCompletion(request_ptr, kStatusDeviceNotConnected, packet_statuses_ptr_,
                     packet_lengths_ptr_);
    }
    failed_reads.clear();

    // In order; a completion routine can queue the next request, taken in this same pass.
    for (;;) {
      Pending pending;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_.empty()) {
          break;
        }
        pending = pending_.front();
        pending_.pop_front();
      }
      Complete(pending);
    }

    std::unique_lock<std::mutex> lock(mutex_);
    for (Slot& slot : slots_) {
      while (CompleteRead(slot, lock)) {
      }
    }
  }
}

void UsbMicrophones::RefreshPresence() {
  std::array<bool, kCount> present = {};
  for (uint32_t i = 0; i < kCount; ++i) {
    present[i] = slots_[i].configured && FindDevice(slots_[i].device) != 0;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (uint32_t i = 0; i < kCount; ++i) {
    Slot& slot = slots_[i];
    if (present[i] == slot.present) {
      continue;
    }
    slot.present = present[i];
    slot.open = false;
    CloseStream(slot);
    if (slot.present) {
      // A device plugged in again is a new device to the game, with a handle of its own.
      slot.handle = next_handle_++;
      if (next_handle_ == 0) {
        next_handle_ = 1;
      }
      REXAPU_INFO("USB microphone {}: connected ({})", i + 1,
                  IsDefault(slot.device) ? "the default recording device" : slot.device);
    } else {
      REXAPU_INFO("USB microphone {}: no recording device {}", i + 1,
                  IsDefault(slot.device) ? "at all" : "named " + slot.device);
    }
  }
}

bool UsbMicrophones::OpenStream(Slot& slot) {
  const SDL_AudioDeviceID device = FindDevice(slot.device);
  if (!device) {
    return false;
  }
  SDL_AudioSpec device_spec = {};
  int device_frames = 0;
  if (!SDL_GetAudioDeviceFormat(device, &device_spec, &device_frames)) {
    device_spec.channels = 1;
  }
  slot.stream_channels = std::clamp(device_spec.channels, 1, 8);
  // The device's own channels, so that one of them can be taken alone (a microphone on one input of
  // an interface); SDL converts the format and the rate.
  SDL_AudioSpec spec = {};
  spec.format = SDL_AUDIO_S16;
  spec.channels = slot.stream_channels;
  spec.freq = static_cast<int>(slot.rate);
  slot.stream = SDL_OpenAudioDeviceStream(device, &spec, OnRecorded, work_event_.get());
  if (!slot.stream) {
    REXAPU_ERROR("USB microphone {}: SDL_OpenAudioDeviceStream failed: {}", slot.index + 1,
                 SDL_GetError());
    return false;
  }
  const char* name = SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(slot.stream));
  slot.stream_name = name ? name : slot.device;
  SDL_ResumeAudioStreamDevice(slot.stream);
  REXAPU_INFO("USB microphone {}: opened, {} ({} Hz, {} channels, {} frames a period), {}",
              slot.index + 1, slot.stream_name, device_spec.freq, slot.stream_channels, device_frames,
              slot.channel == 0 || slot.stream_channels == 1
                  ? std::string("all channels")
                  : "channel " + std::to_string(std::min<int>(slot.channel, slot.stream_channels)));
  return true;
}

void UsbMicrophones::CloseStream(Slot& slot) {
  if (slot.stream) {
    SDL_DestroyAudioStream(slot.stream);
    slot.stream = nullptr;
  }
}

void UsbMicrophones::Complete(const Pending& pending) {
  X_STATUS status = X_STATUS_SUCCESS;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot& slot = slots_[pending.slot];
    auto* block = memory_->TranslateVirtual<uint8_t*>(pending.request_ptr);
    if (!slot.present) {
      status = kStatusDeviceNotConnected;
    } else {
      switch (pending.code) {
        case kCodeOpen:
          CloseStream(slot);
          slot.open = OpenStream(slot);
          slot.level_ms = 0;
          slot.level_peak = 0;
          status = slot.open ? X_STATUS_SUCCESS : kStatusDeviceNotConnected;
          break;
        case kCodeSetRate: {
          const uint32_t rate = memory::load_and_swap<uint32_t>(block + kOffsetArgument);
          const uint32_t packet_bytes = memory::load_and_swap<uint32_t>(block + kOffsetCount);
          if (rate != 48000 && rate != 32000) {
            status = X_STATUS_INVALID_PARAMETER;
            break;
          }
          slot.rate = rate;
          slot.packet_bytes = packet_bytes ? packet_bytes : rate / 1000 * 2;
          if (slot.stream) {
            SDL_AudioSpec spec = {};
            spec.format = SDL_AUDIO_S16;
            spec.channels = slot.stream_channels;
            spec.freq = static_cast<int>(rate);
            SDL_SetAudioStreamFormat(slot.stream, nullptr, &spec);
            SDL_ClearAudioStream(slot.stream);
          }
          REXAPU_INFO("USB microphone {}: {} Hz, {} bytes a packet", slot.index + 1, rate,
                      slot.packet_bytes);
          break;
        }
        case kCodeRates:
          memory::store_and_swap<uint32_t>(block + kOffsetArgument, kRatesMask);
          break;
        case kCodeControl:
          status = Control(slot, pending.request_ptr);
          break;
      }
    }
  }
  CallCompletion(pending.request_ptr, status, 0, 0);
}

X_STATUS UsbMicrophones::Control(Slot& slot, uint32_t request_ptr) {
  auto* block = memory_->TranslateVirtual<uint8_t*>(request_ptr);
  const uint32_t data_ptr = memory::load_and_swap<uint32_t>(block + kOffsetArgument);
  const uint8_t request_type = block[kOffsetSetup + 0];
  const uint8_t request = block[kOffsetSetup + 1];
  const uint16_t value = memory::load_and_swap<uint16_t>(block + kOffsetSetup + 2);
  const uint16_t length = memory::load_and_swap<uint16_t>(block + kOffsetSetup + 6);
  const uint8_t selector = static_cast<uint8_t>(value >> 8);
  if (!data_ptr || length < 2 || (selector != kSelectorVolume && selector != kSelectorMute)) {
    return X_STATUS_UNSUCCESSFUL;
  }
  // The data stage in USB's order, little-endian.
  auto* data = memory_->TranslateVirtual<uint8_t*>(data_ptr);
  if (request_type & 0x80) {
    int16_t answer = 0;
    switch (request) {
      case kRequestGetCur:
        answer = selector == kSelectorVolume ? slot.volume : 0;
        break;
      case kRequestGetMin:
        answer = kVolumeMin;
        break;
      case kRequestGetMax:
        answer = kVolumeMax;
        break;
      case kRequestGetRes:
        answer = kVolumeRes;
        break;
      default:
        return X_STATUS_UNSUCCESSFUL;
    }
    data[0] = static_cast<uint8_t>(answer & 0xFF);
    data[1] = static_cast<uint8_t>((answer >> 8) & 0xFF);
    return X_STATUS_SUCCESS;
  }
  if (request != kRequestSetCur) {
    return X_STATUS_UNSUCCESSFUL;
  }
  if (selector == kSelectorVolume) {
    const auto volume = static_cast<int16_t>(data[0] | (data[1] << 8));
    slot.volume = std::clamp(volume, kVolumeMin, kVolumeMax);
  }
  return X_STATUS_SUCCESS;
}

bool UsbMicrophones::CompleteRead(Slot& slot, std::unique_lock<std::mutex>& lock) {
  if (slot.reads.empty() || !slot.stream) {
    return false;
  }
  const uint32_t request_ptr = slot.reads.front();
  auto* block = memory_->TranslateVirtual<uint8_t*>(request_ptr);
  const uint32_t count = memory::load_and_swap<uint32_t>(block + kOffsetCount);
  const uint32_t buffer_ptr = memory::load_and_swap<uint32_t>(block + kOffsetArgument);
  const uint32_t frames = count * (slot.packet_bytes / 2);
  const int frame_bytes = 2 * slot.stream_channels;

  // The microphone's own clock paces the reads: one ends when the device has recorded its packets.
  // What the game did not ask for in time (before it starts reading, or while it does not) is dropped
  // down to two reads' worth, so that the singing reaches it late by no more than that.
  int available = SDL_GetAudioStreamAvailable(slot.stream) / frame_bytes;
  std::vector<int16_t> samples;
  if (available > static_cast<int>(frames * 5)) {
    const int drop = available - static_cast<int>(frames * 2);
    samples.resize(static_cast<size_t>(drop) * slot.stream_channels);
    SDL_GetAudioStreamData(slot.stream, samples.data(), drop * frame_bytes);
    available -= drop;
  }
  if (available < static_cast<int>(frames)) {
    return false;
  }
  samples.resize(static_cast<size_t>(frames) * slot.stream_channels);
  SDL_GetAudioStreamData(slot.stream, samples.data(), static_cast<int>(frames) * frame_bytes);

  // One channel or their mean, at the player's gain.
  const uint32_t channels = static_cast<uint32_t>(slot.stream_channels);
  const uint32_t only = std::min(slot.channel, channels);
  auto* out = memory_->TranslateVirtual<uint8_t*>(buffer_ptr);
  for (uint32_t i = 0; i < frames; ++i) {
    const int16_t* frame = &samples[static_cast<size_t>(i) * channels];
    float mixed = 0.0f;
    if (only != 0) {
      mixed = frame[only - 1];
    } else {
      for (uint32_t c = 0; c < channels; ++c) {
        mixed += frame[c];
      }
      mixed /= static_cast<float>(channels);
    }
    const auto sample = static_cast<int32_t>(std::clamp(mixed * slot.gain, -32768.0f, 32767.0f));
    slot.level_peak = std::max(slot.level_peak, std::abs(sample));
    // Little-endian, as a USB device sends it; the library swaps it.
    out[i * 2 + 0] = static_cast<uint8_t>(sample & 0xFF);
    out[i * 2 + 1] = static_cast<uint8_t>((sample >> 8) & 0xFF);
  }
  if (slot.level_ms == 0) {
    REXAPU_INFO("USB microphone {}: the game reads it, {} packets a read", slot.index + 1, count);
  }
  if (slot.level_ms < kLevelReportMs && (slot.level_ms += count) >= kLevelReportMs) {
    REXAPU_INFO("USB microphone {}: loudest sample of the first {} s read: {:.1f} dBFS", slot.index + 1,
                kLevelReportMs / 1000,
                20.0 * std::log10(std::max(slot.level_peak, 1) / 32768.0));
  }

  auto* lengths = memory_->TranslateVirtual<uint8_t*>(packet_lengths_ptr_);
  for (uint32_t i = 0; i < count; ++i) {
    memory::store_and_swap<uint16_t>(lengths + i * 2, static_cast<uint16_t>(slot.packet_bytes));
  }
  slot.reads.pop_front();

  lock.unlock();
  CallCompletion(request_ptr, X_STATUS_SUCCESS, packet_statuses_ptr_, packet_lengths_ptr_);
  lock.lock();
  return true;
}

void UsbMicrophones::CallCompletion(uint32_t request_ptr, X_STATUS status, uint32_t statuses_ptr,
                                    uint32_t lengths_ptr) {
  auto* block = memory_->TranslateVirtual<uint8_t*>(request_ptr);
  const uint32_t routine = memory::load_and_swap<uint32_t>(block + kOffsetRoutine);
  if (!routine) {
    return;
  }
  // routine(request, status, the packets' statuses, the packets' lengths); the last two for reads.
  uint64_t args[] = {request_ptr, static_cast<uint32_t>(status), statuses_ptr, lengths_ptr};
  function_dispatcher_->Execute(worker_thread_->thread_state(), routine, args, rex::countof(args));
}

}  // namespace rex::audio
