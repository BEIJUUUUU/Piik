#include "nvenc_encoder.h"

#include <d3d11.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "capture_output.h"
#include "nvEncodeAPI.h"

namespace piik::capture::windows {
namespace {

// Two B-frames per anchor. This is the whole point of the direct API: the
// Media Foundation transform ignores B-frame requests entirely.
constexpr uint32_t kBFrameCount = 2;

// NVENC holds pictures back for reordering when B-frames are on, so both pools
// have to cover the reorder depth on top of the frames in flight. A pool sized
// for IPPP starves the driver: measured at 1080p60 with two B-frames, a 16-deep
// pool stalls on frame 16 and never returns, because a submission whose output
// cannot be completed blocks the lock the drain is waiting on.
constexpr uint32_t kBitstreamPool = 16 + kBFrameCount * 4;
constexpr uint32_t kInputPool = 16 + kBFrameCount * 4;

std::string NvencStage(NVENCSTATUS status) {
  return "nvenc-" + std::to_string(static_cast<int>(status));
}

void CheckNvenc(NVENCSTATUS status, const char* stage) {
  if (status != NV_ENC_SUCCESS && status != NV_ENC_ERR_NEED_MORE_INPUT) {
    Fail(stage, "NVENC returned status " + std::to_string(static_cast<int>(status)));
  }
}

using CreateInstanceFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);

// Owns the runtime-loaded DLL and the function table it hands back.
class NvencApi final {
 public:
  NvencApi() = default;
  NvencApi(NvencApi&& other) noexcept
      : module_(other.module_), functions_(other.functions_) {
    other.module_ = nullptr;
  }
  NvencApi& operator=(NvencApi&& other) noexcept {
    if (this != &other) {
      if (module_) FreeLibrary(module_);
      module_ = other.module_;
      functions_ = other.functions_;
      other.module_ = nullptr;
    }
    return *this;
  }
  ~NvencApi() {
    if (module_) FreeLibrary(module_);
  }
  NvencApi(const NvencApi&) = delete;
  NvencApi& operator=(const NvencApi&) = delete;

  bool Load() {
    module_ = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (module_ == nullptr) return false;
    const auto create = reinterpret_cast<CreateInstanceFn>(
        GetProcAddress(module_, "NvEncodeAPICreateInstance"));
    if (create == nullptr) return false;
    functions_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    return create(&functions_) == NV_ENC_SUCCESS;
  }

  const NV_ENCODE_API_FUNCTION_LIST& functions() const { return functions_; }

 private:
  HMODULE module_ = nullptr;
  NV_ENCODE_API_FUNCTION_LIST functions_{};
};

class NvencEncoder final : public VideoEncoder {
 public:
  NvencEncoder(NvencApi api, ID3D11Device* device, ID3D11DeviceContext* context,
               const VideoProfile& profile)
      : VideoEncoder(OutputKind::h264, "NVENC H.264", "nvenc"),
        api_(std::move(api)),
        device_(device),
        context_(context),
        profile_(profile) {
    if (device_ == nullptr || context_ == nullptr) {
      Fail("nvenc-device", "NVENC needs a D3D11 device and context");
    }
    OpenSession();
    ConfigureEncoder();
    CreateInputPool();
  }

  // Releasing the session is not optional. GeForce drivers cap how many NVENC
  // sessions a process may hold, and a share that leaves its session, registered
  // resources and bitstream buffers behind hands the next share a crippled
  // encoder whose submissions fail: the picture freezes with no error reported.
  // Failures here are ignored because a destructor must not throw.
  ~NvencEncoder() override {
    if (encoder_ == nullptr) return;
    for (auto& slot : inputs_) {
      if (slot.registered != nullptr) {
        api_.functions().nvEncUnregisterResource(encoder_, slot.registered);
      }
    }
    for (auto& buffer : bitstreams_) {
      if (buffer.bitstreamBuffer != nullptr) {
        api_.functions().nvEncDestroyBitstreamBuffer(encoder_,
                                                     buffer.bitstreamBuffer);
      }
    }
    api_.functions().nvEncDestroyEncoder(encoder_);
    encoder_ = nullptr;
  }

