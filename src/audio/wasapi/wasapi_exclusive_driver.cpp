/**
 * @file        audio/wasapi/wasapi_exclusive_driver.cpp
 * @brief       The sound through WASAPI in exclusive mode (Windows), beside the SDL driver
 *
 * @license     BSD 3-Clause License, as the rest of the SDK (LICENSE in the project root). Added on the
 *              recomp branch of ElGuslml/rexglue-sdk (RECOMP.md).
 */

#include <rex/audio/wasapi/wasapi_exclusive_driver.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <rex/audio/conversion.h>
#include <rex/audio/downmix.h>
#include <rex/audio/flags.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

REXCVAR_DEFINE_BOOL(audio_wasapi_exclusive, false, "Audio",
                    "Windows: the sound through WASAPI in exclusive mode on the default output device, a few "
                    "milliseconds ahead (audio_wasapi_exclusive_buffer_ms), past Windows' audio engine (much less delay on most devices; other programs "
                    "cannot play on it meanwhile). The shared output (SDL) when the device refuses");
REXCVAR_DEFINE_INT32(audio_wasapi_exclusive_buffer_ms, 10, "Audio",
                     "audio_wasapi_exclusive: how much sound is kept in the device's buffer, in milliseconds: less "
                     "is less delay, too little and a device that takes its sound in larger blocks plays gaps")
    .range(2, 50);
REXCVAR_DEFINE_INT32(audio_wasapi_exclusive_queue, 0, "Audio",
                     "audio_wasapi_exclusive: the game's frames (5.3 ms each) queued for the device, in place of "
                     "audio_maxqframes (0 = the same; at least 4 there): exclusive output takes them steadily, so 2 "
                     "is about 10 ms less delay than 4")
    .range(0, 64);

REXCVAR_DECLARE(bool, audio_keep_time);

namespace rex::audio::wasapi {

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kMaxOwedFrames = 375;  // as the SDL driver's
constexpr auto kClientActive = std::chrono::milliseconds(500);

// KSDATAFORMAT_SUBTYPE_PCM and _IEEE_FLOAT (ksmedia.h's need ksuser.lib or INITGUID).
constexpr GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// PKEY_Device_FriendlyName (functiondiscoverykeys_devpkey.h needs propkeydef.h's macros).
constexpr PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

// The sample formats tried, best first: the container's bits, the valid bits, float or integer.
struct SampleFormat {
  WORD container;
  WORD valid;
  bool is_float;
  const char* name;
};
constexpr SampleFormat kFormats[] = {
    {32, 32, true, "32-bit float"},
    {32, 32, false, "32-bit"},
    {32, 24, false, "24-bit in 32"},
    {24, 24, false, "24-bit"},
    {16, 16, false, "16-bit"},
};

WAVEFORMATEXTENSIBLE MakeFormat(const SampleFormat& sample) {
  WAVEFORMATEXTENSIBLE format = {};
  format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  format.Format.nChannels = 2;
  format.Format.nSamplesPerSec = kRate;
  format.Format.wBitsPerSample = sample.container;
  format.Format.nBlockAlign = WORD(2 * sample.container / 8);
  format.Format.nAvgBytesPerSec = kRate * format.Format.nBlockAlign;
  format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  format.Samples.wValidBitsPerSample = sample.valid;
  format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  format.SubFormat = sample.is_float ? kSubtypeFloat : kSubtypePcm;
  return format;
}

// One sample to the device's format.
void WriteSample(uint8_t*& out, float value, const SampleFormat& sample) {
  value = std::clamp(value, -1.0f, 1.0f);
  if (sample.is_float) {
    std::memcpy(out, &value, 4);
  } else if (sample.container == 32) {
    const int32_t v = int32_t(std::lround(double(value) * 2147483647.0));
    std::memcpy(out, &v, 4);
  } else if (sample.container == 24) {
    const int32_t v = int32_t(std::lround(double(value) * 8388607.0));
    out[0] = uint8_t(v);
    out[1] = uint8_t(v >> 8);
    out[2] = uint8_t(v >> 16);
  } else {
    const int16_t v = int16_t(std::lround(double(value) * 32767.0));
    std::memcpy(out, &v, 2);
  }
  out += sample.container / 8;
}

std::string ToUtf8(const wchar_t* text) {
  if (!text) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  std::string result(size > 0 ? size - 1 : 0, '\0');
  if (size > 1) {
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
  }
  return result;
}

void JoinProAudio() {
  using SetCharacteristics = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
  static const auto set_characteristics = [] {
    HMODULE avrt = LoadLibraryW(L"avrt.dll");
    return avrt ? reinterpret_cast<SetCharacteristics>(GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW"))
                : nullptr;
  }();
  DWORD task = 0;
  if (!set_characteristics || !set_characteristics(L"Pro Audio", &task)) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  }
}

}  // namespace

