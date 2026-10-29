// Copyright 2026 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
#define LOG_TAG "C2GoldfishVideoEnc"
#include <C2PlatformSupport.h>
#include <Codec2BufferUtils.h>
#include <SimpleC2Component.h>
#include <SimpleC2Interface.h>
#include <goldfish_codec2/store/GoldfishComponentStore.h>
#include <libyuv.h>
#include <log/log.h>
#include <media/stagefright/MediaDefs.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "GoldfishEncoderTransport.h"

namespace android {
using namespace goldfish_encoder;
#ifdef GOLDFISH_HEVC_ENCODER
static constexpr bool kHevc = true;
static constexpr char kName[] = "c2.goldfish.hevc.encoder";
#else
static constexpr bool kHevc = false;
static constexpr char kName[] = "c2.goldfish.h264.encoder";
#endif

class EncoderInterface : public SimpleInterface<void>::BaseParams {
 public:
  explicit EncoderInterface(const std::shared_ptr<C2ReflectorHelper>& helper)
      : SimpleInterface<void>::BaseParams(
            helper, kName, C2Component::KIND_ENCODER, C2Component::DOMAIN_VIDEO,
            kHevc ? MEDIA_MIMETYPE_VIDEO_HEVC : MEDIA_MIMETYPE_VIDEO_AVC) {
    noPrivateBuffers();
    noInputReferences();
    noOutputReferences();
    noInputLatency();
    noOutputLatency();
    noPipelineLatency();
    noTimeStretch();
    addParameter(DefineParam(mUsage, C2_PARAMKEY_INPUT_STREAM_USAGE)
                     .withConstValue(new C2StreamUsageTuning::input(
                         0u, uint64_t(C2MemoryUsage::CPU_READ)))
                     .build());
    addParameter(DefineParam(mAttrib, C2_PARAMKEY_COMPONENT_ATTRIBUTES)
                     .withConstValue(new C2ComponentAttributesSetting(
                         C2Component::ATTRIB_IS_TEMPORAL))
                     .build());
    addParameter(
        DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
            .withDefault(new C2StreamPictureSizeInfo::input(0u, 320, 240))
            .withFields({C2F(mSize, width).inRange(160, 2560, 2),
                         C2F(mSize, height).inRange(64, 2560, 2)})
            .withSetter(SizeSetter)
            .build());
    addParameter(
        DefineParam(mFrameRate, C2_PARAMKEY_FRAME_RATE)
            .withDefault(new C2StreamFrameRateInfo::output(0u, 30.))
            .withFields({C2F(mFrameRate, value).inRange(1., 60.)})
            .withSetter(Setter<decltype(*mFrameRate)>::StrictValueWithNoDeps)
            .build());
    addParameter(
        DefineParam(mBitrate, C2_PARAMKEY_BITRATE)
            .withDefault(new C2StreamBitrateInfo::output(0u, 8000000))
            .withFields({C2F(mBitrate, value).inRange(64000, 40000000)})
            .withSetter(Setter<decltype(*mBitrate)>::StrictValueWithNoDeps)
            .build());
    addParameter(
        DefineParam(mBitrateMode, C2_PARAMKEY_BITRATE_MODE)
            .withDefault(new C2StreamBitrateModeTuning::output(
                0u, C2Config::BITRATE_VARIABLE))
            .withFields({C2F(mBitrateMode, value)
                             .oneOf({C2Config::BITRATE_CONST,
                                     C2Config::BITRATE_VARIABLE})})
            .withSetter(Setter<decltype(*mBitrateMode)>::StrictValueWithNoDeps)
            .build());
    if (kHevc) {
      addParameter(
          DefineParam(mProfile, C2_PARAMKEY_PROFILE_LEVEL)
              .withDefault(new C2StreamProfileLevelInfo::output(
                  0u, PROFILE_HEVC_MAIN, LEVEL_HEVC_MAIN_5_1))
              .withFields({C2F(mProfile, profile).oneOf({PROFILE_HEVC_MAIN}),
                           C2F(mProfile, level).oneOf({LEVEL_HEVC_MAIN_5_1})})
              .withSetter(ProfileSetter)
              .build());
    } else {
      addParameter(
          DefineParam(mProfile, C2_PARAMKEY_PROFILE_LEVEL)
              .withDefault(new C2StreamProfileLevelInfo::output(
                  0u, PROFILE_AVC_HIGH, LEVEL_AVC_5_2))
              .withFields({C2F(mProfile, profile)
                               .oneOf({PROFILE_AVC_BASELINE, PROFILE_AVC_MAIN,
                                       PROFILE_AVC_HIGH}),
                           C2F(mProfile, level).oneOf({LEVEL_AVC_5_2})})
              .withSetter(ProfileSetter)
              .build());
    }
    addParameter(
        DefineParam(mRequestSync, C2_PARAMKEY_REQUEST_SYNC_FRAME)
            .withDefault(
                new C2StreamRequestSyncFrameTuning::output(0u, C2_FALSE))
            .withFields({C2F(mRequestSync, value).oneOf({C2_FALSE, C2_TRUE})})
            .withSetter(
                Setter<decltype(*mRequestSync)>::NonStrictValueWithNoDeps)
            .build());
    addParameter(
        DefineParam(mSyncPeriod, C2_PARAMKEY_SYNC_FRAME_INTERVAL)
            .withDefault(
                new C2StreamSyncFrameIntervalTuning::output(0u, 1000000))
            .withFields({C2F(mSyncPeriod, value).any()})
            .withSetter(Setter<decltype(*mSyncPeriod)>::StrictValueWithNoDeps)
            .build());
    addParameter(
        DefineParam(mColor, C2_PARAMKEY_COLOR_ASPECTS)
            .withDefault(new C2StreamColorAspectsInfo::input(
                0u, C2Color::RANGE_LIMITED, C2Color::PRIMARIES_BT601_525,
                C2Color::TRANSFER_170M, C2Color::MATRIX_BT601))
            .withFields(
                {C2F(mColor, range)
                     .oneOf({C2Color::RANGE_UNSPECIFIED, C2Color::RANGE_LIMITED,
                             C2Color::RANGE_FULL}),
                 C2F(mColor, primaries)
                     .oneOf({C2Color::PRIMARIES_UNSPECIFIED,
                             C2Color::PRIMARIES_BT709,
                             C2Color::PRIMARIES_BT601_625,
                             C2Color::PRIMARIES_BT601_525}),
                 C2F(mColor, transfer)
                     .oneOf({C2Color::TRANSFER_UNSPECIFIED,
                             C2Color::TRANSFER_170M, C2Color::TRANSFER_LINEAR,
                             C2Color::TRANSFER_SRGB}),
                 C2F(mColor, matrix)
                     .oneOf({C2Color::MATRIX_UNSPECIFIED, C2Color::MATRIX_BT601,
                             C2Color::MATRIX_BT709})})
            .withSetter(ColorSetter)
            .build());
  }