  void SetBitrate(UINT32 bitrate) override {
    if (encoder_ == nullptr || bitrate == profile_.bit_rate) return;
    config_.rcParams.averageBitRate = bitrate;
    config_.rcParams.maxBitRate = bitrate;
    NV_ENC_RECONFIGURE_PARAMS reconfigure = {};
    reconfigure.version = NV_ENC_RECONFIGURE_PARAMS_VER;
    // A live bitrate change resets the rate-control state and emits an IDR so
    // the decoder restarts from a known point. Reconfiguring in place instead
    // (resetEncoder = 0) was measured to wedge the driver: output stops and the
    // next calls never return. This mirrors ffmpeg's nvenc reconfig_encoder.
    reconfigure.resetEncoder = 1;
    reconfigure.forceIDR = 1;
    reconfigure.reInitEncodeParams = init_params_;
    reconfigure.reInitEncodeParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
    reconfigure.reInitEncodeParams.encodeConfig = &config_;
    const NVENCSTATUS reconfigured =
        api_.functions().nvEncReconfigureEncoder(encoder_, &reconfigure);
    if (reconfigured == NV_ENC_SUCCESS) {
      profile_.bit_rate = bitrate;
      // forceIDR makes the next submitted picture an IDR, so it has to be
      // reported as a key frame rather than as a delta.
      key_frame_after_reconfigure_ = true;
    }
  }

  EncodedAccessUnit Encode(
      ID3D11Texture2D* texture, UINT64 timestamp100ns, bool force_key_frame,
      EncoderClock::time_point deadline) override {
    RequireEncoderTime(deadline);
    if (texture == nullptr) {
      Fail("nvenc-input", "NVENC needs an input texture");
    }
    if (free_inputs_.empty() || free_bitstreams_.empty()) {
      Fail("nvenc-input-starved", "NVENC input pool was not replenished");
    }

    const uint32_t input_slot = free_inputs_.front();
    free_inputs_.erase(free_inputs_.begin());
    const uint32_t bitstream_slot = free_bitstreams_.front();
    free_bitstreams_.erase(free_bitstreams_.begin());
    // A successful reconfigure already forced an IDR on this picture, so it has
    // to be reported as a key frame rather than as a delta.
    const bool key_frame = force_key_frame || key_frame_after_reconfigure_;
    key_frame_after_reconfigure_ = false;

    // The capture pipeline hands over the selected NV12 surface on this device,
    // so a plain copy keeps the registered pool texture in the right format.
    context_->CopyResource(inputs_[input_slot].texture.Get(), texture);

    NV_ENC_MAP_INPUT_RESOURCE mapping = {};
    mapping.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mapping.registeredResource = inputs_[input_slot].registered;
    CheckNvenc(api_.functions().nvEncMapInputResource(encoder_, &mapping),
               "nvenc-map-input");

    NV_ENC_PIC_PARAMS picture = {};
    picture.version = NV_ENC_PIC_PARAMS_VER;
    picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picture.inputWidth = profile_.width;
    picture.inputHeight = profile_.height;
    picture.inputBuffer = mapping.mappedResource;
    picture.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
    picture.inputTimeStamp = timestamp100ns;
    picture.outputBitstream = bitstreams_[bitstream_slot].bitstreamBuffer;
    if (key_frame) {
      // enablePTD is on, so the encoder picks picture types and FORCEIDR is the
      // supported way to demand an IDR. OUTPUT_SPSPPS makes that IDR carry its
      // own parameter sets even when the preset left repeatSPSPPS off.
      picture.encodePicFlags |= NV_ENC_PIC_FLAG_FORCEIDR |
                               NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    }

    const NVENCSTATUS status =
        api_.functions().nvEncEncodePicture(encoder_, &picture);
    CheckNvenc(api_.functions().nvEncUnmapInputResource(
                   encoder_, mapping.mappedResource),
               "nvenc-unmap-input");
    CheckNvenc(status, "nvenc-encode-picture");

    pending_.push_back(
        Pending{input_slot, bitstream_slot, timestamp100ns, key_frame});
    // NV_ENC_SUCCESS means the driver took this picture and every earlier
    // queued one is ready now, so their output can be read out. Any other
    // status means the reorder queue is still filling, and a lock taken in that
    // state was measured to spin inside the driver forever even with
    // doNotWait = 1, so nothing is drained until the driver reports readiness.
    DrainReady(deadline, status == NV_ENC_SUCCESS);
    return TakeFirst();
  }

 private:
  struct InputSlot final {
    ComPtr<ID3D11Texture2D> texture;
    NV_ENC_REGISTERED_PTR registered = nullptr;
  };
  struct Pending final {
    uint32_t input_slot = 0;
    uint32_t bitstream_slot = 0;
    UINT64 timestamp100ns = 0;
    bool key_frame = false;
  };

