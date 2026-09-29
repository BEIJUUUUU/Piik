#pragma once

// Direct NVENC SDK encoder.
//
// The Media Foundation H.264 MFT cannot emit B-frames at all: it ignores
// CODECAPI_AVEncMPVDefaultBPictureCount, so bf=0/2/3 produce byte-identical
// bitstreams (measured on the RTX 3080 MFT). Without B-frames the encoder
// reaches only ~78% high-frequency retention on sustained-motion screen
// content, which is what shows up as rounded-off edges and mosaic in games;
// enabling B-frames through this API reaches ~86% at the same bitrate.
//
// The DLL is loaded at runtime, so a machine without an NVIDIA encoder simply
// keeps using the Media Foundation path.

#include "h264_encoder.h"

#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace piik::capture::windows {

// Returns nullptr when no NVIDIA encoder session can be created, so the caller
// can fall back to the Media Foundation transform.
std::unique_ptr<VideoEncoder> CreateNvencEncoder(ID3D11Device* device,
                                                 ID3D11DeviceContext* context,
                                                 const VideoProfile& profile);

}  // namespace piik::capture::windows
