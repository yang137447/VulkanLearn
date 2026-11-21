#include "terrain/terrainCompositedProduction.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace VL
{
namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(std::string("Terrain composited production: ") + message);
}

void Text(const std::string& value)
{
    Require(!value.empty() && value.size() <= 4096 && value.find('\0') == std::string::npos,
        "invalid text/identity");
}

void Path(const std::string& value, bool asciiObject = false)
{
    Text(value);
    Require(value.front() != '/' && value.back() != '/' && value.find('\\') == std::string::npos &&
        value.find(':') == std::string::npos, "unsafe input object path");
    size_t start = 0;
    while (start < value.size())
    {
        const auto end = value.find('/', start);
        const auto part = value.substr(start, end == std::string::npos ? end : end - start);
        Require(!part.empty() && part != "." && part != ".." && part.back() != '.' &&
            part.back() != ' ' && part.find_first_of("<>\"|?*") == std::string::npos,
            "unsafe input object path component");
        auto device = part.substr(0, part.find('.'));
        for (auto& character : device)
            if (character >= 'a' && character <= 'z')
                character = char(character - 'a' + 'A');
        Require(device != "CON" && device != "PRN" && device != "AUX" && device != "NUL" &&
            device != "CONIN$" && device != "CONOUT$" &&
            !(device.size() == 4 && (device.substr(0, 3) == "COM" || device.substr(0, 3) == "LPT") &&
                device[3] >= '1' && device[3] <= '9'), "Windows device path");
        for (const unsigned char character : part)
        {
            Require(character >= 32, "control character in path");
            if (asciiObject)
                Require((character >= 'a' && character <= 'z') ||
                    (character >= 'A' && character <= 'Z') ||
                    (character >= '0' && character <= '9') ||
                    character == '-' || character == '_' || character == '.',
                    "input object path requires ASCII letters, digits, dash, underscore or dot");
        }
        start = end == std::string::npos ? value.size() : end + 1;
    }
}

template<size_t Count>
void Finite(const std::array<double, Count>& values)
{
    for (const auto value : values)
        Require(std::isfinite(value), "non-finite numeric input");
}

void Number(CanonicalFieldHasher& hash, const char* name, double value)
{
    Require(std::isfinite(value), "non-finite hash input");
    if (value == 0)
        value = 0;
    uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    hash.AddUInt64(name, bits);
}

template<size_t Count>
void Numbers(CanonicalFieldHasher& hash, const char* name, const std::array<double, Count>& values)
{
    for (const auto value : values)
        Number(hash, name, value);
}

std::vector<std::string> Bindings(const TerrainCompositedProductionBindings& inputs)
{
    return {inputs.coveragePageTable, inputs.coverageIds, inputs.coverageWeights,
        inputs.localLayerWeights, inputs.layerCatalog, inputs.layerAlbedo, inputs.layerNormal,
        inputs.layerOrm, inputs.layerHeight, inputs.surfaceLocalNormal, inputs.surfaceLocalTangent,
        inputs.heightAtlas, inputs.heightLayerMap, inputs.globalColour, inputs.globalMasks};
}

void Placement(const std::array<double, 16>& matrix)
{
    Finite(matrix);
    Require(matrix[3] == 0 && matrix[7] == 0 && matrix[11] == 0 && matrix[15] == 1,
        "non-affine placement");
    std::array<double, 3> lengths{};
    for (size_t column = 0; column < 3; ++column)
        for (size_t row = 0; row < 3; ++row)
            lengths[column] += matrix[column * 4 + row] * matrix[column * 4 + row];
    Require(std::isfinite(lengths[0]) && lengths[0] > 1.e-18, "degenerate placement");
    for (size_t column = 0; column < 3; ++column)
    {
        Require(std::isfinite(lengths[column]) &&
            std::abs(lengths[column] - lengths[0]) <= lengths[0] * 1.e-6,
            "nonuniform placement scale");
        for (size_t other = column + 1; other < 3; ++other)
        {
            double product = 0;
            for (size_t row = 0; row < 3; ++row)
                product += matrix[column * 4 + row] * matrix[other * 4 + row];
            Require(std::abs(product) <= lengths[0] * 1.e-6, "sheared placement");
        }
    }
    const double determinant = matrix[0] * (matrix[5] * matrix[10] - matrix[9] * matrix[6]) -
        matrix[4] * (matrix[1] * matrix[10] - matrix[9] * matrix[2]) +
        matrix[8] * (matrix[1] * matrix[6] - matrix[5] * matrix[2]);
    Require(std::isfinite(determinant) && determinant > 0, "mirrored/invalid placement");
}
}

