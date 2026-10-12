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

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/format/ucode.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/util/draw_extent_estimator.h>
#include <rex/graphics/xenos.h>
#include <rex/memory.h>
#include <rex/ui/graphics_util.h>

REXCVAR_DEFINE_BOOL(execute_unclipped_draw_vs_on_cpu, false, "GPU",
                    "Execute unclipped draw vertex shader on CPU");

REXCVAR_DEFINE_BOOL(execute_unclipped_draw_vs_on_cpu_with_scissor, false, "GPU",
                    "Execute unclipped draw VS on CPU with scissor");

// DEFINE_bool(
//     execute_unclipped_draw_vs_on_cpu, true,
//     "Execute the vertex shader for draws with clipping disabled, primarily "
//     "screen-space draws (such as clears), on the CPU when possible to estimate "
//     "the extent of the EDRAM involved in the draw.\n"
//     "Enabling this may significantly improve GPU performance as otherwise up "
//     "to the entire EDRAM may be considered used in draws without clipping, "
//     "potentially resulting in spurious EDRAM range ownership transfer round "
//     "trips between host render targets.\n"
//     "Also, on hosts where certain render target formats have to be emulated in "
//     "a lossy way (for instance, 16-bit fixed-point via 16-bit floating-point), "
//     "this prevents corruption of other render targets located after the "
//     "current ones in the EDRAM by lossy range ownership transfers done for "
//     "those draws.",
//     "GPU");
// DEFINE_bool(
//     execute_unclipped_draw_vs_on_cpu_with_scissor, false,
//     "Don't restrict the usage of execute_unclipped_draw_vs_on_cpu to only "
//     "non-scissored draws (with the right and the bottom sides of the scissor "
//     "rectangle at 8192 or beyond) even though if the scissor rectangle is "
//     "present, it's usually sufficient for esimating the height of the render "
//     "target.\n"
//     "Enabling this may cause excessive processing of vertices on the CPU, as "
//     "some games draw rectangles (for their UI, for instance) without clipping, "
//     "but with a proper scissor rectangle.",
//     "GPU");

namespace rex::graphics {

void DrawExtentEstimator::PositionYExportSink::Export(ucode::ExportRegister export_register,
                                                      const float* value, uint32_t value_mask) {
  if (export_register == ucode::ExportRegister::kVSPosition) {
    if (value_mask & 0b0010) {
      position_y_ = value[1];
    }
    if (value_mask & 0b1000) {
      position_w_ = value[3];
    }
  } else if (export_register == ucode::ExportRegister::kVSPointSizeEdgeFlagKillVertex) {
    if (value_mask & 0b0001) {
      point_size_ = value[0];
    }
    if (value_mask & 0b0100) {
      vertex_kill_ = rex::memory::Reinterpret<uint32_t>(value[2]);
    }
  }
}

uint32_t DrawExtentEstimator::EstimateVertexMaxY(const Shader& vertex_shader) {
  SCOPE_profile_cpu_f("gpu");

  const RegisterFile& regs = register_file_;

  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  if (!vgt_draw_initiator.num_indices) {
    return 0;
  }
  if (vgt_draw_initiator.source_select != xenos::SourceSelect::kDMA &&
      vgt_draw_initiator.source_select != xenos::SourceSelect::kAutoIndex) {
    // TODO(Triang3l): Support immediate indices.
    return xenos::kTexture2DCubeMaxWidthHeight;
  }

  // Not reproducing tessellation.
  if (xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode, vgt_draw_initiator.prim_type) &&
      regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
          xenos::VGTOutputPath::kTessellationEnable) {
    return xenos::kTexture2DCubeMaxWidthHeight;
  }

  assert_true(vertex_shader.type() == xenos::ShaderType::kVertex);
  assert_true(vertex_shader.is_ucode_analyzed());
  if (!ShaderInterpreter::CanInterpretShader(vertex_shader)) {
    return xenos::kTexture2DCubeMaxWidthHeight;
  }

