/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/graphics/graphics_system.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#if REX_PLATFORM_WIN32
#include <dxgi.h>
#include <wrl/client.h>
#endif

REXCVAR_DEFINE_STRING(swap_post_effect, "none", "GPU", "Swap post effect: none, fxaa, fxaa_extreme")
    .allowed({"none", "fxaa", "fxaa_extreme"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(vblank_follows_display, false, "GPU",
                    "With vsync, the emulated vertical blank follows the display's (Windows: the monitor the "
                    "window is on) while video_mode_refresh_rate is within 2 % of its refresh rate or of a whole "
                    "fraction of it: the game's frames in step with the display's refreshes, where a timer's rate "
                    "drifts from them (a frame dropped or shown twice every few seconds). The timer otherwise, and "
                    "while the display gives no vertical blank");

REXCVAR_DEFINE_BOOL(store_shaders, true, "GPU",
                    "Store shaders persistently and load them when loading games to avoid "
                    "runtime spikes and freezes when playing the game not for the first time.");

namespace {

rex::graphics::CommandProcessor::SwapPostEffect ParseSwapPostEffect(
    const std::string& effect_name) {
  std::string lowered = effect_name;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    c = static_cast<unsigned char>(std::tolower(c));
    return c == '-' ? '_' : char(c);
  });
  if (lowered == "fxaa") {
    return rex::graphics::CommandProcessor::SwapPostEffect::kFxaa;
  }
  if (lowered == "fxaa_extreme" || lowered == "extreme") {
    return rex::graphics::CommandProcessor::SwapPostEffect::kFxaaExtreme;
  }
  return rex::graphics::CommandProcessor::SwapPostEffect::kNone;
}

#if REX_PLATFORM_WIN32
// vblank_follows_display: the vertical blanks of the monitor the game's window is on. A thread waits for them
// (IDXGIOutput::WaitForVBlank, as the presenter's UI ticks do) and counts them; the vblank worker waits for that
// count with a timeout, so a display that stops signalling (asleep, unplugged) leaves the timer in charge.
class DisplayVblank {
 public:
  DisplayVblank() : thread_([this] { Run(); }) {}
  ~DisplayVblank() {
    {
      std::scoped_lock lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }

  // About once a second: the window's monitor, its refresh rate, and how many of its vertical blanks make one of
  // the guest's (0: they do not fit, within 2 %, and the timer keeps the guest's rate).
  void Update(double guest_hz) {
    HWND hwnd = nullptr;
    if (rex::Runtime* runtime = rex::Runtime::instance()) {
      if (rex::ui::Window* window = runtime->display_window()) {
        hwnd = static_cast<HWND>(window->GetNativeWindowHandle());
      }
    }
    HMONITOR monitor = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) : nullptr;
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    uint32_t hz = 0;
    if (monitor && GetMonitorInfoW(monitor, &info) &&
        EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1) {
      hz = mode.dmDisplayFrequency;
    }
    uint32_t divisor = hz ? uint32_t(std::lround(hz / guest_hz)) : 0;
    if (divisor && std::abs(double(hz) / divisor - guest_hz) > 0.02 * guest_hz) {
      divisor = 0;
    }
    std::scoped_lock lock(mutex_);
    if (monitor != monitor_ || hz != hz_) {
      variable_ = false;
      intervals_filled_ = 0;
    }
    // Followed, a variable refresh rate display (G-Sync, FreeSync) refreshes when the game's frame arrives, and the
    // game waits for that refresh to start the next: each frame would take its render time plus a refresh. Its
    // vertical blanks then come well apart from the period of its rate; the timer is kept for this display.
    if (output_ && divisor_ && !variable_ && intervals_filled_ == intervals_.size()) {
      std::array<int64_t, 64> sorted = intervals_;
      std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
      const int64_t median = sorted[sorted.size() / 2];
      if (median > period_.count() * 115 / 100) {
        variable_ = true;
        REXLOG_INFO("[vblank] the display's vertical blanks come every {:.2f} ms, not {:.2f}: a variable refresh rate",
                    median / 1e6, period_.count() / 1e6);
      }
    }
    if (variable_) {
      divisor = 0;
    }
    if (monitor != monitor_ || (divisor && !output_)) {
      output_.Reset();
      intervals_filled_ = 0;
      if (monitor && divisor) {
        if (!factory_ || !factory_->IsCurrent()) {
          factory_.Reset();
          CreateDXGIFactory1(IID_PPV_ARGS(&factory_));
        }
        output_ = factory_ ? OutputForMonitor(factory_.Get(), monitor) : nullptr;
      }
    }
    if (!divisor) {
      output_.Reset();
    }
    const uint32_t following = output_ ? divisor : 0;
    if (monitor != monitor_ || hz != hz_ || following != divisor_) {
      if (following) {
        std::string device;
        for (const wchar_t* c = info.szDevice; *c; ++c) {
          device += char(*c);  // "\\.\DISPLAY1"
        }
        REXLOG_INFO("[vblank] following the display's vertical blank: {} at {} Hz, one in {} for {:.0f} Hz", device,
                    hz, following, guest_hz);
      } else {
        REXLOG_INFO("[vblank] the timer at {:.0f} Hz: the display {} Hz {}", guest_hz, hz,
                    variable_  ? "follows the game's frames"
                    : divisor ? "gives no vertical blank to wait for"
                              : "is not a whole multiple of it");
      }
    }
    monitor_ = monitor;
    hz_ = hz;
    divisor_ = following;
    period_ = hz ? std::chrono::nanoseconds(int64_t(1e9 / hz)) : std::chrono::nanoseconds(0);
    cv_.notify_all();
  }

  uint32_t divisor() const { return divisor_; }
  std::chrono::nanoseconds period() const { return period_; }

  // Waits up to `timeout` for the display's vertical blanks since the previous call: how many (0 on a timeout).
  uint64_t Wait(std::chrono::nanoseconds timeout) {
    std::unique_lock lock(mutex_);
    cv_.wait_for(lock, timeout, [this] { return count_ != seen_ || stop_; });
    const uint64_t blanks = count_ - seen_;
    seen_ = count_;
    return blanks;
  }

 private:
  static Microsoft::WRL::ComPtr<IDXGIOutput> OutputForMonitor(IDXGIFactory1* factory, HMONITOR monitor) {
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    for (UINT a = 0; SUCCEEDED(factory->EnumAdapters(a, adapter.ReleaseAndGetAddressOf())); ++a) {
      Microsoft::WRL::ComPtr<IDXGIOutput> output;
      for (UINT o = 0; SUCCEEDED(adapter->EnumOutputs(o, output.ReleaseAndGetAddressOf())); ++o) {
        DXGI_OUTPUT_DESC desc;
        if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
          return output;
        }
      }
    }
    return nullptr;
  }

  void Run() {
    std::unique_lock lock(mutex_);
    while (!stop_) {
      Microsoft::WRL::ComPtr<IDXGIOutput> output = output_;
      if (!output) {
        cv_.wait_for(lock, std::chrono::milliseconds(100));
        continue;
      }
      lock.unlock();
      const bool waited = SUCCEEDED(output->WaitForVBlank());
      const auto now = std::chrono::steady_clock::now();
      lock.lock();
      if (!waited) {
        if (output_ == output) {
          output_.Reset();  // found again by the next Update
        }
        continue;
      }
      // A return well before a refresh period is not a vertical blank (a display that is off may not wait at all).
      if (now - last_ >= period_ / 2) {
        intervals_[intervals_next_] = std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_).count();
        intervals_next_ = (intervals_next_ + 1) % intervals_.size();
        intervals_filled_ = std::min(intervals_filled_ + 1, intervals_.size());
        last_ = now;
        ++count_;
        cv_.notify_all();
      } else {
        cv_.wait_for(lock, std::chrono::milliseconds(1));
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory_;
  Microsoft::WRL::ComPtr<IDXGIOutput> output_;
  HMONITOR monitor_ = nullptr;
  uint32_t hz_ = 0;
  uint32_t divisor_ = 0;
  std::chrono::nanoseconds period_{0};
  uint64_t count_ = 0;
  uint64_t seen_ = 0;
  std::chrono::steady_clock::time_point last_;
  // The intervals between the last vertical blanks counted (nanoseconds), and whether they showed a variable
  // refresh rate display.
  std::array<int64_t, 64> intervals_{};
  size_t intervals_next_ = 0;
  size_t intervals_filled_ = 0;
  bool variable_ = false;
  std::thread thread_;
};
#endif  // REX_PLATFORM_WIN32
}  // namespace

namespace rex::graphics {

// Nvidia Optimus/AMD PowerXpress support.
// These exports force the process to trigger the discrete GPU in multi-GPU
// systems.
// https://developer.download.nvidia.com/devzone/devcenter/gamegraphics/files/OptimusRenderingPolicies.pdf
// https://stackoverflow.com/questions/17458803/amd-equivalent-to-nvoptimusenablement
#if REX_PLATFORM_WIN32
extern "C" {
__declspec(dllexport) uint32_t NvOptimusEnablement = 0x00000001;
__declspec(dllexport) uint32_t AmdPowerXpressRequestHighPerformance = 1;
}  // extern "C"
#endif  // REX_PLATFORM_WIN32

GraphicsSystem::GraphicsSystem() : vsync_worker_running_(false) {}

GraphicsSystem::~GraphicsSystem() = default;

X_STATUS GraphicsSystem::SetupPresentation(ui::WindowedAppContext* app_context) {
  if (presenter_) {
    return X_STATUS_SUCCESS;
  }

  if (!provider_) {
    CreateProvider(true);
    if (!provider_) {
      REXGPU_ERROR("Unable to create graphics provider");
      return X_STATUS_UNSUCCESSFUL;
    }
    provider_supports_presentation_ = true;
  } else if (!provider_supports_presentation_) {
    // A prior SetupGuestGpu built a headless provider; backends like Vulkan
    // need swapchain support baked in at provider creation time.
    REXGPU_ERROR("SetupPresentation called after headless SetupGuestGpu; call order is reversed");
    return X_STATUS_UNSUCCESSFUL;
  }

  app_context_ = app_context;
  auto loss_cb = [this](bool is_responsible, bool statically_from_ui_thread) {
    OnHostGpuLossFromAnyThread(is_responsible);
  };
  if (app_context_) {
    // Presenter creation must happen on the UI thread.
    app_context_->CallInUIThreadSynchronous(
        [this, loss_cb]() { presenter_ = provider_->CreatePresenter(loss_cb); });
  } else {
    // Offscreen path (e.g. capturing guest output without a window).
    presenter_ = provider_->CreatePresenter(loss_cb);
  }

  if (!presenter_) {
    REXGPU_ERROR("Unable to create presenter");
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS GraphicsSystem::SetupGuestGpu(runtime::FunctionDispatcher* function_dispatcher,
                                       system::KernelState* kernel_state) {
  memory_ = function_dispatcher->memory();
  function_dispatcher_ = function_dispatcher;
  kernel_state_ = kernel_state;

  // Headless path: no one set up presentation, so build a no-presentation
  // provider just for the command processor.
  if (!provider_) {
    CreateProvider(false);
    provider_supports_presentation_ = false;
  }

  // Create command processor. This will spin up a thread to process all
  // incoming ringbuffer packets.
  command_processor_ = CreateCommandProcessor();
  if (!command_processor_->Initialize()) {
    REXGPU_ERROR("Unable to initialize command processor");
    return X_STATUS_UNSUCCESSFUL;
  }
  command_processor_->SetDesiredSwapPostEffect(ParseSwapPostEffect(REXCVAR_GET(swap_post_effect)));

  // Register GPU MMIO handlers
  // GPU registers are at 0x7FC80000-0x7FCFFFFF
  memory_->AddVirtualMappedRange(0x7FC80000,  // base address
                                 0xFFFF0000,  // mask
                                 0x0000FFFF,  // size (64KB)
                                 this,        // context (GraphicsSystem*)
                                 reinterpret_cast<runtime::MMIOReadCallback>(ReadRegisterThunk),
                                 reinterpret_cast<runtime::MMIOWriteCallback>(WriteRegisterThunk));

  // Guest vblank timer based on the configured guest video mode.
  vsync_worker_running_ = true;
  vsync_worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
        system::X_VIDEO_MODE video_mode;
        kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
        double refresh_rate_hz = std::max(1.0, double(float(video_mode.refresh_rate)));
        uint64_t guest_tick_frequency = chrono::Clock::guest_tick_frequency();
        uint64_t vsync_interval_ticks =
            std::max(uint64_t(1), uint64_t(double(guest_tick_frequency) / refresh_rate_hz));
        uint64_t no_vsync_interval_ticks = std::max(uint64_t(1), guest_tick_frequency / 1000);
        uint64_t last_frame_time = chrono::Clock::QueryGuestTickCount();
#if REX_PLATFORM_WIN32
        // vblank_follows_display: the display's vertical blanks while they fit the guest's rate; the timer below
        // otherwise, and for a second after the display gave none within three of its periods.
        std::optional<DisplayVblank> display;
        auto display_checked = std::chrono::steady_clock::time_point{};
        auto display_timer_until = std::chrono::steady_clock::time_point{};
        bool display_drain = true;
        uint64_t display_blanks = 0;
#endif
        while (vsync_worker_running_) {
#if REX_PLATFORM_WIN32
          if (REXCVAR_GET(vsync) && REXCVAR_GET(vblank_follows_display)) {
            if (!display) {
              display.emplace();
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - display_checked >= std::chrono::seconds(1)) {
              display_checked = now;
              display->Update(refresh_rate_hz);
            }
            if (display->divisor() && now >= display_timer_until) {
              if (display_drain) {
                display->Wait(std::chrono::nanoseconds(0));  // the blanks counted while the timer ran
                display_drain = false;
              }
              if (uint64_t blanks = display->Wait(3 * display->period())) {
                for (display_blanks += blanks; display_blanks >= display->divisor();
                     display_blanks -= display->divisor()) {
                  MarkVblank();
                }
                last_frame_time = chrono::Clock::QueryGuestTickCount();
                continue;
              }
              REXLOG_WARN("[vblank] no vertical blank from the display in {} ms: the timer for a second",
                          3 * display->period().count() / 1000000);
              display_timer_until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            }
            display_drain = true;
          } else if (display) {
            display.reset();
          }
#endif
          uint64_t current_time = chrono::Clock::QueryGuestTickCount();
          uint64_t interval_ticks =
              REXCVAR_GET(vsync) ? vsync_interval_ticks : no_vsync_interval_ticks;
          while (current_time - last_frame_time >= interval_ticks) {
            MarkVblank();
            last_frame_time += interval_ticks;
          }
          rex::thread::Sleep(std::chrono::milliseconds(1));
        }
        return 0;
      }));
  // TODO: set_can_debugger_suspend not yet ported
  // vsync_worker_thread_->set_can_debugger_suspend(true);
  vsync_worker_thread_->set_name("GPU VSync");
  vsync_worker_thread_->Create();

  return X_STATUS_SUCCESS;
}

void GraphicsSystem::Shutdown() {
  if (command_processor_) {
    command_processor_->Shutdown();
    command_processor_.reset();
  }

  if (vsync_worker_thread_) {
    vsync_worker_running_ = false;
    vsync_worker_thread_->Wait(0, 0, 0, nullptr);
    vsync_worker_thread_.reset();
  }

  if (presenter_) {
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
    }
    // If there's no app context (thus the presenter is owned by the thread that
    // initialized the GraphicsSystem) or can't be queueing UI thread calls
    // anymore, shutdown anyway.
    presenter_.reset();
  }

  provider_.reset();
}