uint64_t TerrainCompositedProductionImageDesc::ByteSize() const
{
    Require(width > 0 && height > 0 && width <= 32768 && height <= 32768 &&
        layers > 0 && layers <= 256 && mipLevels > 0 && mipLevels <= 16,
        "invalid image dimensions");
    Require(transfer == TerrainCompositedProductionTransfer::Linear ||
        transfer == TerrainCompositedProductionTransfer::Srgb, "invalid transfer");
    Require(address == TerrainCompositedProductionAddress::Clamp ||
        address == TerrainCompositedProductionAddress::Repeat, "invalid address mode");
    Require(filter == TerrainCompositedProductionFilter::Nearest ||
        filter == TerrainCompositedProductionFilter::Linear, "invalid filter");
    const std::map<std::string, uint32_t> formats{{"r8-unorm", 1}, {"rg8-unorm", 2},
        {"rgba8-unorm", 4}, {"rgba8-srgb", 4}, {"r16-unorm", 2},
        {"r32-float", 4}, {"rgba32-float", 16}, {"bc5-unorm", 16},
        {"bc7-unorm", 16}, {"bc7-srgb", 16}};
    const auto found = formats.find(format);
    Require(found != formats.end(), "unsupported image format");
    const bool srgb = format == "rgba8-srgb" || format == "bc7-srgb";
    Require(srgb == (transfer == TerrainCompositedProductionTransfer::Srgb),
        "format/transfer mismatch");
    const bool block = format.rfind("bc", 0) == 0;
    uint64_t bytes = 0;
    uint32_t mipWidth = width;
    uint32_t mipHeight = height;
    for (uint32_t mip = 0; mip < mipLevels; ++mip)
    {
        bytes += uint64_t(block ? (mipWidth + 3) / 4 : mipWidth) *
            (block ? (mipHeight + 3) / 4 : mipHeight) * found->second * layers;
        Require(mip + 1 == mipLevels || mipWidth > 1 || mipHeight > 1, "excess image mips");
        mipWidth = std::max(1u, mipWidth / 2);
        mipHeight = std::max(1u, mipHeight / 2);
    }
    return bytes;
}

const TerrainCompositedProductionObject& TerrainCompositedProductionClosure::Object(
    const std::string& objectId) const
{
    for (const auto& object : objects)
        if (object.objectId == objectId)
            return object;
    throw std::runtime_error("Terrain composited production: missing object " + objectId);
}

