#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "render/resource/rendererGpuImageToken.h"

namespace VL
{

// Neutral external-texture payloads for the RmlUi render interface.
// They describe only what the overlay can upload or borrow (CPU pixels or a
// renderer-owned image token). Editor document/thumbnail snapshots adapt or
// alias these types; no editor header participates in this contract.
struct UiTexturePreviewImage
{
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;
};

struct UiThumbnail
{
    std::string source;
    std::string diagnostic;
    std::shared_ptr<const UiTexturePreviewImage> image;
    bool diskCacheHit = false;
    std::shared_ptr<const RendererGpuImageToken> gpuImage;
};

// Only the current visible window is published. An empty snapshot still
// revokes the previous sources, so "no thumbnails" must not be skipped.
struct UiThumbnailSnapshot
{
    uint64_t revision = 0;
    std::unordered_map<std::string, UiThumbnail> entries;
};

// View facet of a document snapshot: revision plus the CPU preview pixels the
// render interface pins. Editor document snapshots derive from this type, so
// the shared_ptr identity used for "same document" checks is preserved.
struct UiTexturePreviewDocument
{
    uint64_t revision = 0;
    std::shared_ptr<const UiTexturePreviewImage> texturePreview;
};

inline std::string UiTexturePreviewSource(uint64_t revision)
{
    return "vl://texture-preview/" + std::to_string(revision);
}

} // namespace VL