void GraphicsSystem::OnHostGpuLossFromAnyThread([[maybe_unused]] bool is_responsible) {
  // TODO(Triang3l): Somehow gain exclusive ownership of the Provider (may be
  // used by the command processor, the presenter, and possibly anything else,
  // it's considered free-threaded, except for lifetime management which will be
  // involved in this case) and reset it so a new host GPU API device is
  // created. Then ask the command processor to reset itself in its thread, and
  // ask the UI thread to reset the Presenter (the UI thread manages its
  // lifetime - but if there's no WindowedAppContext, either don't reset it as
  // in this case there's no user who needs uninterrupted gameplay, or somehow
  // protect it with a mutex so any thread can be considered a UI thread and
  // reset).
  if (host_gpu_loss_reported_.test_and_set(std::memory_order_relaxed)) {
    return;
  }
  rex::FatalError("Graphics device lost (probably due to an internal error)");
}

uint32_t GraphicsSystem::ReadRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr) {
  return gs->ReadRegister(addr);
}

void GraphicsSystem::WriteRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr,
                                        uint32_t value) {
  gs->WriteRegister(addr, value);
}

uint32_t GraphicsSystem::ReadRegister(uint32_t addr) {
  uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case 0x0F00:  // RB_EDRAM_TIMING
      return 0x08100748;
    case 0x0F01:  // RB_BC_CONTROL
      return 0x0000200E;
    case 0x194C: {  // R500_D1MODE_V_COUNTER
      system::X_VIDEO_MODE video_mode;
      kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      return std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
    }
    case 0x1951:    // interrupt status
      return 1;     // vblank
    case 0x1961: {  // AVIVO_D1MODE_VIEWPORT_SIZE
      // Maximum [width(0x0FFF), height(0x0FFF)].
      system::X_VIDEO_MODE video_mode;
      kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      uint32_t viewport_width = std::min(uint32_t(video_mode.display_width), uint32_t(0x0FFF));
      uint32_t viewport_height = std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
      return (viewport_width << 16) | viewport_height;
    }
    default:
      if (!register_file_.GetRegisterInfo(r)) {
        REXGPU_DEBUG("GPU: Read from unknown register ({:04X})", r);
      }
  }

  assert_true(r < RegisterFile::kRegisterCount);
  return register_file_.values[r];
}