struct WasapiExclusiveDriver::Device {
  ComPtr<IAudioClient> client;
  ComPtr<IAudioRenderClient> render;
  uint32_t buffer_frames = 0;  // the device's buffer
  uint32_t target_frames = 0;  // kept in it (audio_wasapi_exclusive_buffer_ms)
  SampleFormat sample = kFormats[0];

  ~Device() {
    if (client) {
      client->Stop();
    }
  }
};

WasapiExclusiveDriver::WasapiExclusiveDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

WasapiExclusiveDriver::~WasapiExclusiveDriver() {
  Shutdown();
}

bool WasapiExclusiveDriver::Initialize() {
  std::promise<bool> opened;
  std::future<bool> result = opened.get_future();
  thread_ = std::thread([this, opened = std::move(opened)]() mutable { RenderThread(std::move(opened)); });
  if (result.get()) {
    return true;
  }
  thread_.join();
  return false;
}

void WasapiExclusiveDriver::Shutdown() {
  stop_ = true;
  if (thread_.joinable()) {
    thread_.join();
  }
  std::scoped_lock lock(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  }
  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  }
}

void WasapiExclusiveDriver::RenderThread(std::promise<bool> opened) {
  const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  JoinProAudio();
  Device device;
  HRESULT hr = E_FAIL;
  const char* step = "the default output device";
  [&] {
    ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return;
    ComPtr<IMMDevice> endpoint;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint);
    if (FAILED(hr)) return;
    ComPtr<IPropertyStore> properties;
    if (SUCCEEDED(endpoint->OpenPropertyStore(STGM_READ, &properties))) {
      PROPVARIANT name;
      PropVariantInit(&name);
      if (SUCCEEDED(properties->GetValue(kFriendlyName, &name)) && name.vt == VT_LPWSTR) {
        device_name_ = ToUtf8(name.pwszVal);
      }
      PropVariantClear(&name);
    }
    step = "exclusive mode";
    hr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &device.client);
    if (FAILED(hr)) return;
    step = "a format (48 kHz stereo)";
    WAVEFORMATEXTENSIBLE format = {};
    hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
    for (const SampleFormat& sample : kFormats) {
      format = MakeFormat(sample);
      if (device.client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &format.Format, nullptr) == S_OK) {
        device.sample = sample;
        hr = S_OK;
        break;
      }
    }
    if (FAILED(hr)) return;
    step = "exclusive mode";
    REFERENCE_TIME default_period = 0, period = 0;
    hr = device.client->GetDevicePeriod(&default_period, &period);
    if (FAILED(hr)) return;
    // Pushed rather than event driven: a driver may signal far less often than the period it accepts (a Focusrite
    // Scarlett signalled every 7 ms for a 3 ms period, so a stream refilled one period per event played 43 % of the
    // sound). The buffer holds 40 ms; the render thread keeps audio_wasapi_exclusive_buffer_ms of it filled.
    REFERENCE_TIME duration = 400000;
    hr = device.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, duration, period, &format.Format, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
      // The size the device aligns its buffer to, and a new client for it.
      UINT32 frames = 0;
      device.client->GetBufferSize(&frames);
      duration = REFERENCE_TIME(10000000.0 * frames / kRate + 0.5);
      device.client.Reset();
      hr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &device.client);
      if (FAILED(hr)) return;
      hr = device.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, duration, period, &format.Format, nullptr);
    }
    if (FAILED(hr)) return;
    UINT32 buffer_frames = 0;
    hr = device.client->GetBufferSize(&buffer_frames);
    if (FAILED(hr)) return;
    device.buffer_frames = buffer_frames;
    device.target_frames = std::min<uint32_t>(
        buffer_frames, uint32_t(REXCVAR_GET(audio_wasapi_exclusive_buffer_ms)) * kRate / 1000);
    hr = device.client->GetService(IID_PPV_ARGS(&device.render));
    if (FAILED(hr)) return;
    // The amount kept, silent, then the render thread keeps it filled.
    BYTE* data = nullptr;
    hr = device.render->GetBuffer(device.target_frames, &data);
    if (FAILED(hr)) return;
    hr = device.render->ReleaseBuffer(device.target_frames, AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(hr)) return;
    hr = device.client->Start();
  }();
  if (FAILED(hr)) {
    REXAPU_WARN("audio_wasapi_exclusive: '{}' refused {} (0x{:08X}): the shared output", device_name_, step,
                static_cast<uint32_t>(hr));
    device.client.Reset();
    if (com) {
      CoUninitialize();
    }
    opened.set_value(false);
    return;
  }
  REXAPU_INFO("audio_wasapi_exclusive: '{}' in exclusive mode, 48 kHz stereo {}, {:.1f} ms kept in a {:.1f} ms buffer",
              device_name_, device.sample.name, device.target_frames * 1000.0 / kRate,
              device.buffer_frames * 1000.0 / kRate);
  opened.set_value(true);

  bool lost = false;
  auto next_paced = std::chrono::steady_clock::now();
  while (!stop_) {
    if (lost) {
      // The device is gone: the client's frames are taken at the console's pace, so the game goes on, silently.
      next_paced += std::chrono::microseconds(5333);
      std::this_thread::sleep_until(next_paced);
      if (NextFrame()) {
        staging_read_ = kChannelSamples;
      }
      continue;
    }
    // Every half millisecond, the buffer topped up to the amount kept.
    rex::thread::PreciseSleep(std::chrono::microseconds(500));
    UINT32 padding = 0;
    const bool ok = SUCCEEDED(device.client->GetCurrentPadding(&padding));
    if (ok && padding >= device.target_frames) {
      continue;
    }
    if (ok && padding == 0) {
      // Empty before it was topped up: the device played a gap (logged by SubmitFrame: never on this thread).
      underruns_.fetch_add(1, std::memory_order_relaxed);
    }
    if (!ok || !Render(device, device.target_frames - padding)) {
      REXAPU_ERROR("audio_wasapi_exclusive: '{}' stopped taking sound (the device was removed or taken): silence",
                   device_name_);
      lost = true;
      next_paced = std::chrono::steady_clock::now();
    }
  }
  device.render.Reset();
  device.client.Reset();
  if (com) {
    CoUninitialize();
  }
}