  static C2R ProfileSetter(bool,
                           const C2P<C2StreamProfileLevelInfo::output>& old,
                           C2P<C2StreamProfileLevelInfo::output>& value) {
    C2R result = C2R::Ok();
    if (!value.F(value.v.profile).supportsAtAll(value.v.profile)) {
      result = result.plus(
          C2SettingResultBuilder::BadValue(value.F(value.v.profile)));
      value.set().profile = old.v.profile;
    }
    if (!value.F(value.v.level).supportsAtAll(value.v.level)) {
      result =
          result.plus(C2SettingResultBuilder::BadValue(value.F(value.v.level)));
      value.set().level = old.v.level;
    }
    return result;
  }

  static C2R ColorSetter(bool, const C2P<C2StreamColorAspectsInfo::input>& old,
                         C2P<C2StreamColorAspectsInfo::input>& value) {
    C2R result = C2R::Ok();
#define VALIDATE_COLOR(field)                                                  \
  if (!value.F(value.v.field).supportsAtAll(value.v.field)) {                  \
    result =                                                                   \
        result.plus(C2SettingResultBuilder::BadValue(value.F(value.v.field))); \
    value.set().field = old.v.field;                                           \
  }
    VALIDATE_COLOR(range)
    VALIDATE_COLOR(primaries)
    VALIDATE_COLOR(transfer)
    VALIDATE_COLOR(matrix)
#undef VALIDATE_COLOR
    return result;
  }

