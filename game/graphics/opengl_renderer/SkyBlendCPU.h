#pragma once

#include "common/dma/dma_chain_read.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/pipelines/opengl.h"
#include "game/graphics/opengl_renderer/SkyBlendCommon.h"

class SkyBlendCPU {
 public:
  SkyBlendCPU();
  ~SkyBlendCPU();

  SkyBlendStats do_sky_blends(DmaFollower& dma,
                              SharedRenderState* render_state,
                              ScopedProfilerNode& prof);

 private:
  GLuint m_textures[2];  // sky, clouds
  static constexpr int m_sizes[2] = {32, 64};
  std::vector<u8> m_texture_data[2];
};