  auto vgt_dma_size = regs.Get<reg::VGT_DMA_SIZE>();
  union {
    const void* index_buffer;
    const uint16_t* index_buffer_16;
    const uint32_t* index_buffer_32;
  };
  xenos::Endian index_endian = vgt_dma_size.swap_mode;
  if (vgt_draw_initiator.source_select == xenos::SourceSelect::kDMA) {
    xenos::IndexFormat index_format = vgt_draw_initiator.index_size;
    uint32_t index_buffer_base = regs[XE_GPU_REG_VGT_DMA_BASE];
    if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16) {
      // Handle the index endianness to same way as the PrimitiveProcessor.
      if (index_endian == xenos::Endian::k8in32) {
        index_endian = xenos::Endian::k8in16;
      } else if (index_endian == xenos::Endian::k16in32) {
        index_endian = xenos::Endian::kNone;
      }
      index_buffer_base &= ~uint32_t(sizeof(uint16_t) - 1);
    } else {
      assert_true(vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32);
      index_buffer_base &= ~uint32_t(sizeof(uint32_t) - 1);
    }
    index_buffer = memory_.TranslatePhysical(index_buffer_base);
  }
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  uint32_t reset_index = regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
  uint32_t index_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  uint32_t min_index = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;
  uint32_t max_index = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;

  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  float viewport_y_scale =
      pa_cl_vte_cntl.vport_y_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.0f;
  float viewport_y_offset =
      pa_cl_vte_cntl.vport_y_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f;

  int32_t point_vertex_min_diameter_float = 0;
  int32_t point_vertex_max_diameter_float = 0;
  float point_constant_radius_y = 0.0f;
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    point_vertex_min_diameter_float =
        rex::memory::Reinterpret<int32_t>(float(pa_su_point_minmax.min_size) * (2.0f / 16.0f));
    point_vertex_max_diameter_float =
        rex::memory::Reinterpret<int32_t>(float(pa_su_point_minmax.max_size) * (2.0f / 16.0f));
    point_constant_radius_y = float(regs.Get<reg::PA_SU_POINT_SIZE>().height) * (1.0f / 16.0f);
  }

  float max_y = -FLT_MAX;

  shader_interpreter_.SetShader(vertex_shader);

  PositionYExportSink position_y_export_sink;
  shader_interpreter_.SetExportSink(&position_y_export_sink);
  for (uint32_t i = 0; i < vgt_draw_initiator.num_indices; ++i) {
    uint32_t vertex_index;
    if (vgt_draw_initiator.source_select == xenos::SourceSelect::kDMA) {
      if (i < vgt_dma_size.num_words) {
        if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16) {
          vertex_index = index_buffer_16[i];
        } else {
          vertex_index = index_buffer_32[i];
        }
        // The Xenos only uses 24 bits of the index (reset_indx is 24-bit).
        vertex_index = xenos::GpuSwap(vertex_index, index_endian) & 0xFFFFFF;
      } else {
        vertex_index = 0;
      }
      if (pa_su_sc_mode_cntl.multi_prim_ib_ena && vertex_index == reset_index) {
        continue;
      }
    } else {
      assert_true(vgt_draw_initiator.source_select == xenos::SourceSelect::kAutoIndex);
      vertex_index = i;
    }
    vertex_index =
        std::min(max_index, std::max(min_index, (vertex_index + index_offset) & 0xFFFFFF));

    position_y_export_sink.Reset();

    shader_interpreter_.temp_registers()[0] = float(vertex_index);
    shader_interpreter_.Execute();

    if (position_y_export_sink.vertex_kill().has_value() &&
        (position_y_export_sink.vertex_kill().value() & ~(UINT32_C(1) << 31))) {
      continue;
    }
    if (!position_y_export_sink.position_y().has_value()) {
      continue;
    }
    float vertex_y = position_y_export_sink.position_y().value();
    if (!pa_cl_vte_cntl.vtx_xy_fmt) {
      if (!position_y_export_sink.position_w().has_value()) {
        continue;
      }
      vertex_y /= position_y_export_sink.position_w().value();
    }

    vertex_y = vertex_y * viewport_y_scale + viewport_y_offset;

    if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
      float point_radius_y;
      if (position_y_export_sink.point_size().has_value()) {
        // Vertex-specified diameter. Clamped effectively as a signed integer in
        // the hardware, -NaN, -Infinity ... -0 to the minimum, +Infinity, +NaN
        // to the maximum.
        point_radius_y = 0.5f * rex::memory::Reinterpret<float>(std::min(
                                    point_vertex_max_diameter_float,
                                    std::max(point_vertex_min_diameter_float,
                                             rex::memory::Reinterpret<int32_t>(
                                                 position_y_export_sink.point_size().value()))));
      } else {
        // Constant radius.
        point_radius_y = point_constant_radius_y;
      }
      vertex_y += point_radius_y;
    }

    // std::max is `a < b ? b : a`, thus in case of NaN, the first argument is
    // always returned - max_y, which is initialized to a normalized value.
    max_y = std::max(max_y, vertex_y);
  }
  shader_interpreter_.SetExportSink(nullptr);

  int32_t max_y_24p8 = ui::FloatToD3D11Fixed16p8(max_y);
  // 16p8 range is -32768 to 32767+255/256, but it's stored as uint32_t here,
  // as 24p8, so overflowing up to -8388608 to 8388608+255/256 is safe. The
  // range of the window offset plus the half-pixel offset is -16384 to 16384.5,
  // so it's safe to add both - adding it will neither move the 16p8 clamping
  // bounds -32768 and 32767+255/256 into the 0...8192 screen space range, nor
  // cause 24p8 overflow.
  if (regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero) {
    max_y_24p8 += 128;
  }
  if (pa_su_sc_mode_cntl.vtx_window_offset_enable) {
    max_y_24p8 += regs.Get<reg::PA_SC_WINDOW_OFFSET>().window_y_offset * 256;
  }
  // Top-left rule - .5 exclusive without MSAA, 1. exclusive with MSAA.
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  return (uint32_t(std::max(int32_t(0), max_y_24p8)) +
          ((rb_surface_info.msaa_samples == xenos::MsaaSamples::k1X) ? 127 : 255)) >>
         8;
}

