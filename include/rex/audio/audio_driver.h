/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <rex/kernel.h>
#include <rex/memory.h>

namespace rex::audio {

class AudioDriver {
 public:
  explicit AudioDriver(memory::Memory* memory);
  virtual ~AudioDriver();

  virtual void SubmitFrame(uint32_t samples_ptr) = 0;

  // The device played silence for want of the client's frames, and as many of the client's next frames are dropped
  // to catch up (audio_keep_time): the client is called for them without the usual spacing.
  virtual bool CatchingUp() const { return false; }

  // How many of the client's frames may be queued for this driver (at most `asked`, audio_maxqframes).
  virtual uint32_t QueuedFrames(uint32_t asked) const { return asked; }

 protected:
  inline uint8_t* TranslatePhysical(uint32_t guest_address) const {
    return memory_->TranslatePhysical(guest_address);
  }

  memory::Memory* memory_ = nullptr;
};

}  // namespace rex::audio