void TerrainCompositedProductionClosure::Validate() const
{
    Require(!objects.empty() && objects.size() <= 65536 && !surfaces.empty() && surfaces.size() <= 256,
        "invalid closure size");
    std::set<std::string> objectIds;
    std::set<std::string> paths;
    for (const auto& object : objects)
    {
        Text(object.objectId);
        Path(object.relativePath, true);
        std::string folded = object.relativePath;
        for (auto& character : folded)
            if (character >= 'A' && character <= 'Z')
                character = char(character - 'A' + 'a');
        Require(objectIds.insert(object.objectId).second && paths.insert(folded).second,
            "duplicate object identity/path");
        Require(object.bytes > 0 && object.digest != ContentDigest{}, "empty object size/digest");
        if (object.kind == TerrainCompositedProductionObjectKind::Image)
            Require(object.bytes == object.image.ByteSize(), "image object byte size mismatch");
        else
        {
            Require(object.kind == TerrainCompositedProductionObjectKind::ShaderSource ||
                object.kind == TerrainCompositedProductionObjectKind::ShaderArtifact ||
                object.kind == TerrainCompositedProductionObjectKind::RawAuthorSource, "unknown object kind");
            Require(object.image.format.empty() && !object.image.width && !object.image.height &&
                object.image.layers == 1 && object.image.mipLevels == 1 &&
                object.image.transfer == TerrainCompositedProductionTransfer::Linear &&
                object.image.address == TerrainCompositedProductionAddress::Clamp &&
                object.image.filter == TerrainCompositedProductionFilter::Nearest &&
                object.baseCatalogDigest == ContentDigest{} && object.heightBlendCatalogDigest == ContentDigest{},
                "non-image object carries image/catalog metadata");
        }
    }
    Require(generator.version == TerrainCompositedGeneratorVersion && generator.evaluator == "M_terrain" &&
        generator.filterRule == TerrainCompositedFilterRule &&
        generator.encoding == TerrainCompositedEncoding,
        "unsupported generator/evaluator/filter/encoding");
    Text(generator.compilePolicy);
    const auto shader = [&](const std::string& id, TerrainCompositedProductionObjectKind kind)
    {
        Require(Object(id).kind == kind, "shader object kind mismatch");
    };
    shader(generator.computeSource, TerrainCompositedProductionObjectKind::ShaderSource);
    std::set<std::string> shaderIds{generator.computeSource};
    for (const auto& id : generator.includeSources)
    {
        Require(shaderIds.insert(id).second, "duplicate shader source");
        shader(id, TerrainCompositedProductionObjectKind::ShaderSource);
    }
    Require(!generator.compiledArtifacts.empty(), "missing compiled generator");
    for (const auto& id : generator.compiledArtifacts)
    {
        Require(shaderIds.insert(id).second, "duplicate shader artifact");
        shader(id, TerrainCompositedProductionObjectKind::ShaderArtifact);
    }
    std::vector<std::string> ids;
    for (const auto& surface : surfaces)
    {
        Text(surface.surfaceId);
        Require(surface.authorDependencies.size() <= 65536, "author dependency count exceeded");
        std::set<std::tuple<std::string, std::string, std::string>> authorKeys;
        for (const auto& dependency : surface.authorDependencies)
        {
            Require(dependency.root == "resource" || dependency.root == "project",
                "invalid author dependency root");
            Path(dependency.path);
            Require(dependency.mode == "bytes" || dependency.mode == "absent" ||
                dependency.mode == "terrain-json-without-package-path-v1" ||
                dependency.mode == "terrain-material-instance-static-json-v2" ||
                dependency.mode == "terrain-material-static-json-v2",
                "unsupported author dependency mode");
            Require(authorKeys.emplace(dependency.root, dependency.path, dependency.mode).second,
                "duplicate author dependency");
            const auto digest = ContentDigest::FromHex(dependency.digest);
            Require(digest.ToHex() == dependency.digest && digest != ContentDigest{},
                "invalid/noncanonical author digest");
            if (dependency.mode == "absent")
                Require(digest == ContentHasher::HashString("terrain-optional-absent-v1"),
                    "invalid absent author digest");
            Require(!dependency.productionObjectIds.empty() &&
                dependency.productionObjectIds.size() <= objects.size(),
                "author dependency requires production object references");
            std::set<std::string> referenced;
            for (const auto& id : dependency.productionObjectIds)
            {
                Require(referenced.insert(id).second, "duplicate author production object reference");
                Object(id);
            }
        }
        Path(surface.runtimeMaterialInstancePath);
        if (!surface.runtimeDetailMaterialInstancePath.empty())
            Path(surface.runtimeDetailMaterialInstancePath);
        ids.push_back(surface.surfaceId);
        Require(surface.sourceSet <= 255 && !surface.localToGlobal.empty() &&
            surface.localToGlobal.size() <= 256, "invalid source set/layer count");
        Placement(surface.localToWorld);
        const auto& sampling = surface.sampling;
        Require(sampling.coveragePageResolution == 128, "unsupported coverage page resolution");
        for (const auto* values : {&sampling.surfaceUvOffset, &sampling.surfaceUvScale,
            &sampling.coverageUvOffset, &sampling.coverageUvScale,
            &sampling.frameUvOffset, &sampling.frameUvScale})
            Finite(*values);
        for (const auto* scale : {&sampling.surfaceUvScale, &sampling.coverageUvScale, &sampling.frameUvScale})
            Require((*scale)[0] > 0 && (*scale)[1] > 0, "invalid sampling scale");
        const auto& parameters = surface.parameters;
        Require(std::isfinite(parameters.uvScale) && parameters.uvScale > 0 &&
            std::isfinite(parameters.heightBlendStrength) && parameters.heightBlendStrength >= 0 &&
            parameters.frozenHeightBlendFade == 1 && parameters.retainFragmentPostprocess,
            "unsupported static parameters");
        const auto& inputs = surface.inputs;
        for (const auto& id : Bindings(inputs))
            if (!id.empty())
                Require(Object(id).kind == TerrainCompositedProductionObjectKind::Image,
                    "non-image surface input");
        const auto image = [&](const std::string& id) -> const TerrainCompositedProductionImageDesc&
        {
            Require(!id.empty() && Object(id).kind == TerrainCompositedProductionObjectKind::Image,
                "missing required image input");
            return Object(id).image;
        };
        const auto& albedo = image(inputs.layerAlbedo);
        const auto& normal = image(inputs.layerNormal);
        const auto& orm = image(inputs.layerOrm);
        Require((albedo.format == "rgba8-srgb" || albedo.format == "bc7-srgb") &&
            albedo.transfer == TerrainCompositedProductionTransfer::Srgb,
            "layer albedo requires sRGB format and transfer");
        Require(albedo.layers == normal.layers && albedo.layers == orm.layers &&
            normal.transfer == TerrainCompositedProductionTransfer::Linear &&
            orm.transfer == TerrainCompositedProductionTransfer::Linear, "source array mismatch");
        const auto& catalog = image(inputs.layerCatalog);
        Require(catalog.format == "rgba32-float" && catalog.width == 256 &&
            catalog.height == 4 && catalog.layers == 1 && catalog.mipLevels == 1,
            "invalid layer catalog");
        Require(Object(inputs.layerCatalog).baseCatalogDigest != ContentDigest{} &&
            Object(inputs.layerCatalog).heightBlendCatalogDigest != ContentDigest{},
            "missing verified catalog projections");
        std::set<uint32_t> globals;
        for (const auto global : surface.localToGlobal)
            Require(global < 256 && globals.insert(global).second &&
                surface.globalToSourceSlice[global] < albedo.layers, "invalid local/global/source mapping");
        for (const auto slice : surface.globalToSourceSlice)
            Require(slice == UINT32_MAX || slice < albedo.layers, "source slice out of range");
        const auto& frameNormal = image(inputs.surfaceLocalNormal);
        const auto& frameTangent = image(inputs.surfaceLocalTangent);
        Require(frameNormal.format == "rgba32-float" && frameTangent.format == "rgba32-float" &&
            frameNormal.width == frameTangent.width && frameNormal.height == frameTangent.height &&
            frameNormal.width >= 2 && frameNormal.height >= 2 &&
            frameNormal.layers == 1 && frameTangent.layers == 1 &&
            frameNormal.mipLevels == 1 && frameTangent.mipLevels == 1, "invalid frame inputs");
        if (surface.weightSource == TerrainCompositedWeightSource::CoverageAtlas)
        {
            const auto& table = image(inputs.coveragePageTable);
            const auto& idsImage = image(inputs.coverageIds);
            const auto& weights = image(inputs.coverageWeights);
            Require(table.format == "rgba8-unorm" && table.width == table.height && table.width <= 32 &&
                idsImage.format == "rgba8-unorm" && weights.format == "rgba8-unorm" &&
                idsImage.width == table.width * 128 && idsImage.height == table.height * 128 &&
                weights.width == idsImage.width && weights.height == idsImage.height &&
                table.layers == 1 && idsImage.layers == 1 && weights.layers == 1 &&
                table.mipLevels == 1 && idsImage.mipLevels == 1 && weights.mipLevels == 1,
                "invalid coverage inputs");
        }
        else
        {
            Require(surface.weightSource == TerrainCompositedWeightSource::LocalLayerArray,
                "unknown weight mode");
            const auto& weights = image(inputs.localLayerWeights);
            Require(weights.format == "r32-float" && weights.width >= 4 &&
                weights.width == weights.height && weights.width <= 4096 &&
                weights.layers == surface.localToGlobal.size() && weights.mipLevels == 1 &&
                weights.filter == TerrainCompositedProductionFilter::Linear, "invalid frozen local weights");
        }
        if (parameters.heightBlendStrength > 0)
        {
            const auto& height = image(inputs.layerHeight);
            Require(height.format == "r16-unorm" && height.mipLevels >= 4 &&
                height.layers == albedo.layers, "missing height blend mip3");
        }
        Require(inputs.heightAtlas.empty() == inputs.heightLayerMap.empty(), "incomplete height closure");
        if (!inputs.heightAtlas.empty())
        {
            const auto& atlas = image(inputs.heightAtlas);
            const auto& mapping = image(inputs.heightLayerMap);
            Require(atlas.format == "r16-unorm" && atlas.layers == 1 && atlas.mipLevels == 1 &&
                mapping.format == "rgba32-float" && mapping.height == 1 &&
                mapping.layers == 1 && mapping.mipLevels == 1, "invalid height closure");
        }
    }
    std::sort(ids.begin(), ids.end());
    Require(std::adjacent_find(ids.begin(), ids.end()) == ids.end(), "duplicate surface identity");
    adjacency.Validate(ids);
}