  void OpenSession() {
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params = {};
    params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    params.device = device_;
    params.apiVersion = NVENCAPI_VERSION;
    CheckNvenc(api_.functions().nvEncOpenEncodeSessionEx(&params, &encoder_),
               "nvenc-open-session");
    if (encoder_ == nullptr) {
      Fail("nvenc-open-session", "NVENC returned no encoder session");
    }
  }

  void ConfigureEncoder() {
    NV_ENC_PRESET_CONFIG preset = {};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    CheckNvenc(api_.functions().nvEncGetEncodePresetConfigEx(
                   encoder_, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P5_GUID,
                   NV_ENC_TUNING_INFO_HIGH_QUALITY, &preset),
               "nvenc-preset");

    config_ = preset.presetCfg;
    config_.version = NV_ENC_CONFIG_VER;
    config_.profileGUID = NV_ENC_H264_PROFILE_MAIN_GUID;
    config_.gopLength = profile_.gop_frames();
    config_.frameIntervalP = kBFrameCount + 1;
    config_.rcParams.version = NV_ENC_RC_PARAMS_VER;
    config_.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    config_.rcParams.averageBitRate = profile_.bit_rate;
    config_.rcParams.maxBitRate = profile_.bit_rate;
    config_.rcParams.vbvBufferSize = profile_.bit_rate;
    config_.rcParams.vbvInitialDelay = profile_.bit_rate;
    config_.encodeCodecConfig.h264Config.useBFramesAsRef =
        NV_ENC_BFRAME_REF_MODE_DISABLED;
    // Every IDR has to be a self-contained recovery point. A relay or a Viewer
    // joining mid-stream, or WebRTC's PLI, asks for a key frame at any time, and
    // the caller validates that SPS, PPS and IDR arrive together. Without this
    // only the first IDR carries the parameter sets, so the first forced key
    // frame fails that check.
    config_.encodeCodecConfig.h264Config.repeatSPSPPS = 1;

    init_params_ = {};
    init_params_.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init_params_.encodeGUID = NV_ENC_CODEC_H264_GUID;
    init_params_.presetGUID = NV_ENC_PRESET_P5_GUID;
    init_params_.tuningInfo = NV_ENC_TUNING_INFO_HIGH_QUALITY;
    init_params_.encodeWidth = profile_.width;
    init_params_.encodeHeight = profile_.height;
    init_params_.darWidth = profile_.width;
    init_params_.darHeight = profile_.height;
    init_params_.frameRateNum = profile_.frame_rate;
    init_params_.frameRateDen = 1;
    init_params_.enablePTD = 1;
    init_params_.encodeConfig = &config_;
    CheckNvenc(api_.functions().nvEncInitializeEncoder(encoder_, &init_params_),
               "nvenc-initialize");
  }

  void CreateInputPool() {
    D3D11_TEXTURE2D_DESC description = {};
    description.Width = profile_.width;
    description.Height = profile_.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_NV12;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;

    inputs_.resize(kInputPool);
    for (auto& slot : inputs_) {
      Check(device_->CreateTexture2D(&description, nullptr, &slot.texture),
            "nvenc-input-texture");
      NV_ENC_REGISTER_RESOURCE registration = {};
      registration.version = NV_ENC_REGISTER_RESOURCE_VER;
      registration.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
      registration.resourceToRegister = slot.texture.Get();
      registration.width = profile_.width;
      registration.height = profile_.height;
      registration.pitch = 0;
      registration.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
      registration.bufferUsage = NV_ENC_INPUT_IMAGE;
      CheckNvenc(api_.functions().nvEncRegisterResource(encoder_, &registration),
                 "nvenc-register-input");
      slot.registered = registration.registeredResource;
      free_inputs_.push_back(static_cast<uint32_t>(&slot - inputs_.data()));
    }

    bitstreams_.resize(kBitstreamPool);
    for (auto& buffer : bitstreams_) {
      buffer.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
      CheckNvenc(api_.functions().nvEncCreateBitstreamBuffer(encoder_, &buffer),
                 "nvenc-bitstream");
    }
    for (uint32_t slot = 0; slot < kBitstreamPool; ++slot) {
      free_bitstreams_.push_back(slot);
    }
  }