bool WasapiExclusiveDriver::NextFrame() {
  float* frame = nullptr;
  {
    std::scoped_lock lock(frames_mutex_);
    if (frames_queued_.empty()) {
      return false;
    }
    frame = frames_queued_.front();
    frames_queued_.pop();
  }
  if (REXCVAR_GET(audio_mute)) {
    std::fill(std::begin(staging_), std::end(staging_), 0.0f);
  } else {
    conversion::sequential_6_BE_to_interleaved_2_LE(staging_, frame, kChannelSamples, GetStereoFold(),
                                                    GetOutputGain());
  }
  staging_read_ = 0;
  {
    std::scoped_lock lock(frames_mutex_);
    frames_unused_.push(frame);
  }
  semaphore_->Release(1, nullptr);
  return true;
}

bool WasapiExclusiveDriver::Render(Device& device, uint32_t frames) {
  BYTE* data = nullptr;
  if (FAILED(device.render->GetBuffer(frames, &data))) {
    return false;
  }
  uint8_t* out = data;
  uint32_t silence = 0;
  for (uint32_t i = 0; i < frames; ++i) {
    if (staging_read_ == kChannelSamples && !NextFrame()) {
      // Nothing queued: the rest of the period silent.
      silence = frames - i;
      std::memset(out, 0, size_t(silence) * device.sample.container / 8 * 2);
      break;
    }
    WriteSample(out, staging_[staging_read_ * 2], device.sample);
    WriteSample(out, staging_[staging_read_ * 2 + 1], device.sample);
    ++staging_read_;
  }
  if (silence) {
    NoteSilence(silence);
  }
  return SUCCEEDED(device.render->ReleaseBuffer(frames, 0));
}