uint32_t DrawExtentEstimator::EstimateMaxY(bool try_to_estimate_vertex_max_y,
                                           const Shader& vertex_shader) {
  SCOPE_profile_cpu_f("gpu");

  const RegisterFile& regs = register_file_;

  auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
  int32_t window_y_offset = pa_sc_window_offset.window_y_offset;

  // Scissor.
  auto pa_sc_window_scissor_br = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
  int32_t scissor_bottom = int32_t(pa_sc_window_scissor_br.br_y);
  bool scissor_window_offset = !regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>().window_offset_disable;
  if (scissor_window_offset) {
    scissor_bottom += window_y_offset;
  }
  auto pa_sc_screen_scissor_br = regs.Get<reg::PA_SC_SCREEN_SCISSOR_BR>();
  scissor_bottom = std::min(scissor_bottom, int32_t(pa_sc_screen_scissor_br.br_y));
  uint32_t max_y = uint32_t(std::max(scissor_bottom, int32_t(0)));

  if (regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable) {
    // Actual extent from the vertices.
    if (try_to_estimate_vertex_max_y && REXCVAR_GET(execute_unclipped_draw_vs_on_cpu)) {
      bool estimate_vertex_max_y;
      if (REXCVAR_GET(execute_unclipped_draw_vs_on_cpu_with_scissor)) {
        estimate_vertex_max_y = true;
      } else {
        estimate_vertex_max_y = false;
        if (scissor_bottom >= xenos::kTexture2DCubeMaxWidthHeight) {
          // Handle just the usual special 8192x8192 case in Direct3D 9 - 8192
          // may be a normal render target height (80x8192 is well within the
          // EDRAM size, for instance), no need to process the vertices on the
          // CPU in this case.
          int32_t scissor_right = int32_t(pa_sc_window_scissor_br.br_x);
          if (scissor_window_offset) {
            scissor_right += pa_sc_window_offset.window_x_offset;
          }
          scissor_right = std::min(scissor_right, int32_t(pa_sc_screen_scissor_br.br_x));
          if (scissor_right >= xenos::kTexture2DCubeMaxWidthHeight) {
            estimate_vertex_max_y = true;
          }
        }
      }
      if (estimate_vertex_max_y) {
        max_y = std::min(max_y, EstimateVertexMaxY(vertex_shader));
      }
    }
  } else {
    // Viewport. Though the Xenos itself doesn't have an implicit viewport
    // scissor (it's set by Direct3D 9 when a viewport is used), on hosts, it
    // usually exists and can't be disabled.
    auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
    float viewport_bottom = 0.0f;
    // First calculate all the integer.0 or integer.5 offsetting exactly at full
    // precision.
    if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
      viewport_bottom += float(window_y_offset);
    }
    if (regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero) {
      viewport_bottom += 0.5f;
    }
    // Then apply the floating-point viewport offset.
    if (pa_cl_vte_cntl.vport_y_offset_ena) {
      viewport_bottom += regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET);
    }
    viewport_bottom += pa_cl_vte_cntl.vport_y_scale_ena
                           ? std::abs(regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE))
                           : 1.0f;
    // Using floor, or, rather, truncation (because maxing with zero anyway)
    // similar to how viewport scissoring behaves on real AMD, Intel and Nvidia
    // GPUs on Direct3D 12 (but not WARP), also like in
    // draw_util::GetHostViewportInfo.
    // max(0.0f, viewport_bottom) to drop NaN and < 0 - max picks the first
    // argument in the !(a < b) case (always for NaN), min as float (max_y is
    // well below 2^24) to safely drop very large values.
    max_y = uint32_t(std::min(float(max_y), std::max(0.0f, viewport_bottom)));
  }

  return max_y;
}