ContentDigest TerrainCompositedProductionClosure::ContentHash() const
{
    Validate();
    CanonicalFieldHasher hash("terrain-composited-production-v2");
    hash.AddUInt32("generatorVersion", generator.version);
    for (const auto* value : {&generator.evaluator, &generator.filterRule, &generator.encoding,
        &generator.compilePolicy, &generator.computeSource})
        hash.AddString("generator", *value);
    for (auto values : {generator.includeSources, generator.compiledArtifacts})
    {
        std::sort(values.begin(), values.end());
        hash.AddUInt64("shaderCount", values.size());
        for (const auto& value : values)
            hash.AddString("shader", value);
    }
    std::vector<const TerrainCompositedProductionObject*> orderedObjects;
    for (const auto& object : objects)
        orderedObjects.push_back(&object);
    std::sort(orderedObjects.begin(), orderedObjects.end(), [](const auto* first, const auto* second)
        { return first->objectId < second->objectId; });
    hash.AddUInt64("objects", orderedObjects.size());
    for (const auto* object : orderedObjects)
    {
        hash.AddString("object", object->objectId);
        hash.AddUInt64("bytes", object->bytes);
        hash.AddDigest("digest", object->digest);
        hash.AddDigest("baseCatalog", object->baseCatalogDigest);
        hash.AddDigest("heightCatalog", object->heightBlendCatalogDigest);
        hash.AddUInt32("kind", uint32_t(object->kind));
        if (object->kind != TerrainCompositedProductionObjectKind::Image)
            continue;
        const auto& image = object->image;
        hash.AddString("format", image.format);
        hash.AddUInt32("width", image.width);
        hash.AddUInt32("height", image.height);
        hash.AddUInt32("layers", image.layers);
        hash.AddUInt32("mips", image.mipLevels);
        hash.AddUInt32("transfer", uint32_t(image.transfer));
        hash.AddUInt32("address", uint32_t(image.address));
        hash.AddUInt32("filter", uint32_t(image.filter));
    }
    std::vector<const TerrainCompositedSurfaceProduction*> orderedSurfaces;
    for (const auto& surface : surfaces)
        orderedSurfaces.push_back(&surface);
    std::sort(orderedSurfaces.begin(), orderedSurfaces.end(), [](const auto* first, const auto* second)
        { return first->surfaceId < second->surfaceId; });
    hash.AddUInt64("surfaces", orderedSurfaces.size());
    for (const auto* surface : orderedSurfaces)
    {
        hash.AddString("surface", surface->surfaceId);
        hash.AddString("runtimeMaterial", surface->runtimeMaterialInstancePath);
        hash.AddString("runtimeDetailMaterial", surface->runtimeDetailMaterialInstancePath);
        hash.AddUInt32("weightSource", uint32_t(surface->weightSource));
        hash.AddUInt32("sourceSet", surface->sourceSet);
        Numbers(hash, "placement", surface->localToWorld);
        hash.AddUInt64("localLayerCount", surface->localToGlobal.size());
        for (const auto global : surface->localToGlobal)
            hash.AddUInt32("localToGlobal", global);
        for (const auto slice : surface->globalToSourceSlice)
            hash.AddUInt32("globalToSlice", slice);
        for (const auto& binding : Bindings(surface->inputs))
            hash.AddString("binding", binding);
        hash.AddUInt32("coverageResolution", surface->sampling.coveragePageResolution);
        for (const auto* values : {&surface->sampling.surfaceUvOffset, &surface->sampling.surfaceUvScale,
            &surface->sampling.coverageUvOffset, &surface->sampling.coverageUvScale,
            &surface->sampling.frameUvOffset, &surface->sampling.frameUvScale})
            Numbers(hash, "sampling", *values);
        Number(hash, "uvScale", surface->parameters.uvScale);
        Number(hash, "heightBlend", surface->parameters.heightBlendStrength);
        Number(hash, "heightFade", surface->parameters.frozenHeightBlendFade);
        hash.AddBool("retainFragment", surface->parameters.retainFragmentPostprocess);
        auto dependencies = surface->authorDependencies;
        std::sort(dependencies.begin(), dependencies.end(), [](const auto& first, const auto& second) {
            return std::tie(first.root, first.path, first.mode) <
                std::tie(second.root, second.path, second.mode);
        });
        hash.AddUInt64("authorDependencyCount", dependencies.size());
        for (auto& dependency : dependencies)
        {
            hash.AddString("authorRoot", dependency.root);
            hash.AddString("authorPath", dependency.path);
            hash.AddString("authorMode", dependency.mode);
            hash.AddString("authorDigest", dependency.digest);
            std::sort(dependency.productionObjectIds.begin(), dependency.productionObjectIds.end());
            hash.AddUInt64("authorObjectCount", dependency.productionObjectIds.size());
            for (const auto& id : dependency.productionObjectIds)
                hash.AddString("authorObject", id);
        }
    }
    auto orderedLinks = adjacency.Links();
    const auto linkKey = [](const TerrainCompositedAdjacencyLink& link)
    {
        return std::make_tuple(link.sourceSurfaceId, link.sourceEdge, link.sourceEdgeInterval,
            link.targetSurfaceId, link.targetEdge, link.targetEdgeInterval, link.sourceUvToTargetUv);
    };
    std::sort(orderedLinks.begin(), orderedLinks.end(), [&](const auto& first, const auto& second)
        { return linkKey(first) < linkKey(second); });
    hash.AddUInt64("links", orderedLinks.size());
    for (const auto& link : orderedLinks)
    {
        hash.AddString("from", link.sourceSurfaceId);
        hash.AddString("to", link.targetSurfaceId);
        hash.AddUInt32("fromEdge", uint32_t(link.sourceEdge));
        hash.AddUInt32("toEdge", uint32_t(link.targetEdge));
        Numbers(hash, "fromInterval", link.sourceEdgeInterval);
        Numbers(hash, "toInterval", link.targetEdgeInterval);
        Numbers(hash, "uvTransform", link.sourceUvToTargetUv);
    }
    return hash.Finalize();
}