  static C2R SizeSetter(bool, const C2P<C2StreamPictureSizeInfo::input>& old,
                        C2P<C2StreamPictureSizeInfo::input>& value) {
    C2R result = C2R::Ok();
    if (!value.F(value.v.width).supportsAtAll(value.v.width)) {
      result =
          result.plus(C2SettingResultBuilder::BadValue(value.F(value.v.width)));
      value.set().width = old.v.width;
    }
    if (!value.F(value.v.height).supportsAtAll(value.v.height)) {
      result = result.plus(
          C2SettingResultBuilder::BadValue(value.F(value.v.height)));
      value.set().height = old.v.height;
    }
    return result;
  }

  Request request() {
    Request r = {};
    {
      Lock guard = lock();
      r.version = kVersion;
      r.width = mSize->width;
      r.height = mSize->height;
      r.bitrate = mBitrate->value;
      r.frameRate = std::isfinite(mFrameRate->value)
                        ? uint32_t(std::lround(mFrameRate->value * 1000))
                        : 30000;
      r.gop = mSyncPeriod->value < 0
                  ? 0
                  : uint32_t(std::max<int64_t>(
                        1, std::min<int64_t>(
                               UINT32_MAX,
                               std::llround(double(mSyncPeriod->value) *
                                            r.frameRate / 1000000000.))));
      r.profile = kHevc                                   ? 0
                  : mProfile->profile == PROFILE_AVC_HIGH ? 2
                  : mProfile->profile == PROFILE_AVC_MAIN ? 1
                                                          : 0;
      r.level = kHevc ? 153 : 52;
      r.bitrateMode = mBitrateMode->value == C2Config::BITRATE_VARIABLE;
      r.flags = mRequestSync->value ? kForceIdr : 0;
      // The RGB fast path uses NVENC's limited-range BT.601 conversion.
      r.fullRange = mColor->range == C2Color::RANGE_FULL;
      r.primaries = mColor->primaries == C2Color::PRIMARIES_BT709       ? 1
                    : mColor->primaries == C2Color::PRIMARIES_BT601_625 ? 5
                                                                        : 6;
      r.transfer = mColor->transfer == C2Color::TRANSFER_LINEAR ? 8
                   : mColor->transfer == C2Color::TRANSFER_SRGB ? 13
                                                                : 6;
      r.matrix = mColor->matrix == C2Color::MATRIX_BT709 ? 1 : 6;
    }
    if (r.flags) {
      C2StreamRequestSyncFrameTuning::output clear(0u, C2_FALSE);
      std::vector<std::unique_ptr<C2SettingResult>> failures;
      config({&clear}, C2_MAY_BLOCK, &failures);
    }
    return r;
  }

 private:
  std::shared_ptr<C2StreamUsageTuning::input> mUsage;
  std::shared_ptr<C2StreamPictureSizeInfo::input> mSize;
  std::shared_ptr<C2StreamFrameRateInfo::output> mFrameRate;
  std::shared_ptr<C2StreamBitrateInfo::output> mBitrate;
  std::shared_ptr<C2StreamBitrateModeTuning::output> mBitrateMode;
  std::shared_ptr<C2StreamProfileLevelInfo::output> mProfile;
  std::shared_ptr<C2StreamRequestSyncFrameTuning::output> mRequestSync;
  std::shared_ptr<C2StreamSyncFrameIntervalTuning::output> mSyncPeriod;
  std::shared_ptr<C2StreamColorAspectsInfo::input> mColor;
};

class C2GoldfishVideoEnc : public SimpleC2Component {
 public:
  C2GoldfishVideoEnc(c2_node_id_t id,
                     const std::shared_ptr<EncoderInterface>& impl)
      : SimpleC2Component(std::make_shared<SimpleInterface<EncoderInterface>>(
            kName, id, impl)),
        mInterface(impl),
        mTransport(kHevc) {}
  ~C2GoldfishVideoEnc() override { onRelease(); }