void DrawExtentEstimator::PositionExportSink::Export(ucode::ExportRegister export_register,
                                                     const float* value, uint32_t value_mask) {
  if (export_register == ucode::ExportRegister::kVSPosition) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (value_mask & (uint32_t(1) << i)) {
        position_[i] = value[i];
        position_mask_ |= uint32_t(1) << i;
      }
    }
  } else if (export_register == ucode::ExportRegister::kVSPointSizeEdgeFlagKillVertex) {
    if (value_mask & 0b0100) {
      vertex_kill_ = rex::memory::Reinterpret<uint32_t>(value[2]);
    }
  }
}

uint32_t DrawExtentEstimator::GetFullCoverageHeight(const Shader& vertex_shader, uint32_t width,
                                                    uint32_t max_height) {
  SCOPE_profile_cpu_f("gpu");

  const RegisterFile& regs = register_file_;

  if (!width || !max_height) {
    return 0;
  }

  // One triangle, rectangle or quad, with indices from memory or generated.
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  uint32_t vertex_count = vgt_draw_initiator.num_indices;
  xenos::PrimitiveType prim_type = vgt_draw_initiator.prim_type;
  bool is_rectangle = false;
  if (vertex_count == 3) {
    is_rectangle = prim_type == xenos::PrimitiveType::kRectangleList;
    if (!is_rectangle && prim_type != xenos::PrimitiveType::kTriangleList &&
        prim_type != xenos::PrimitiveType::kTriangleStrip &&
        prim_type != xenos::PrimitiveType::kTriangleFan) {
      return 0;
    }
  } else if (vertex_count == 4) {
    if (prim_type != xenos::PrimitiveType::kTriangleStrip &&
        prim_type != xenos::PrimitiveType::kTriangleFan &&
        prim_type != xenos::PrimitiveType::kQuadList) {
      return 0;
    }
  } else {
    return 0;
  }
  if (vgt_draw_initiator.source_select != xenos::SourceSelect::kDMA &&
      vgt_draw_initiator.source_select != xenos::SourceSelect::kAutoIndex) {
    return 0;
  }
  if (xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode, prim_type) &&
      regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
          xenos::VGTOutputPath::kTessellationEnable) {
    return 0;
  }
  assert_true(vertex_shader.type() == xenos::ShaderType::kVertex);
  assert_true(vertex_shader.is_ucode_analyzed());
  if (!ShaderInterpreter::CanInterpretShader(vertex_shader)) {
    return 0;
  }

  // The scissor limits the height, and must contain the width (not clamped to the surface pitch:
  // the width may be the EDRAM tile-aligned one, slightly larger than the pitch).
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor, false);
  if (scissor.offset[0] || scissor.offset[1] || scissor.extent[0] < width) {
    return 0;
  }
  max_height = std::min(max_height, scissor.extent[1]);
  if (!max_height) {
    return 0;
  }

  // Culling and polygon mode can leave the primitive undrawn or drawn as lines or points.
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (!is_rectangle) {
    if (pa_su_sc_mode_cntl.cull_front || pa_su_sc_mode_cntl.cull_back) {
      return 0;
    }
    if (pa_su_sc_mode_cntl.poly_mode == xenos::PolygonModeEnable::kDualMode &&
        (pa_su_sc_mode_cntl.polymode_front_ptype != xenos::PolygonType::kTriangles ||
         pa_su_sc_mode_cntl.polymode_back_ptype != xenos::PolygonType::kTriangles)) {
      return 0;
    }
  }

  // With clipping, only what's inside the viewport and the depth range is drawn: require the
  // viewport to contain the rectangle and check the depth of each vertex; no user clip planes.
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  bool clipping = !pa_cl_clip_cntl.clip_disable;
  if (pa_cl_clip_cntl.value & 0b111111) {
    return 0;
  }
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  float viewport_x_scale =
      pa_cl_vte_cntl.vport_x_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE) : 1.0f;
  float viewport_x_offset =
      pa_cl_vte_cntl.vport_x_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET) : 0.0f;
  float viewport_y_scale =
      pa_cl_vte_cntl.vport_y_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.0f;
  float viewport_y_offset =
      pa_cl_vte_cntl.vport_y_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f;
  if (clipping) {
    if (pa_cl_vte_cntl.vtx_xy_fmt) {
      return 0;
    }
    // Normalized device coordinates -1...1 must map to at least 0...width, and the viewport
    // limits the height.
    if (!(viewport_x_offset - std::fabs(viewport_x_scale) <= 0.0f &&
          viewport_x_offset + std::fabs(viewport_x_scale) >= float(width) &&
          viewport_y_offset - std::fabs(viewport_y_scale) <= 0.0f)) {
      return 0;
    }
    float viewport_bottom = viewport_y_offset + std::fabs(viewport_y_scale);
    if (!(viewport_bottom >= 1.0f)) {
      return 0;
    }
    max_height = std::min(max_height, uint32_t(std::min(viewport_bottom, 65536.0f)));
  }
  float vertex_x_offset = 0.0f, vertex_y_offset = 0.0f;
  if (pa_su_sc_mode_cntl.vtx_window_offset_enable) {
    auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    vertex_x_offset += float(pa_sc_window_offset.window_x_offset);
    vertex_y_offset += float(pa_sc_window_offset.window_y_offset);
  }
  if (regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero) {
    // Pixel centers at integer coordinates: the same as vertices shifted by half a pixel with
    // centers at .5 (see EstimateVertexMaxY).
    vertex_x_offset += 0.5f;
    vertex_y_offset += 0.5f;
  }

  // Fetch the indices like the primitive processor and EstimateVertexMaxY do.
  union {
    const void* index_buffer;
    const uint16_t* index_buffer_16;
    const uint32_t* index_buffer_32;
  };
  index_buffer = nullptr;
  auto vgt_dma_size = regs.Get<reg::VGT_DMA_SIZE>();
  xenos::Endian index_endian = vgt_dma_size.swap_mode;
  if (vgt_draw_initiator.source_select == xenos::SourceSelect::kDMA) {
    uint32_t index_buffer_base = regs[XE_GPU_REG_VGT_DMA_BASE];
    if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16) {
      if (index_endian == xenos::Endian::k8in32) {
        index_endian = xenos::Endian::k8in16;
      } else if (index_endian == xenos::Endian::k16in32) {
        index_endian = xenos::Endian::kNone;
      }
      index_buffer_base &= ~uint32_t(sizeof(uint16_t) - 1);
    } else {
      assert_true(vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32);
      index_buffer_base &= ~uint32_t(sizeof(uint32_t) - 1);
    }
    index_buffer = memory_.TranslatePhysical(index_buffer_base);
  }
  uint32_t reset_index = regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
  uint32_t index_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  uint32_t min_index = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;
  uint32_t max_index = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;

  // Run the vertex shader for the vertices, getting their screen-space positions.
  double vertices[4][2];
  shader_interpreter_.SetShader(vertex_shader);
  PositionExportSink position_export_sink;
  shader_interpreter_.SetExportSink(&position_export_sink);
  bool vertices_ok = true;
  for (uint32_t i = 0; i < vertex_count; ++i) {
    uint32_t vertex_index;
    if (vgt_draw_initiator.source_select == xenos::SourceSelect::kDMA) {
      if (i < vgt_dma_size.num_words) {
        if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16) {
          vertex_index = index_buffer_16[i];
        } else {
          vertex_index = index_buffer_32[i];
        }
        vertex_index = xenos::GpuSwap(vertex_index, index_endian) & 0xFFFFFF;
      } else {
        vertex_index = 0;
      }
      if (pa_su_sc_mode_cntl.multi_prim_ib_ena && vertex_index == reset_index) {
        vertices_ok = false;
        break;
      }
    } else {
      vertex_index = i;
    }
    vertex_index =
        std::min(max_index, std::max(min_index, (vertex_index + index_offset) & 0xFFFFFF));

    position_export_sink.Reset();
    shader_interpreter_.temp_registers()[0] = float(vertex_index);
    shader_interpreter_.Execute();

    if (position_export_sink.vertex_kill().has_value() &&
        (position_export_sink.vertex_kill().value() & ~(UINT32_C(1) << 31))) {
      vertices_ok = false;
      break;
    }
    uint32_t position_mask = position_export_sink.position_mask();
    const float* position = position_export_sink.position();
    if ((position_mask & 0b0011) != 0b0011) {
      vertices_ok = false;
      break;
    }
    float x = position[0], y = position[1];
    if (!pa_cl_vte_cntl.vtx_xy_fmt) {
      if (!(position_mask & 0b1000) || !(position[3] > 0.0f)) {
        // At or behind the eye - no coverage can be proven.
        vertices_ok = false;
        break;
      }
      x /= position[3];
      y /= position[3];
    }
    if (clipping) {
      if (!(position_mask & 0b0100)) {
        vertices_ok = false;
        break;
      }
      float z = pa_cl_vte_cntl.vtx_z_fmt ? position[2] : position[2] / position[3];
      if (!(z >= (pa_cl_clip_cntl.dx_clip_space_def ? 0.0f : -1.0f) && z <= 1.0f)) {
        vertices_ok = false;
        break;
      }
    }
    x = x * viewport_x_scale + viewport_x_offset + vertex_x_offset;
    y = y * viewport_y_scale + viewport_y_offset + vertex_y_offset;
    if (!std::isfinite(x) || !std::isfinite(y)) {
      vertices_ok = false;
      break;
    }
    vertices[i][0] = x;
    vertices[i][1] = y;
  }
  shader_interpreter_.SetExportSink(nullptr);
  if (!vertices_ok) {
    return 0;
  }

  // The polygon: the triangle; for a rectangle, the parallelogram completed by mirroring the
  // vertex opposite to the longest edge (the diagonal) across it, like the host rectangle list
  // expansion does; for a quad, its vertices in boundary order (a strip's last two swapped), which
  // must form a convex polygon for the containment test below to be valid.
  if (is_rectangle) {
    auto edge_length_sq = [&](uint32_t a, uint32_t b) {
      double dx = vertices[b][0] - vertices[a][0], dy = vertices[b][1] - vertices[a][1];
      return dx * dx + dy * dy;
    };
    double len_12 = edge_length_sq(1, 2), len_20 = edge_length_sq(2, 0),
           len_01 = edge_length_sq(0, 1);
    // c is the vertex opposite to the longest edge (a, b); the order is c, a, a + b - c, b.
    uint32_t a, b, c;
    if (len_12 > len_20 && len_12 > len_01) {
      a = 1, b = 2, c = 0;
    } else if (len_20 > len_01) {
      a = 2, b = 0, c = 1;
    } else {
      a = 0, b = 1, c = 2;
    }
    double polygon[4][2] = {
        {vertices[c][0], vertices[c][1]},
        {vertices[a][0], vertices[a][1]},
        {vertices[a][0] + vertices[b][0] - vertices[c][0],
         vertices[a][1] + vertices[b][1] - vertices[c][1]},
        {vertices[b][0], vertices[b][1]},
    };
    std::memcpy(vertices, polygon, sizeof(polygon));
    vertex_count = 4;
  } else if (vertex_count == 4) {
    if (prim_type == xenos::PrimitiveType::kTriangleStrip) {
      std::swap(vertices[2][0], vertices[3][0]);
      std::swap(vertices[2][1], vertices[3][1]);
    }
    double cross_min = 0.0, cross_max = 0.0;
    for (uint32_t i = 0; i < 4; ++i) {
      const double* p0 = vertices[i];
      const double* p1 = vertices[(i + 1) % 4];
      const double* p2 = vertices[(i + 2) % 4];
      double cross = (p1[0] - p0[0]) * (p2[1] - p1[1]) - (p1[1] - p0[1]) * (p2[0] - p1[0]);
      cross_min = std::min(cross_min, cross);
      cross_max = std::max(cross_max, cross);
    }
    if (!(cross_min >= 0.0 || cross_max <= 0.0)) {
      // Concave or self-intersecting (or NaN).
      return 0;
    }
  }

  // The convex polygon contains the rectangle [0, width] x [0, height] if it contains its four
  // corners (and isn't degenerate). Sample positions are strictly inside the pixel rectangle, so
  // the corners being on the polygon's edges is fine regardless of the fill rules. For each edge
  // (a half-plane), the top corners (x, 0) must be inside, and the bottom corners (x, height)
  // give a linear bound on the height.
  double area2 = 0.0;
  for (uint32_t i = 0; i < vertex_count; ++i) {
    const double* p0 = vertices[i];
    const double* p1 = vertices[(i + 1) % vertex_count];
    area2 += p0[0] * p1[1] - p1[0] * p0[1];
  }
  if (!(std::fabs(area2) > 0.0)) {
    return 0;
  }
  double orientation = area2 > 0.0 ? 1.0 : -1.0;
  double height = double(max_height);
  const double corner_xs[2] = {0.0, double(width)};
  for (uint32_t i = 0; i < vertex_count; ++i) {
    const double* p0 = vertices[i];
    const double* p1 = vertices[(i + 1) % vertex_count];
    double ex = p1[0] - p0[0], ey = p1[1] - p0[1];
    for (double corner_x : corner_xs) {
      // Inside if (ex * (y - p0.y) - ey * (x - p0.x)) * orientation >= 0, or a * y + b >= 0.
      double a = ex * orientation;
      double b = (-ex * p0[1] - ey * (corner_x - p0[0])) * orientation;
      if (!(b >= 0.0)) {
        return 0;
      }
      if (a < 0.0) {
        height = std::min(height, -b / a);
      }
    }
  }
  if (!(height >= 1.0)) {
    return 0;
  }
  return uint32_t(std::floor(height));
}

}  // namespace rex::graphics