void GraphicsSystem::WriteRegister(uint32_t addr, uint32_t value) {
  uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case 0x01C5:  // CP_RB_WPTR
      command_processor_->UpdateWritePointer(value);
      break;
    case 0x1844:  // AVIVO_D1GRPH_PRIMARY_SURFACE_ADDRESS
      break;
    default:
      REXGPU_WARN("Unknown GPU register {:04X} write: {:08X}", r, value);
      break;
  }

  assert_true(r < RegisterFile::kRegisterCount);
  register_file_.values[r] = value;
}

void GraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  command_processor_->InitializeRingBuffer(ptr, size_log2);
}

void GraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  command_processor_->EnableReadPointerWriteBack(ptr, block_size_log2);
}

void GraphicsSystem::SetInterruptCallback(uint32_t callback, uint32_t user_data) {
  interrupt_callback_ = callback;
  interrupt_callback_data_ = user_data;
  REXGPU_INFO("SetInterruptCallback({:08X}, {:08X})", callback, user_data);
}

bool GraphicsSystem::SetDrawObserver(system::GpuDrawObserver observer, void* user) {
  if (!command_processor_) {
    return false;
  }
  command_processor_->SetDrawObserver(observer, user);
  REXGPU_INFO("Draw observer {}", observer ? "set" : "cleared");
  return true;
}

