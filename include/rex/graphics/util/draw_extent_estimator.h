#pragma once
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

#include <cstdint>
#include <cstring>
#include <optional>

#include <rex/graphics/pipeline/shader/interpreter.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/memory.h>

namespace rex::graphics {

class DrawExtentEstimator {
 public:
  DrawExtentEstimator(const RegisterFile& register_file, const memory::Memory& memory)
      : register_file_(register_file),
        memory_(memory),
        shader_interpreter_(register_file, memory) {}

  // The shader must have its ucode analyzed.
  uint32_t EstimateVertexMaxY(const Shader& vertex_shader);
  uint32_t EstimateMaxY(bool try_to_estimate_vertex_max_y, const Shader& vertex_shader);

  struct CoverageRectangle {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
  };
  // If the draw is a single convex primitive (a triangle or a rectangle with 3 indices, or a
  // convex quad as a strip, fan or quad list with 4), a rectangle of pixels within
  // [0, width) x [0, height) of the render target whose every sample is certainly covered,
  // considering the scissor, the viewport, clipping and vertex killing - only the geometry, not the
  // pixel shader or the output state: the primitive itself if it is an axis-aligned rectangle,
  // otherwise the widest band from the top of the scissor. constant_z_out receives the screen-space
  // depth if all vertices have the same one. Conservative: false whenever coverage cannot be proven.
  // The vertex shader must have its ucode analyzed.
  bool GetCoverageRectangle(const Shader& vertex_shader, uint32_t width, uint32_t height,
                            CoverageRectangle& rectangle_out, std::optional<float>& constant_z_out);

  // If the pixel shader's color outputs don't depend on the pixel (no position, no texture
  // fetches, no discarding, and only interpolants that the last GetCoverageRectangle found equal
  // in every vertex), runs it on the CPU with the current constants and those interpolants, and
  // returns the colors it writes: colors_out[i] for color target i, with written_mask_out bit i set
  // when all four components of that target were written. The shader must have its ucode analyzed.
  bool GetConstantPixelShaderOutputs(const Shader& pixel_shader, float colors_out[4][4],
                                     uint32_t& written_mask_out);

 private:
  class PositionYExportSink : public ShaderInterpreter::ExportSink {
   public:
    void Export(ucode::ExportRegister export_register, const float* value,
                uint32_t value_mask) override;

    void Reset() {
      position_y_.reset();
      position_w_.reset();
      point_size_.reset();
      vertex_kill_.reset();
    }

    const std::optional<float>& position_y() const { return position_y_; }
    const std::optional<float>& position_w() const { return position_w_; }
    const std::optional<float>& point_size() const { return point_size_; }
    const std::optional<uint32_t>& vertex_kill() const { return vertex_kill_; }

   private:
    std::optional<float> position_y_;
    std::optional<float> position_w_;
    std::optional<float> point_size_;
    std::optional<uint32_t> vertex_kill_;
  };

  class PositionExportSink : public ShaderInterpreter::ExportSink {
   public:
    void Export(ucode::ExportRegister export_register, const float* value,
                uint32_t value_mask) override;

    void Reset() {
      position_mask_ = 0;
      vertex_kill_.reset();
      std::memset(interpolator_masks_, 0, sizeof(interpolator_masks_));
    }

    // Bits for the x, y, z, w components written.
    uint32_t position_mask() const { return position_mask_; }
    const float* position() const { return position_; }
    const std::optional<uint32_t>& vertex_kill() const { return vertex_kill_; }
    uint32_t interpolator_mask(uint32_t index) const { return interpolator_masks_[index]; }
    const float* interpolator(uint32_t index) const { return interpolators_[index]; }

   private:
    float position_[4] = {};
    uint32_t position_mask_ = 0;
    std::optional<uint32_t> vertex_kill_;
    float interpolators_[16][4] = {};
    uint32_t interpolator_masks_[16] = {};
  };

  class ColorExportSink : public ShaderInterpreter::ExportSink {
   public:
    void Export(ucode::ExportRegister export_register, const float* value,
                uint32_t value_mask) override;

    const float* color(uint32_t index) const { return colors_[index]; }
    // Bit i set when all four components of color target i were written.
    uint32_t written_mask() const { return written_mask_; }

   private:
    float colors_[4][4] = {};
    uint32_t component_masks_[4] = {};
    uint32_t written_mask_ = 0;
  };

  const RegisterFile& register_file_;
  const memory::Memory& memory_;

  ShaderInterpreter shader_interpreter_;

  // Interpolators that the last GetCoverageRectangle found fully written and equal in every
  // vertex, for GetConstantPixelShaderOutputs.
  uint32_t constant_interpolator_mask_ = 0;
  float constant_interpolators_[16][4] = {};
};

}  // namespace rex::graphics