ContentDigest TerrainCompositedProductionClosure::SurfaceContentHash(const std::string& surfaceId) const
{
    Validate();
    Require(std::any_of(surfaces.begin(), surfaces.end(), [&](const auto& surface)
        { return surface.surfaceId == surfaceId; }), "unknown surface identity");
    auto base = *this;
    std::set<std::string> connected{surfaceId};
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (const auto& link : adjacency.Links())
            if (connected.count(link.sourceSurfaceId) != 0)
                changed = connected.insert(link.targetSurfaceId).second || changed;
    }
    base.surfaces.erase(std::remove_if(base.surfaces.begin(), base.surfaces.end(),
        [&](const auto& surface) { return connected.count(surface.surfaceId) == 0; }), base.surfaces.end());
    std::vector<TerrainCompositedAdjacencyLink> relevantLinks;
    for (const auto& link : adjacency.Links())
        if (connected.count(link.sourceSurfaceId) != 0)
            relevantLinks.push_back(link);
    base.adjacency = TerrainCompositedAdjacencyGraph(
        std::vector<std::string>(connected.begin(), connected.end()), std::move(relevantLinks));
    std::set<std::string> used{generator.computeSource};
    used.insert(generator.includeSources.begin(), generator.includeSources.end());
    used.insert(generator.compiledArtifacts.begin(), generator.compiledArtifacts.end());
    for (auto& surface : base.surfaces)
    {
        surface.authorDependencies.clear();
        surface.runtimeMaterialInstancePath = "runtime/base-authority.json";
        surface.runtimeDetailMaterialInstancePath.clear();
        surface.inputs.globalColour.clear();
        surface.inputs.globalMasks.clear();
        surface.inputs.heightAtlas.clear();
        surface.inputs.heightLayerMap.clear();
        if (surface.parameters.heightBlendStrength == 0)
            surface.inputs.layerHeight.clear();
        if (surface.weightSource == TerrainCompositedWeightSource::CoverageAtlas)
            surface.inputs.localLayerWeights.clear();
        else
        {
            surface.inputs.coveragePageTable.clear();
            surface.inputs.coverageIds.clear();
            surface.inputs.coverageWeights.clear();
        }
        for (const auto& binding : Bindings(surface.inputs))
            if (!binding.empty())
                used.insert(binding);
    }
    for (auto& object : base.objects)
    {
        bool catalog = false;
        bool heightBlend = false;
        for (const auto& surface : base.surfaces)
            if (surface.inputs.layerCatalog == object.objectId)
            {
                catalog = true;
                heightBlend = heightBlend || surface.parameters.heightBlendStrength > 0;
            }
        if (catalog)
        {
            object.digest = heightBlend ? object.heightBlendCatalogDigest : object.baseCatalogDigest;
            object.baseCatalogDigest = object.digest;
            object.heightBlendCatalogDigest = object.digest;
        }
    }
    base.objects.erase(std::remove_if(base.objects.begin(), base.objects.end(),
        [&](const auto& object) { return used.count(object.objectId) == 0; }), base.objects.end());
    CanonicalFieldHasher hash("terrain-composited-production-surface-v2");
    hash.AddDigest("production", base.ContentHash());
    hash.AddString("surface", surfaceId);
    return hash.Finalize();
}

namespace
{
using Json = nlohmann::json;

void Fields(const Json& value, std::initializer_list<const char*> names)
{
    Require(value.is_object() && value.size() == names.size(), "invalid production JSON fields");
    for (const auto* name : names)
        Require(value.contains(name), "missing production JSON field");
}

uint64_t Unsigned(const Json& value, uint64_t maximum = UINT32_MAX)
{
    Require(value.is_number_integer() && (value.is_number_unsigned() || value.get<int64_t>() >= 0),
        "unsigned production integer required");
    const auto result = value.get<uint64_t>();
    Require(result <= maximum, "production integer overflow");
    return result;
}

template<size_t Count>
std::array<double, Count> Array(const Json& value)
{
    Require(value.is_array() && value.size() == Count, "invalid numeric array");
    std::array<double, Count> result{};
    for (size_t index = 0; index < Count; ++index)
    {
        Require(value[index].is_number(), "numeric array contains non-number");
        result[index] = value[index].get<double>();
    }
    Finite(result);
    return result;
}

std::vector<std::string> Strings(const Json& value)
{
    Require(value.is_array() && value.size() <= 65536, "invalid string array");
    std::vector<std::string> result;
    for (const auto& entry : value)
    {
        Require(entry.is_string(), "string required");
        result.push_back(entry.get<std::string>());
        Text(result.back());
    }
    return result;
}
}

