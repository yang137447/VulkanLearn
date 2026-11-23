"""Prepare, but do not apply, the two-file graph leaf ownership extension."""

import difflib
from pathlib import Path


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise RuntimeError("Graph ownership patch anchor is not unique")
    return source.replace(before, after, 1)


def main():
    root = Path(__file__).resolve().parents[3]
    header_path = "source/renderGraph.h"
    source_path = "source/renderGraphPassResources.cpp"
    header = (root / header_path).read_text("utf-8")
    source = (root / source_path).read_text("utf-8")
    updated_header = replace_once(header, "struct RenderResource\n{\n",
                                  "struct RenderResource\n{\n    std::shared_ptr<const void> CaptureLifetimeOwner() const;\n")
    updated = replace_once(source, "#include <memory>\n", "#include <memory>\n#include <map>\n#include <mutex>\n")
    updated = replace_once(updated, "namespace\n{\n", """namespace
{
std::mutex terrainDepthOwnerMutex;
std::map<vk::Image, std::shared_ptr<void>> terrainDepthOwners;
""")
    anchor = "RenderResource RenderGraph::CreateRenderResource("
    method = """std::shared_ptr<const void> RenderResource::CaptureLifetimeOwner() const
{
    std::lock_guard<std::mutex> lock(terrainDepthOwnerMutex);
    const auto found = terrainDepthOwners.find(image);
    return found == terrainDepthOwners.end() ? std::shared_ptr<const void>{} : found->second;
}

"""
    updated = replace_once(updated, anchor, method + anchor)
    before = """        resource.samplerHandle =
            rendererBackend.GetSamplerHandle(
                resource.sampler);
        return resource;
"""
    after = """        resource.samplerHandle =
            rendererBackend.GetSamplerHandle(
                resource.sampler);
        if (resource.name == "terrainFeedbackDepth")
        {
            auto owner = std::make_shared<RetiredRenderGraphImageResource>();
            owner->rendererBackend = &rendererBackend;
            RenderResource borrowed = resource;
            std::lock_guard<std::mutex> lock(terrainDepthOwnerMutex);
            const auto inserted = terrainDepthOwners.emplace(resource.image, owner);
            if (!inserted.second)
                throw std::runtime_error("Duplicate graph terrain feedback depth owner");
            owner->resource = std::move(resource);
            resource = std::move(borrowed);
        }
        return resource;
"""
    updated = replace_once(updated, before, after)
    before = """void RenderGraph::DestroyRenderResource(
    RenderResource& resource,
    VL::RendererBackendVulkan& rendererBackend,
    VL::RenderGraphReleaseMode releaseMode)
{
"""
    after = before + """    std::shared_ptr<void> captureOwner;
    {
        std::lock_guard<std::mutex> lock(terrainDepthOwnerMutex);
        const auto found = terrainDepthOwners.find(resource.image);
        if (found != terrainDepthOwners.end())
        {
            captureOwner = std::move(found->second);
            terrainDepthOwners.erase(found);
        }
    }
    if (captureOwner)
    {
        const auto debugName = "RenderGraphResource:" + resource.name;
        ClearRenderResourceFields(resource);
        if (releaseMode == VL::RenderGraphReleaseMode::Retire)
            VL::ResourceRetireQueue::GetInstance().RetireShared(
                debugName, 0, GetGraphResourceLastUsedEpoch(), std::move(captureOwner));
        return;
    }
"""
    updated = replace_once(updated, before, after)
    patch = "".join(difflib.unified_diff(header.splitlines(True), updated_header.splitlines(True),
                                         "a/" + header_path, "b/" + header_path))
    patch += "".join(difflib.unified_diff(source.splitlines(True), updated.splitlines(True),
                                          "a/" + source_path, "b/" + source_path))
    target = root / "artifacts/terrain-rvt-production/p3-noether-graph-depth-owner.patch"
    target.write_text(patch, encoding="utf-8")
    print(target)


if __name__ == "__main__":
    main()
