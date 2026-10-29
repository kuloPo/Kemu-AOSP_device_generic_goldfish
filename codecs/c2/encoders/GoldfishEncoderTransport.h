// Copyright 2026 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <goldfish_address_space.h>

#include <cstring>

#include "GoldfishVideoEncoderDefs.h"

namespace android {
class GoldfishEncoderTransport {
 public:
  explicit GoldfishEncoderTransport(bool hevc) : mCodec(hevc ? 5 : 4) {}
  ~GoldfishEncoderTransport() { close(); }
  GoldfishEncoderTransport(const GoldfishEncoderTransport&) = delete;
  GoldfishEncoderTransport& operator=(const GoldfishEncoderTransport&) = delete;

  bool open() {
    if (mMemory) return true;
    mHandle = goldfish_address_space_open();
    if (mHandle < 0) return false;
    uint64_t physical = 0;
    if (!goldfish_address_space_allocate(mHandle, goldfish_encoder::kMemorySize,
                                         &physical, &mOffset)) {
      close();
      return false;
    }
    mAllocated = true;
    mMemory = static_cast<uint8_t*>(goldfish_address_space_map(
        mHandle, mOffset, goldfish_encoder::kMemorySize));
    if (!mMemory) {
      close();
      return false;
    }
    address_space_ping ping = {};
    ping.metadata = GoldfishAddressSpaceSubdeviceType::Media;
    ping.offset = mOffset;
    if (!goldfish_address_space_ping(mHandle, &ping)) {
      close();
      return false;
    }
    mConnected = true;
    return true;
  }

  bool send(uint32_t operation, goldfish_encoder::Request& request) {
    if (!mConnected) return false;
    request.status = goldfish_encoder::kUnavailable;
    std::memcpy(mMemory, &request, sizeof(request));
    address_space_ping ping = {};
    ping.metadata = (uint64_t(mCodec) << 56) | operation;
    ping.offset = mOffset;
    if (!goldfish_address_space_ping(mHandle, &ping)) return false;
    std::memcpy(&request, mMemory, sizeof(request));
    return request.status == goldfish_encoder::kOk;
  }

  uint8_t* input() { return mMemory + goldfish_encoder::kInputOffset; }
  const uint8_t* output() const {
    return mMemory + goldfish_encoder::kOutputOffset;
  }

  void close() {
    if (mMemory) {
      if (mConnected) {
        goldfish_encoder::Request r = {};
        r.version = goldfish_encoder::kVersion;
        send(1, r);
      }
      goldfish_address_space_unmap(mMemory, goldfish_encoder::kMemorySize);
      mMemory = nullptr;
    }
    mConnected = false;
    if (mAllocated) goldfish_address_space_free(mHandle, mOffset);
    mAllocated = false;
    if (mHandle >= 0) goldfish_address_space_close(mHandle);
    mHandle = -1;
  }

 private:
  uint32_t mCodec;
  address_space_handle_t mHandle = -1;
  uint64_t mOffset = 0;
  uint8_t* mMemory = nullptr;
  bool mAllocated = false;
  bool mConnected = false;
};
}  // namespace android
