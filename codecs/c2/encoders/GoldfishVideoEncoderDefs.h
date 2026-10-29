// Copyright 2026 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

// Wire ABI shared with device/generic/goldfish/codecs/c2/encoders.
namespace goldfish_encoder {
constexpr uint32_t kVersion = 1;
constexpr uint32_t kInputOffset = 4096;
constexpr uint32_t kBufferCapacity = 16 * 1024 * 1024;
constexpr uint32_t kOutputOffset = kInputOffset + kBufferCapacity;
constexpr uint32_t kMemorySize = kOutputOffset + kBufferCapacity;
constexpr uint32_t kNv12 = 0;
constexpr uint32_t kRgba = 1;
constexpr uint32_t kForceIdr = 1;
constexpr int32_t kOk = 0;
constexpr int32_t kUnavailable = -1;
constexpr int32_t kBadRequest = -2;
constexpr int32_t kEncodeFailed = -3;

struct Request {
  uint32_t version;
  int32_t status;
  uint32_t width;
  uint32_t height;
  uint32_t bitrate;
  uint32_t frameRate;  // Frames per second multiplied by 1000.
  uint32_t gop;
  uint32_t format;
  uint32_t profile;  // AVC: 0=baseline, 1=main, 2=high. HEVC: 0=main.
  uint32_t level;    // AVC level_idc or HEVC general_level_idc; 0=automatic.
  uint32_t bitrateMode;  // 0=CBR, 1=VBR.
  uint32_t flags;
  uint32_t inputSize;
  uint32_t outputSize;
  uint32_t configSize;
  uint32_t keyFrame;
  uint32_t fullRange;
  uint32_t primaries;  // ISO/IEC 23091-2 values.
  uint32_t transfer;
  uint32_t matrix;
  uint64_t timestamp;
};
static_assert(sizeof(Request) == 88, "Encoder protocol layout changed");
}  // namespace goldfish_encoder