bool GraphicsSystem::SetHostRendering(bool enabled) {
  if (!command_processor_) {
    return false;
  }
  command_processor_->SetHostRendering(enabled);
  REXGPU_INFO("Host rendering {}", enabled ? "on" : "off (sync only)");
  return true;
}

bool GraphicsSystem::SetOcclusionQueryResolver(system::OcclusionQueryResolver resolver, void* user) {
  if (!command_processor_) {
    return false;
  }
  command_processor_->SetOcclusionQueryResolver(resolver, user);
  REXGPU_INFO("Occlusion query resolver {}", resolver ? "set" : "removed");
  return true;
}

bool GraphicsSystem::SetSignalGate(system::GpuSignalGate gate, void* user) {
  if (!command_processor_) {
    return false;
  }
  command_processor_->SetSignalGate(gate, user);
  REXGPU_INFO("Signal gate {}", gate ? "set" : "removed");
  return true;
}

bool GraphicsSystem::GetSwapCounts(uint64_t& issued, uint64_t& executed) {
  issued = swaps_issued_.load(std::memory_order_acquire);
  executed = command_processor_ ? command_processor_->swaps_executed() : 0;
  return command_processor_ != nullptr;
}

bool GraphicsSystem::SetSwapObserver(system::GpuSwapObserver observer, void* user) {
  if (!command_processor_) {
    return false;
  }
  command_processor_->SetSwapObserver(observer, user);
  REXGPU_INFO("Swap observer {}", observer ? "set" : "removed");
  return true;
}