nlohmann::json TerrainCompositedProductionToJson(const TerrainCompositedProductionClosure& closure)
{
    closure.Validate();
    const auto& generator = closure.generator;
    Json document{{"generator", {{"version", generator.version}, {"evaluator", generator.evaluator},
        {"filterRule", generator.filterRule}, {"encoding", generator.encoding},
        {"compilePolicy", generator.compilePolicy}, {"computeSource", generator.computeSource},
        {"includeSources", generator.includeSources}, {"compiledArtifacts", generator.compiledArtifacts}}},
        {"objects", Json::array()}, {"surfaces", Json::array()}, {"adjacency", Json::array()},
        {"hash", closure.ContentHash().ToHex()}};
    for (const auto& object : closure.objects)
    {
        Json image = nullptr;
        if (object.kind == TerrainCompositedProductionObjectKind::Image)
            image = {{"format", object.image.format}, {"width", object.image.width},
                {"height", object.image.height}, {"layers", object.image.layers},
                {"mipLevels", object.image.mipLevels}, {"transfer", uint32_t(object.image.transfer)},
                {"address", uint32_t(object.image.address)}, {"filter", uint32_t(object.image.filter)}};
        document["objects"].push_back({{"objectId", object.objectId}, {"path", object.relativePath},
            {"bytes", object.bytes}, {"digest", object.digest.ToHex()},
            {"baseCatalogDigest", object.baseCatalogDigest.ToHex()},
            {"heightBlendCatalogDigest", object.heightBlendCatalogDigest.ToHex()}, {"kind", uint32_t(object.kind)},
            {"image", image}});
    }
    for (const auto& surface : closure.surfaces)
    {
        const auto& sampling = surface.sampling;
        Json dependencies = Json::array();
        for (const auto& dependency : surface.authorDependencies)
            dependencies.push_back({{"root", dependency.root}, {"path", dependency.path},
                {"mode", dependency.mode}, {"digest", dependency.digest},
                {"productionObjectIds", dependency.productionObjectIds}});
        document["surfaces"].push_back({{"surfaceId", surface.surfaceId},
            {"authorDependencies", std::move(dependencies)},
            {"runtimeMaterialInstancePath", surface.runtimeMaterialInstancePath},
            {"runtimeDetailMaterialInstancePath", surface.runtimeDetailMaterialInstancePath},
            {"weightSource", uint32_t(surface.weightSource)}, {"sourceSet", surface.sourceSet},
            {"localToWorld", surface.localToWorld}, {"localToGlobal", surface.localToGlobal},
            {"globalToSourceSlice", surface.globalToSourceSlice}, {"inputs", Bindings(surface.inputs)},
            {"sampling", {{"coveragePageResolution", sampling.coveragePageResolution},
                {"surfaceUvOffset", sampling.surfaceUvOffset}, {"surfaceUvScale", sampling.surfaceUvScale},
                {"coverageUvOffset", sampling.coverageUvOffset}, {"coverageUvScale", sampling.coverageUvScale},
                {"frameUvOffset", sampling.frameUvOffset}, {"frameUvScale", sampling.frameUvScale}}},
            {"parameters", {{"uvScale", surface.parameters.uvScale},
                {"heightBlendStrength", surface.parameters.heightBlendStrength},
                {"frozenHeightBlendFade", surface.parameters.frozenHeightBlendFade},
                {"retainFragmentPostprocess", surface.parameters.retainFragmentPostprocess}}}});
    }
    for (const auto& link : closure.adjacency.Links())
        document["adjacency"].push_back({{"sourceSurfaceId", link.sourceSurfaceId},
            {"targetSurfaceId", link.targetSurfaceId}, {"sourceEdge", uint32_t(link.sourceEdge)},
            {"targetEdge", uint32_t(link.targetEdge)}, {"sourceEdgeInterval", link.sourceEdgeInterval},
            {"targetEdgeInterval", link.targetEdgeInterval}, {"sourceUvToTargetUv", link.sourceUvToTargetUv}});
    return document;
}