 private:
  c2_status_t onInit() override {
    mEos = mError = false;
    if (!mTransport.open()) return C2_NO_MEMORY;
    Request r = mInterface->request();
    if (!mTransport.send(0, r)) {
      ALOGE("Host hardware encoder initialization failed: %d", r.status);
      mTransport.close();
      return C2_CORRUPTED;
    }
    return C2_OK;
  }
  c2_status_t onStop() override {
    onRelease();
    return C2_OK;
  }
  void onReset() override { onRelease(); }
  void onRelease() override {
    mTransport.close();
    mConversion.clear();
    mEos = mError = false;
  }
  c2_status_t onFlush_sm() override {
    mTransport.close();
    mEos = mError = false;
    return C2_OK;
  }
  c2_status_t drain(uint32_t mode,
                    const std::shared_ptr<C2BlockPool>&) override {
    // Synchronous IP-only encoding returns every frame in its input work.
    if (mode == DRAIN_CHAIN) return C2_OMITTED;
    if (mode == DRAIN_COMPONENT_WITH_EOS) mEos = true;
    return C2_OK;
  }

  bool copyInput(const C2GraphicView& view, Request& r) {
    const auto& layout = view.layout();
    auto* dst = mTransport.input();
    const size_t pixels = size_t(r.width) * r.height;
    if (layout.type == C2PlanarLayout::TYPE_RGB ||
        layout.type == C2PlanarLayout::TYPE_RGBA) {
      const auto& red = layout.planes[C2PlanarLayout::PLANE_R];
      const uint8_t* src = view.data()[C2PlanarLayout::PLANE_R];
      if (pixels * 4 <= kBufferCapacity && red.colInc == 4 &&
          red.rowInc >= int32_t(r.width * 4) &&
          view.data()[C2PlanarLayout::PLANE_G] == src + 1 &&
          view.data()[C2PlanarLayout::PLANE_B] == src + 2 && r.matrix == 6 &&
          !r.fullRange) {
        r.format = kRgba;
        r.inputSize = uint32_t(pixels * 4);
        for (uint32_t row = 0; row < r.height; ++row)
          std::memcpy(dst + size_t(row) * r.width * 4,
                      src + size_t(row) * red.rowInc, r.width * 4);
        return true;
      }
      mConversion.resize(pixels * 3 / 2);
      if (ConvertRGBToPlanarYUV(
              mConversion.data(), r.width, r.height, mConversion.size(), view,
              r.matrix == 1 ? C2Color::MATRIX_BT709 : C2Color::MATRIX_BT601,
              r.fullRange ? C2Color::RANGE_FULL : C2Color::RANGE_LIMITED) != OK)
        return false;
      if (libyuv::I420ToNV12(
              mConversion.data(), r.width, mConversion.data() + pixels,
              r.width / 2, mConversion.data() + pixels * 5 / 4, r.width / 2,
              dst, r.width, dst + pixels, r.width, r.width, r.height))
        return false;
    } else if (layout.type == C2PlanarLayout::TYPE_YUV && IsYUV420(view)) {
      const MediaImage2 image = CreateYUV420SemiPlanarMediaImage2(
          r.width, r.height, r.width, r.height);
      if (ImageCopy(dst, &image, view) != OK) return false;
    } else {
      return false;
    }
    r.format = kNv12;
    r.inputSize = uint32_t(pixels * 3 / 2);
    return true;
  }