bool GraphicsSystem::GetGammaRamp(system::GammaRamp& ramp) {
  if (!command_processor_) {
    return false;
  }
  command_processor_->GetGammaRamp(ramp);
  return true;
}

void GraphicsSystem::DispatchInterruptCallback(uint32_t source, uint32_t cpu) {
  if (!interrupt_callback_) {
    return;
  }

  auto thread = system::XThread::GetCurrentThread();
  assert_not_null(thread);

  // Pick a CPU, if needed. We're going to guess 2. Because.
  if (cpu == 0xFFFFFFFF) {
    cpu = 2;
  }
  thread->SetActiveCpu(cpu);

  // REXGPU_INFO("Dispatching GPU interrupt at {:08X} w/ mode {} on cpu {}",
  //          interrupt_callback_, source, cpu);

  uint64_t args[] = {source, interrupt_callback_data_};
  function_dispatcher_->ExecuteInterrupt(thread->thread_state(), interrupt_callback_, args,
                                         rex::countof(args));
}

void GraphicsSystem::MarkVblank() {
  // TODO: Enable profiling once ported
  // SCOPE_profile_cpu_f("gpu");

  // Increment vblank counter (so the game sees us making progress).
  if (command_processor_) {
    command_processor_->increment_counter();
  }

  // TODO(benvanik): we shouldn't need to do the dispatch here, but there's
  //     something wrong and the CP will block waiting for code that
  //     needs to be run in the interrupt.
  DispatchInterruptCallback(0, 2);
}