// audio_keep_time: silence while the client submits is owed, one client frame per 256 samples.
void WasapiExclusiveDriver::NoteSilence(uint32_t samples) {
  if (!REXCVAR_GET(audio_keep_time)) {
    return;
  }
  std::scoped_lock lock(frames_mutex_);
  if (last_submit_ == std::chrono::steady_clock::time_point{} ||
      std::chrono::steady_clock::now() - last_submit_ >= kClientActive) {
    silence_samples_ = 0;
    return;
  }
  silence_samples_ += samples;
  while (silence_samples_ >= kChannelSamples) {
    silence_samples_ -= kChannelSamples;
    if (owed_frames_.load(std::memory_order_relaxed) < kMaxOwedFrames) {
      owed_frames_.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

uint32_t WasapiExclusiveDriver::QueuedFrames(uint32_t asked) const {
  const int32_t queue = REXCVAR_GET(audio_wasapi_exclusive_queue);
  return queue >= 2 ? std::min(asked, uint32_t(queue)) : asked;
}

void WasapiExclusiveDriver::SubmitFrame(uint32_t frame_ptr) {
  // The render thread's gaps, said once in a while from here: a log line writes its file in the caller's thread.
  if (const uint32_t underruns = underruns_.load(std::memory_order_relaxed); underruns != underruns_logged_) {
    const auto now = std::chrono::steady_clock::now();
    if (now - underruns_logged_at_ >= std::chrono::seconds(10)) {
      REXAPU_WARN("audio_wasapi_exclusive: the device's buffer ran empty ({} times so far): a larger "
                  "audio_wasapi_exclusive_buffer_ms may be needed",
                  underruns);
      underruns_logged_ = underruns;
      underruns_logged_at_ = now;
    }
  }
  float* frame = nullptr;
  {
    std::unique_lock lock(frames_mutex_);
    if (REXCVAR_GET(audio_keep_time)) {
      last_submit_ = std::chrono::steady_clock::now();
      if (owed_frames_.load(std::memory_order_relaxed)) {
        // As the SDL driver: dropped for the silence already played, and the next one asked for at once.
        const uint32_t owed = owed_frames_.fetch_sub(1, std::memory_order_relaxed) - 1;
        ++dropped_frames_;
        const uint32_t dropped = owed ? 0 : dropped_frames_;
        if (!owed) {
          dropped_frames_ = 0;
        }
        lock.unlock();
        semaphore_->Release(1, nullptr);
        // Logged outside the lock (the render thread waits for it), as the SDL driver does.
        if (dropped) {
          REXAPU_INFO("audio_keep_time: {} frames ({} ms) of the game's sound dropped for the silence played "
                      "while it came late",
                      dropped, dropped * kChannelSamples * 1000 / kRate);
        }
        return;
      }
    }
    if (!frames_unused_.empty()) {
      frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }
  if (!frame) {
    frame = new float[kFrameSamples];
  }
  std::memcpy(frame, memory_->TranslateVirtual<float*>(frame_ptr), kFrameSamples * sizeof(float));
  std::scoped_lock lock(frames_mutex_);
  frames_queued_.push(frame);
}

}  // namespace rex::audio::wasapi