TerrainCompositedProductionClosure TerrainCompositedProductionFromJson(const nlohmann::json& document)
{
    Fields(document, {"generator", "objects", "surfaces", "adjacency", "hash"});
    TerrainCompositedProductionClosure closure;
    const auto& generator = document.at("generator");
    Fields(generator, {"version", "evaluator", "filterRule", "encoding", "compilePolicy",
        "computeSource", "includeSources", "compiledArtifacts"});
    closure.generator.version = uint32_t(Unsigned(generator.at("version")));
    closure.generator.evaluator = generator.at("evaluator").get<std::string>();
    closure.generator.filterRule = generator.at("filterRule").get<std::string>();
    closure.generator.encoding = generator.at("encoding").get<std::string>();
    closure.generator.compilePolicy = generator.at("compilePolicy").get<std::string>();
    closure.generator.computeSource = generator.at("computeSource").get<std::string>();
    closure.generator.includeSources = Strings(generator.at("includeSources"));
    closure.generator.compiledArtifacts = Strings(generator.at("compiledArtifacts"));
    Require(document.at("objects").is_array() && document.at("objects").size() <= 65536 &&
        document.at("surfaces").is_array() && document.at("surfaces").size() <= 256 &&
        document.at("adjacency").is_array() && document.at("adjacency").size() <= 4096,
        "production JSON array limit exceeded");
    for (const auto& entry : document.at("objects"))
    {
        Fields(entry, {"objectId", "path", "bytes", "digest", "baseCatalogDigest",
            "heightBlendCatalogDigest", "kind", "image"});
        TerrainCompositedProductionObject object;
        object.objectId = entry.at("objectId").get<std::string>();
        object.relativePath = entry.at("path").get<std::string>();
        object.bytes = Unsigned(entry.at("bytes"), UINT64_MAX);
        const auto digest = entry.at("digest").get<std::string>();
        object.digest = ContentDigest::FromHex(digest);
        Require(object.digest.ToHex() == digest, "noncanonical input digest");
        const auto baseDigest = entry.at("baseCatalogDigest").get<std::string>();
        const auto heightDigest = entry.at("heightBlendCatalogDigest").get<std::string>();
        object.baseCatalogDigest = ContentDigest::FromHex(baseDigest);
        object.heightBlendCatalogDigest = ContentDigest::FromHex(heightDigest);
        Require(object.baseCatalogDigest.ToHex() == baseDigest &&
            object.heightBlendCatalogDigest.ToHex() == heightDigest, "noncanonical projection digest");
        object.kind = TerrainCompositedProductionObjectKind(Unsigned(entry.at("kind"), 3));
        const auto& image = entry.at("image");
        if (object.kind == TerrainCompositedProductionObjectKind::Image)
        {
            Fields(image, {"format", "width", "height", "layers", "mipLevels", "transfer", "address", "filter"});
            object.image.format = image.at("format").get<std::string>();
            object.image.width = uint32_t(Unsigned(image.at("width")));
            object.image.height = uint32_t(Unsigned(image.at("height")));
            object.image.layers = uint32_t(Unsigned(image.at("layers")));
            object.image.mipLevels = uint32_t(Unsigned(image.at("mipLevels")));
            object.image.transfer = TerrainCompositedProductionTransfer(Unsigned(image.at("transfer"), 1));
            object.image.address = TerrainCompositedProductionAddress(Unsigned(image.at("address"), 1));
            object.image.filter = TerrainCompositedProductionFilter(Unsigned(image.at("filter"), 1));
        }
        else
            Require(image.is_null(), "non-image object must not carry image metadata");
        closure.objects.push_back(std::move(object));
    }
    std::vector<std::string> ids;
    for (const auto& entry : document.at("surfaces"))
    {
        Fields(entry, {"surfaceId", "runtimeMaterialInstancePath", "runtimeDetailMaterialInstancePath", "weightSource", "sourceSet",
            "localToWorld", "localToGlobal", "globalToSourceSlice", "inputs", "sampling", "parameters",
            "authorDependencies"});
        TerrainCompositedSurfaceProduction surface;
        surface.surfaceId = entry.at("surfaceId").get<std::string>();
        const auto& dependencies = entry.at("authorDependencies");
        Require(dependencies.is_array() && dependencies.size() <= 65536,
            "invalid author dependency array");
        for (const auto& value : dependencies)
        {
            Fields(value, {"root", "path", "mode", "digest", "productionObjectIds"});
            surface.authorDependencies.push_back({
                value.at("root").get<std::string>(), value.at("path").get<std::string>(),
                value.at("mode").get<std::string>(), value.at("digest").get<std::string>(),
                Strings(value.at("productionObjectIds"))});
        }
        surface.runtimeMaterialInstancePath = entry.at("runtimeMaterialInstancePath").get<std::string>();
        surface.runtimeDetailMaterialInstancePath = entry.at("runtimeDetailMaterialInstancePath").get<std::string>();
        ids.push_back(surface.surfaceId);
        surface.weightSource = TerrainCompositedWeightSource(Unsigned(entry.at("weightSource"), 1));
        surface.sourceSet = uint32_t(Unsigned(entry.at("sourceSet"), 255));
        surface.localToWorld = Array<16>(entry.at("localToWorld"));
        const auto& local = entry.at("localToGlobal");
        const auto& global = entry.at("globalToSourceSlice");
        Require(local.is_array() && local.size() <= 256 && global.is_array() && global.size() == 256,
            "invalid layer mapping arrays");
        for (const auto& value : local)
            surface.localToGlobal.push_back(uint32_t(Unsigned(value, 255)));
        for (size_t index = 0; index < 256; ++index)
            surface.globalToSourceSlice[index] = uint32_t(Unsigned(global[index]));
        const auto& inputs = entry.at("inputs");
        Require(inputs.is_array() && inputs.size() == 15, "invalid input binding count");
        auto& bindings = surface.inputs;
        std::array<std::string*, 15> targets{&bindings.coveragePageTable, &bindings.coverageIds,
            &bindings.coverageWeights, &bindings.localLayerWeights, &bindings.layerCatalog,
            &bindings.layerAlbedo, &bindings.layerNormal, &bindings.layerOrm, &bindings.layerHeight,
            &bindings.surfaceLocalNormal, &bindings.surfaceLocalTangent, &bindings.heightAtlas,
            &bindings.heightLayerMap, &bindings.globalColour, &bindings.globalMasks};
        for (size_t index = 0; index < targets.size(); ++index)
            *targets[index] = inputs[index].get<std::string>();
        const auto& sampling = entry.at("sampling");
        Fields(sampling, {"coveragePageResolution", "surfaceUvOffset", "surfaceUvScale",
            "coverageUvOffset", "coverageUvScale", "frameUvOffset", "frameUvScale"});
        surface.sampling.coveragePageResolution = uint32_t(Unsigned(sampling.at("coveragePageResolution")));
        surface.sampling.surfaceUvOffset = Array<2>(sampling.at("surfaceUvOffset"));
        surface.sampling.surfaceUvScale = Array<2>(sampling.at("surfaceUvScale"));
        surface.sampling.coverageUvOffset = Array<2>(sampling.at("coverageUvOffset"));
        surface.sampling.coverageUvScale = Array<2>(sampling.at("coverageUvScale"));
        surface.sampling.frameUvOffset = Array<2>(sampling.at("frameUvOffset"));
        surface.sampling.frameUvScale = Array<2>(sampling.at("frameUvScale"));
        const auto& parameters = entry.at("parameters");
        Fields(parameters, {"uvScale", "heightBlendStrength", "frozenHeightBlendFade", "retainFragmentPostprocess"});
        for (const auto* field : {"uvScale", "heightBlendStrength", "frozenHeightBlendFade"})
            Require(parameters.at(field).is_number(), "parameter must be a number");
        surface.parameters.uvScale = parameters.at("uvScale").get<double>();
        surface.parameters.heightBlendStrength = parameters.at("heightBlendStrength").get<double>();
        surface.parameters.frozenHeightBlendFade = parameters.at("frozenHeightBlendFade").get<double>();
        Require(parameters.at("retainFragmentPostprocess").is_boolean(), "parameter must be boolean");
        surface.parameters.retainFragmentPostprocess = parameters.at("retainFragmentPostprocess").get<bool>();
        closure.surfaces.push_back(std::move(surface));
    }
    std::vector<TerrainCompositedAdjacencyLink> links;
    for (const auto& entry : document.at("adjacency"))
    {
        Fields(entry, {"sourceSurfaceId", "targetSurfaceId", "sourceEdge", "targetEdge",
            "sourceEdgeInterval", "targetEdgeInterval", "sourceUvToTargetUv"});
        TerrainCompositedAdjacencyLink link;
        link.sourceSurfaceId = entry.at("sourceSurfaceId").get<std::string>();
        link.targetSurfaceId = entry.at("targetSurfaceId").get<std::string>();
        link.sourceEdge = TerrainCompositedSurfaceEdge(Unsigned(entry.at("sourceEdge"), 3));
        link.targetEdge = TerrainCompositedSurfaceEdge(Unsigned(entry.at("targetEdge"), 3));
        link.sourceEdgeInterval = Array<2>(entry.at("sourceEdgeInterval"));
        link.targetEdgeInterval = Array<2>(entry.at("targetEdgeInterval"));
        link.sourceUvToTargetUv = Array<6>(entry.at("sourceUvToTargetUv"));
        links.push_back(std::move(link));
    }
    closure.adjacency = TerrainCompositedAdjacencyGraph(ids, std::move(links));
    closure.Validate();
    Require(document.at("hash") == closure.ContentHash().ToHex(), "production identity mismatch");
    return closure;
}