void GraphicsSystem::ClearCaches() {
  command_processor_->CallInThread([&]() { command_processor_->ClearCaches(); });
}

void GraphicsSystem::InvalidateGpuMemory() {
  command_processor_->CallInThread([&]() { command_processor_->InvalidateGpuMemory(); });
}

void GraphicsSystem::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                             uint32_t title_id, bool blocking) {
  if (!REXCVAR_GET(store_shaders)) {
    return;
  }
  if (blocking) {
    if (command_processor_->is_paused()) {
      // Safe to run on any thread while the command processor is paused, no
      // race condition.
      command_processor_->InitializeShaderStorage(cache_root, title_id, true);
    } else {
      rex::thread::Fence fence;
      command_processor_->CallInThread([this, cache_root, title_id, &fence]() {
        command_processor_->InitializeShaderStorage(cache_root, title_id, true);
        fence.Signal();
      });
      fence.Wait();
    }
  } else {
    command_processor_->CallInThread([this, cache_root, title_id]() {
      command_processor_->InitializeShaderStorage(cache_root, title_id, false);
    });
  }
}

void GraphicsSystem::Pause() {
  paused_ = true;
  command_processor_->Pause();
}

void GraphicsSystem::Resume() {
  paused_ = false;
  command_processor_->Resume();
}

bool GraphicsSystem::Save(::rex::stream::ByteStream* stream) {
  stream->Write<uint32_t>(interrupt_callback_);
  stream->Write<uint32_t>(interrupt_callback_data_);
  return command_processor_->Save(stream);
}

bool GraphicsSystem::Restore(::rex::stream::ByteStream* stream) {
  interrupt_callback_ = stream->Read<uint32_t>();
  interrupt_callback_data_ = stream->Read<uint32_t>();
  return command_processor_->Restore(stream);
}

}  // namespace rex::graphics