  void process(const std::unique_ptr<C2Work>& work,
               const std::shared_ptr<C2BlockPool>& pool) override {
    work->workletsProcessed = 1u;
    work->result = C2_OK;
    auto& output = work->worklets.front()->output;
    output.flags = C2FrameData::flags_t(work->input.flags &
                                        C2FrameData::FLAG_END_OF_STREAM);
    output.ordinal = work->input.ordinal;
    output.buffers.clear();
    output.configUpdate.clear();
    if (mEos || mError) {
      work->result = C2_BAD_STATE;
      return;
    }
    auto fail = [&](c2_status_t error) {
      work->result = error;
      mError = true;
    };
    const bool eos = work->input.flags & C2FrameData::FLAG_END_OF_STREAM;
    if (work->input.buffers.empty()) {
      mEos = eos;
      return;
    }
    const auto& blocks = work->input.buffers.front()->data().graphicBlocks();
    if (blocks.empty()) {
      fail(C2_BAD_VALUE);
      return;
    }
    Request r = mInterface->request();
    C2GraphicView view = blocks.front().map().get();
    if (view.error() != C2_OK) {
      fail(view.error());
      return;
    }
    if (view.width() < r.width || view.height() < r.height) {
      fail(C2_BAD_VALUE);
      return;
    }
    view.setCrop_be(C2Rect(r.width, r.height));
    if (!mTransport.open()) {
      fail(C2_NO_MEMORY);
      return;
    }
    if (!copyInput(view, r)) {
      fail(C2_BAD_VALUE);
      return;
    }
    r.timestamp = work->input.ordinal.timestamp.peekull();
    if (!mTransport.send(7, r)) {
      ALOGE("Host hardware encoding failed: %d", r.status);
      fail(C2_CORRUPTED);
      return;
    }
    if (r.configSize > kBufferCapacity ||
        r.outputSize > kBufferCapacity - r.configSize || !r.outputSize) {
      fail(C2_CORRUPTED);
      return;
    }
    if (r.configSize) {
      auto csd = C2StreamInitDataInfo::output::AllocUnique(r.configSize, 0u);
      if (!csd) {
        fail(C2_NO_MEMORY);
        return;
      }
      std::memcpy(csd->m.value, mTransport.output(), r.configSize);
      output.configUpdate.push_back(std::move(csd));
    }
    std::shared_ptr<C2LinearBlock> block;
    c2_status_t status = pool->fetchLinearBlock(
        r.outputSize,
        C2MemoryUsage(C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE),
        &block);
    if (status != C2_OK) {
      fail(status);
      return;
    }
    C2WriteView bytes = block->map().get();
    if (bytes.error() != C2_OK) {
      fail(bytes.error());
      return;
    }
    std::memcpy(bytes.data(), mTransport.output() + r.configSize, r.outputSize);
    auto buffer = createLinearBuffer(block, 0, r.outputSize);
    if (r.keyFrame)
      buffer->setInfo(std::make_shared<C2StreamPictureTypeMaskInfo::output>(
          0u, C2Config::SYNC_FRAME));
    output.buffers.push_back(std::move(buffer));
    mEos = eos;
  }

  std::shared_ptr<EncoderInterface> mInterface;
  GoldfishEncoderTransport mTransport;
  std::vector<uint8_t> mConversion;
  bool mEos = false;
  bool mError = false;
};

class EncoderFactory : public C2ComponentFactory {
 public:
  EncoderFactory()
      : mHelper(std::static_pointer_cast<C2ReflectorHelper>(
            GoldfishComponentStore::Create()->getParamReflector())) {}
  c2_status_t createComponent(
      c2_node_id_t id, std::shared_ptr<C2Component>* const component,
      std::function<void(C2Component*)> deleter) override {
    *component = std::shared_ptr<C2Component>(
        new C2GoldfishVideoEnc(id, std::make_shared<EncoderInterface>(mHelper)),
        deleter);
    return C2_OK;
  }
  c2_status_t createInterface(
      c2_node_id_t id, std::shared_ptr<C2ComponentInterface>* const interface,
      std::function<void(C2ComponentInterface*)> deleter) override {
    *interface = std::shared_ptr<C2ComponentInterface>(
        new SimpleInterface<EncoderInterface>(
            kName, id, std::make_shared<EncoderInterface>(mHelper)),
        deleter);
    return C2_OK;
  }

 private:
  std::shared_ptr<C2ReflectorHelper> mHelper;
};
}  // namespace android

extern "C" C2ComponentFactory* CreateCodec2Factory() {
  return new android::EncoderFactory();
}
extern "C" void DestroyCodec2Factory(C2ComponentFactory* factory) {
  delete factory;
}