  // Reads out every picture whose output the driver has already produced. The
  // lock never waits: a picture still held back by B-frame reordering stays
  // queued and is picked up on a later call. Waiting instead was measured to
  // hang the encoder permanently (full CPU, no progress past the moment the
  // reorder backlog filled), because this is also the thread that feeds it, so
  // nothing can ever unblock the wait.
  // `output_ready` is only true when nvEncEncodePicture returned NV_ENC_SUCCESS,
  // which is the driver's promise that every earlier queued picture has been
  // produced. Nothing is locked without that promise.
  void DrainReady(EncoderClock::time_point deadline, bool output_ready) {
    if (!output_ready) return;
    while (!pending_.empty()) {
      RequireEncoderTime(deadline);
      const Pending work = pending_.front();
      NV_ENC_LOCK_BITSTREAM locked = {};
      locked.version = NV_ENC_LOCK_BITSTREAM_VER;
      locked.outputBitstream =
          bitstreams_[work.bitstream_slot].bitstreamBuffer;
      locked.doNotWait = 1;
      const NVENCSTATUS status =
          api_.functions().nvEncLockBitstream(encoder_, &locked);
      // Not ready yet: the reorder queue is still filling. Leave it queued and
      // try again when the next picture arrives.
      if (status == NV_ENC_ERR_NEED_MORE_INPUT ||
          status == NV_ENC_ERR_OUT_OF_MEMORY ||
          status == NV_ENC_ERR_LOCK_BUSY) {
        return;
      }
      if (status != NV_ENC_SUCCESS) {
        Fail("nvenc-lock-bitstream",
             "NVENC returned status " + std::to_string(static_cast<int>(status)));
      }
      EncodedAccessUnit unit;
      unit.timestamp100ns = work.timestamp100ns;
      unit.key_frame = work.key_frame;
      const uint32_t size = locked.bitstreamSizeInBytes;
      if (size > 0) {
        const auto* bytes = static_cast<const UINT8*>(locked.bitstreamBufferPtr);
        unit.bytes.assign(bytes, bytes + size);
      }
      (void)api_.functions().nvEncUnlockBitstream(encoder_,
                                                 locked.outputBitstream);
      pending_.erase(pending_.begin());
      free_inputs_.push_back(work.input_slot);
      free_bitstreams_.push_back(work.bitstream_slot);
      if (!unit.bytes.empty()) {
        ready_.push_back(std::move(unit));
      }
    }
  }

  EncodedAccessUnit TakeFirst() {
    if (ready_.empty()) return EncodedAccessUnit{};
    EncodedAccessUnit unit = std::move(ready_.front());
    ready_.erase(ready_.begin());
    // A requested key frame must arrive as a full recovery unit; the caller
    // relies on SPS/PPS/IDR being present together.
    if (unit.key_frame) {
      const NalSummary summary = InspectAnnexB(unit.bytes);
      if (!(summary.sps && summary.pps && summary.idr)) {
        Fail("nvenc-recovery-unit",
             "requested key frame lacks SPS, PPS, or IDR");
      }
    }
    return unit;
  }

  NvencApi api_;
  ID3D11Device* device_ = nullptr;
  ID3D11DeviceContext* context_ = nullptr;
  VideoProfile profile_;
  void* encoder_ = nullptr;
  NV_ENC_INITIALIZE_PARAMS init_params_{};
  NV_ENC_CONFIG config_{};
  // Set by a successful bitrate reconfigure, consumed by the next Encode.
  bool key_frame_after_reconfigure_ = false;
  std::vector<InputSlot> inputs_;
  std::vector<NV_ENC_CREATE_BITSTREAM_BUFFER> bitstreams_;
  std::vector<uint32_t> free_inputs_;
  std::vector<uint32_t> free_bitstreams_;
  std::vector<Pending> pending_;
  std::vector<EncodedAccessUnit> ready_;
};

}  // namespace

std::unique_ptr<VideoEncoder> CreateNvencEncoder(ID3D11Device* device,
                                                 ID3D11DeviceContext* context,
                                                 const VideoProfile& profile) {
  NvencApi api;
  if (!api.Load()) return nullptr;
  try {
    return std::make_unique<NvencEncoder>(std::move(api), device, context,
                                          profile);
  } catch (const GateFailure&) {
    // No NVIDIA session for this device: the caller keeps the Media Foundation
    // transform, so a non-NVIDIA machine is unaffected.
    return nullptr;
  }
}

}  // namespace piik::capture::windows
