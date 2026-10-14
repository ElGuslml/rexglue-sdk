/**
 * @file        system/interfaces/graphics.h
 * @brief       Abstract graphics system interface for dependency injection
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <filesystem>

#include <rex/system/xtypes.h>

// Forward declarations
namespace rex::runtime {
class FunctionDispatcher;
}
namespace rex::ui {
class GraphicsProvider;
class Presenter;
class WindowedAppContext;
}  // namespace rex::ui
namespace rex::system {
class KernelState;
}

namespace rex::system {

// A draw packet the command processor executed, for tools that compare the guest's GPU state with
// their own view of it (for example NativeXRenderer's observer). Plain data, so it crosses the GPU
// plugin boundary; the pointers are valid only during the callback.
struct GpuDrawInfo {
  uint32_t packet_address;  // physical address of the draw packet's type-3 header
  uint32_t opcode;          // PM4_DRAW_INDX or PM4_DRAW_INDX_2
  const uint32_t* registers;  // the register file after the packet (VGT_DRAW_INITIATOR included)
  uint32_t register_count;
  uint64_t bin_mask;    // predicated tiling: the packet ran because bin_mask & bin_select != 0
  uint64_t bin_select;
  bool issued;          // false if the draw was dropped (viz query kill, backend failure)
  // The bound shaders (hash 0 if none). The constant maps are
  // rex::graphics::Shader::ConstantRegisterMap, null until the backend analyzed the shader.
  uint64_t vertex_shader_hash;
  uint64_t pixel_shader_hash;
  const void* vertex_shader_constants;
  const void* pixel_shader_constants;
  uint8_t vertex_shader_memexport;  // eM# written by memexport (0 = none)
  uint8_t pixel_shader_memexport;
};
using GpuDrawObserver = void (*)(void* user, const GpuDrawInfo& info);

// Answers a guest occlusion query that ended (see IGraphicsSystem::SetOcclusionQueryResolver):
// sample_count_address is the physical address of the query's sample counts. Returns true with the
// number of samples that passed, at the guest's resolution, or false for the fake sample count.
using OcclusionQueryResolver = bool (*)(void* user, uint32_t sample_count_address, uint64_t* samples);

// The guest's display gamma ramp, as the GPU's display controller holds it (the DC_LUT registers) and the
// command processor tracks it (see IGraphicsSystem::GetGammaRamp).
struct GammaRamp {
  // The 256-entry table (DC_LUT_30_COLOR): 10 bits per channel, blue in bits 0:9, green in 10:19, red in
  // 20:29. Used for 8-bit front buffers.
  uint32_t table[256];
  // The piecewise linear ramp (DC_LUT_PWL_DATA), 128 entries of red, green and blue: base in bits 0:15,
  // delta in bits 16:31. Used for 10-bit front buffers.
  uint32_t pwl[128][3];
  // Changes whenever the guest writes either of them.
  uint64_t version;
};

class IGraphicsSystem {
 public:
  virtual ~IGraphicsSystem() = default;

  // Build the provider + presenter. Safe to call standalone (without a
  // Runtime) to stand up a window + ImGui for an installer. Idempotent.
  // Must be called before SetupGuestGpu if presentation is desired: some
  // backends (e.g. Vulkan) bake swapchain support into the provider, and
  // a headless provider from SetupGuestGpu cannot be upgraded in place.
  virtual X_STATUS SetupPresentation(ui::WindowedAppContext* app_context) = 0;

  // Wire the GPU into the guest address space: MMIO, command processor,
  // vsync worker. Needs the Runtime's dispatcher + kernel state. If
  // SetupPresentation has not been called, a headless provider is built.
  virtual X_STATUS SetupGuestGpu(runtime::FunctionDispatcher* function_dispatcher,
                                 KernelState* kernel_state) = 0;

  virtual bool has_presentation() const = 0;

  // --- Optional capabilities, default no-op -------------------------------

  // Host presentation objects for ReXApp's overlay wiring; custom systems may
  // leave these null.
  virtual ui::GraphicsProvider* provider() const { return nullptr; }
  virtual ui::Presenter* presenter() const { return nullptr; }

  // Guest GPU services reached from the xboxkrnl Vd* exports.
  virtual void SetInterruptCallback(uint32_t callback, uint32_t user_data) {
    (void)callback;
    (void)user_data;
  }
  virtual void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
    (void)ptr;
    (void)size_log2;
  }
  virtual void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
    (void)ptr;
    (void)block_size_log2;
  }

  // Called on the GPU thread for every draw packet the command processor executes; nullptr turns it
  // off. Off by default, and free when off. Returns false if the system has no command processor.
  virtual bool SetDrawObserver(GpuDrawObserver observer, void* user) {
    (void)observer;
    (void)user;
    return false;
  }

  // Host rendering on or off. Off, the command processor still executes every packet (registers,
  // fences, interrupts, swaps, event writes) but draws nothing on the host: draws, clears and resolves
  // are skipped, and occlusion queries report the fake sample count, or what the occlusion query
  // resolver answers. For a renderer that draws the guest's frames itself from another source
  // (NativeXRenderer). On by default; may be changed at any time. Returns false if the system has no
  // command processor.
  virtual bool SetHostRendering(bool enabled) {
    (void)enabled;
    return false;
  }

  // Called on the GPU thread when a guest occlusion query ends and the command processor has no host
  // query for it (host rendering off, or host queries disabled), to answer it in place of the fake
  // sample count. For a renderer that draws the guest's frames itself and measures the queries.
  // nullptr turns it off. Returns false if the system has no command processor.
  virtual bool SetOcclusionQueryResolver(OcclusionQueryResolver resolver, void* user) {
    (void)resolver;
    (void)user;
    return false;
  }

  // The guest's current display gamma ramp, for a renderer that presents the guest's frames itself. The
  // host presenters apply it to the front buffer; such a renderer has to as well. Safe from any thread.
  // Returns false if the system has no command processor.
  virtual bool GetGammaRamp(GammaRamp& ramp) {
    (void)ramp;
    return false;
  }

  // Persistent shader/pipeline storage under the cache root. Default: none.
  virtual void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                                       bool blocking) {
    (void)cache_root;
    (void)title_id;
    (void)blocking;
  }

  // One-shot convenience for callers that don't care about the split.
  X_STATUS Setup(runtime::FunctionDispatcher* function_dispatcher, KernelState* kernel_state,
                 ui::WindowedAppContext* app_context, bool with_presentation) {
    if (with_presentation && !has_presentation()) {
      X_STATUS status = SetupPresentation(app_context);
      if (XFAILED(status)) {
        return status;
      }
    }
    return SetupGuestGpu(function_dispatcher, kernel_state);
  }

  virtual void Shutdown() = 0;
};

}  // namespace rex::system