void PopulateTerrainCompositedCatalogDigests(
    TerrainCompositedProductionObject& object, const std::vector<uint8_t>& bytes)
{
    Require(object.kind == TerrainCompositedProductionObjectKind::Image &&
        object.image.format == "rgba32-float" && object.image.width == 256 &&
        object.image.height == 4 && object.image.layers == 1 && object.image.mipLevels == 1 &&
        bytes.size() == object.image.ByteSize() && object.bytes == bytes.size() &&
        ContentHasher::HashBytes(bytes.data(), bytes.size()) == object.digest,
        "catalog projection requires verified raw bytes");
    CanonicalFieldHasher base("terrain-catalog-base-uv-v2");
    CanonicalFieldHasher height("terrain-catalog-height-uv-v2");
    for (size_t offset = 0; offset < bytes.size(); offset += sizeof(float))
    {
        float value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(float));
        Require(std::isfinite(value), "non-finite catalog payload");
    }
    for (uint32_t layer = 0; layer < 256; ++layer)
    {
        for (uint32_t channel : {2u, 3u})
        {
            const size_t offset = ((256 + layer) * 4 + channel) * 4;
            base.AddBytes("uv", bytes.data() + offset, 4);
            height.AddBytes("uv", bytes.data() + offset, 4);
        }
        height.AddBytes("displacement", bytes.data() + ((3 * 256 + layer) * 4 + 3) * 4, 4);
    }
    object.baseCatalogDigest = base.Finalize();
    object.heightBlendCatalogDigest = height.Finalize();
}

void ValidateTerrainCompositedProductionPayload(
    const TerrainCompositedProductionObject& object, const std::vector<uint8_t>& bytes)
{
    Require(bytes.size() == object.bytes &&
        ContentHasher::HashBytes(bytes.data(), bytes.size()) == object.digest,
        "production payload integrity mismatch");
    if (object.baseCatalogDigest != ContentDigest{} || object.heightBlendCatalogDigest != ContentDigest{})
    {
        auto actual = object;
        PopulateTerrainCompositedCatalogDigests(actual, bytes);
        Require(actual.baseCatalogDigest == object.baseCatalogDigest &&
            actual.heightBlendCatalogDigest == object.heightBlendCatalogDigest,
            "catalog projection does not match actual bytes");
    }
}

}
