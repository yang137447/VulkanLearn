#include "render/renderable/tressfxRenderable.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "renderableObject.h"
#include "render/renderable/renderableMeshBuffers.h"
#include "render/backend/rendererBackendVulkan.h"
#include "mesh/loader/common/meshAssetLoader.h"
#include "hair/tressfxGroupGpuData.h"
#include "hair/tressfxGroupSimulation.h"
#include "hair/tressfxSdfGrid.h"

void RenderableObject::SetTressfxSkinningBinding(
    const VL::Hair::TressfxSkinningBinding& binding)
{
    tressfxSkinningBinding = binding;
    if (!tressfxBoneWeightsBuffer || tressfxGuideCount == 0)
        return;
    if (!binding.strandBones.empty() &&
        binding.strandBones.size() != tressfxGuideCount)
        throw std::runtime_error(
            "TressFX .tfxbone strand count does not match guide count for \"" + name + "\"");

    std::vector<VL::Hair::TressfxBoneWeightGpu> gpu(tressfxGuideCount);
    // 裁剪/归一化/回退根的语义只在 tressfxSkinning.h 一处实现：.tfxbone 与
    // 网格派生两条来源共用，模块测试也直接测这份纯函数。
    const VL::Hair::TressfxStrandBoneWeights emptyWeights;
    for (size_t strand = 0; strand < gpu.size(); ++strand)
        gpu[strand] = VL::Hair::TressfxPackStrandBoneWeights(
            binding.strandBones.empty() ? emptyWeights : binding.strandBones[strand],
            binding.jointIndex);
    const vk::DeviceSize bytes =
        static_cast<vk::DeviceSize>(gpu.size()) * sizeof(VL::Hair::TressfxBoneWeightGpu);
    void* mapped = rendererBackend->MapMemory(tressfxBoneWeightsMemory, bytes);
    std::memcpy(mapped, gpu.data(), static_cast<size_t>(bytes));
    rendererBackend->UnmapMemory(tressfxBoneWeightsMemory);
}

void RenderableObject::CreateTressfxPositionBuffer()
{
    if (!IsTressfx())
        return;

    const auto& section = imported->modelResource.sections[importedSectionIndex];
    std::vector<Eigen::Vector3f> staticPositions;
    const std::vector<Eigen::Vector3f>* positions = nullptr;
    if (tressfxSimulation)
    {
        positions = &tressfxSimulation->Positions();
    }
    else
    {
        staticPositions.reserve(section.vertices.size() / 2);
        for (size_t point = 0; point < section.vertices.size() / 2; ++point)
            staticPositions.push_back(section.vertices[point * 2].position);
        positions = &staticPositions;
    }
    std::vector<Eigen::Vector4f> initialPositions;
    initialPositions.reserve(positions->size());
    for (const Eigen::Vector3f& position : *positions)
        initialPositions.emplace_back(position.x(), position.y(), position.z(), 1.0f);

    const vk::DeviceSize size =
        static_cast<vk::DeviceSize>(initialPositions.size()) * sizeof(Eigen::Vector4f);
    // GPU backend 的 current 只由 compute 写、vertex shader 读，创建后放在 device-local；
    // CPU backend 仍由游戏线程逐帧写 host-coherent buffer，保持既有同步策略。
    const bool gpuSimulation = (tressfxSimulationConfig.enabled ||
            tressfxSimulationConfig.skinning.has_value()) &&
        tressfxSimulationConfig.backend == TressfxSimulationBackend::Gpu &&
        tressfxGuideCount != 0;
    if (gpuSimulation)
    {
        std::tie(tressfxPositionBuffer, tressfxPositionMemory) =
            VL::RenderableDetail::CreateUploadedDeviceLocalStorageBuffer(
                *rendererBackend, initialPositions.data(), size,
                "TressFXPositions: " + name);
    }
    else
    {
        std::tie(tressfxPositionBuffer, tressfxPositionMemory) = rendererBackend->CreateBuffer(
            size,
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            "TressFXPositions: " + name);
        try
        {
            void* mapped = rendererBackend->MapMemory(tressfxPositionMemory, size);
            std::memcpy(mapped, initialPositions.data(), static_cast<size_t>(size));
            rendererBackend->UnmapMemory(tressfxPositionMemory);
        }
        catch (...)
        {
            rendererBackend->DestroyBuffer(tressfxPositionBuffer, tressfxPositionMemory);
            tressfxPositionBuffer = nullptr;
            tressfxPositionMemory = nullptr;
            throw;
        }
    }
    tressfxPositionInfo
        .setBuffer(tressfxPositionBuffer)
        .setOffset(0)
        .setRange(size);
}

void RenderableObject::CreateTressfxGuideGroupBuffers()
{
    if (!IsTressfx() || tressfxGuideCount == 0)
        return;

    // 组数据来自独立 binding，不读导入结果里的旧 descriptor；组编辑后
    // UpdateTressfxGroupBinding 只重写这两份 buffer。
    const std::vector<uint32_t> emptyIds;
    const std::vector<std::string> emptyNames;
    const std::vector<uint32_t>& guideGroupIds = tressfxGuideGroupBinding
        ? tressfxGuideGroupBinding->guideGroupIds : emptyIds;
    const std::vector<std::string>& groupNames = tressfxGuideGroupBinding
        ? tressfxGuideGroupBinding->groupNames : emptyNames;
    const VL::Hair::TressfxGroupGpuData data = VL::Hair::BuildTressfxGroupGpuData(
        tressfxGuideCount, guideGroupIds, groupNames,
        VL::Hair::TressfxGroupPreviewColorCapacity);

    // group id 只是作者分组数据，不随模拟更新；host-visible coherent 让对象创建后
    // 不再需要 staging/一次性上传。颜色表同样静态，但只读一次上传即可。
    const vk::DeviceSize idBytes = static_cast<vk::DeviceSize>(data.guideGroupIds.size()) *
        sizeof(uint32_t);
    std::tie(tressfxGuideGroupIdBuffer, tressfxGuideGroupIdMemory) =
        rendererBackend->CreateBuffer(
            idBytes,
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "TressFXGuideGroupIds: " + name);
    try
    {
        void* mapped = rendererBackend->MapMemory(tressfxGuideGroupIdMemory, idBytes);
        std::memcpy(mapped, data.guideGroupIds.data(), static_cast<size_t>(idBytes));
        rendererBackend->UnmapMemory(tressfxGuideGroupIdMemory);
    }
    catch (...)
    {
        rendererBackend->DestroyBuffer(tressfxGuideGroupIdBuffer, tressfxGuideGroupIdMemory);
        tressfxGuideGroupIdBuffer = nullptr;
        tressfxGuideGroupIdMemory = nullptr;
        throw;
    }
    tressfxGuideGroupIdInfo
        .setBuffer(tressfxGuideGroupIdBuffer)
        .setOffset(0)
        .setRange(idBytes);

    // descriptor 捕获固定容量；组数变化不再重建 color buffer。
    const vk::DeviceSize colorBytes =
        static_cast<vk::DeviceSize>(VL::Hair::TressfxGroupPreviewColorCapacity) *
        sizeof(VL::Hair::TressfxGroupColorGpu);
    std::tie(tressfxGuideGroupColorBuffer, tressfxGuideGroupColorMemory) =
        rendererBackend->CreateBuffer(
            colorBytes,
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "TressFXGuideGroupColors: " + name);
    try
    {
        void* mapped = rendererBackend->MapMemory(tressfxGuideGroupColorMemory, colorBytes);
        std::memcpy(mapped, data.groupColors.data(), static_cast<size_t>(colorBytes));
        rendererBackend->UnmapMemory(tressfxGuideGroupColorMemory);
    }
    catch (...)
    {
        rendererBackend->DestroyBuffer(tressfxGuideGroupColorBuffer, tressfxGuideGroupColorMemory);
        tressfxGuideGroupColorBuffer = nullptr;
        tressfxGuideGroupColorMemory = nullptr;
        throw;
    }
    tressfxGuideGroupColorInfo
        .setBuffer(tressfxGuideGroupColorBuffer)
        .setOffset(0)
        .setRange(colorBytes);
    tressfxGuideGroupColorCount =
        static_cast<uint32_t>(groupNames.size()) + 1u;
}

void RenderableObject::CreateTressfxBrushWeightBuffer()
{
    if (!IsTressfx() || tressfxGuideCount == 0) return;
    const vk::DeviceSize bytes = static_cast<vk::DeviceSize>(tressfxGuideCount) *
        sizeof(float);
    const uint32_t slots = std::max(1u, rendererBackend->GetSwapchainImageCount());
    tressfxBrushWeightBuffers.assign(slots, nullptr);
    tressfxBrushWeightMemories.assign(slots, nullptr);
    tressfxBrushWeightInfos.assign(slots, {});
    for (uint32_t slot = 0; slot < slots; ++slot)
    {
        std::tie(tressfxBrushWeightBuffers[slot], tressfxBrushWeightMemories[slot]) =
            rendererBackend->CreateBuffer(
                bytes,
                vk::BufferUsageFlagBits::eStorageBuffer,
                vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent,
                "TressFXBrushWeights[" + std::to_string(slot) + "]: " + name);
        try
        {
            // 初值全零；UI 的第一次笔刷命令会整表覆盖，不做增量累加。
            void* mapped = rendererBackend->MapMemory(tressfxBrushWeightMemories[slot], bytes);
            std::memset(mapped, 0, static_cast<size_t>(bytes));
            rendererBackend->UnmapMemory(tressfxBrushWeightMemories[slot]);
        }
        catch (...)
        {
            rendererBackend->DestroyBuffer(
                tressfxBrushWeightBuffers[slot], tressfxBrushWeightMemories[slot]);
            tressfxBrushWeightBuffers[slot] = nullptr;
            tressfxBrushWeightMemories[slot] = nullptr;
            throw;
        }
        tressfxBrushWeightInfos[slot]
            .setBuffer(tressfxBrushWeightBuffers[slot])
            .setOffset(0)
            .setRange(bytes);
    }
}

void RenderableObject::CreateTressfxGpuSimulationBuffers()
{
    if (!IsTressfx() || tressfxSimulation)
        return;
    const auto& section = imported->modelResource.sections[importedSectionIndex];
    if ((!tressfxSimulationConfig.enabled && !tressfxSimulationConfig.skinning) ||
        tressfxSimulationConfig.backend != TressfxSimulationBackend::Gpu ||
        tressfxGuideCount == 0 || tressfxPointsPerGuide < 2)
        return;

    const size_t pointCount = section.tressfxInverseMass.size();
    const vk::DeviceSize positionBytes =
        static_cast<vk::DeviceSize>(pointCount) * sizeof(Eigen::Vector4f);

    // previous positions：初值与 current 一样来自 rest；w 不用作逆质量。
    std::vector<Eigen::Vector4f> initialPoints(pointCount);
    for (size_t point = 0; point < pointCount; ++point)
    {
        const Eigen::Vector3f& rest = section.vertices[point * 2].position;
        initialPoints[point] = Eigen::Vector4f(rest.x(), rest.y(), rest.z(), 1.0f);
    }
    if (tressfxSimulationConfig.enabled)
    {
    // 每组配置按 frame-in-flight 分槽：正常帧内容是静态的，实时参数覆盖
    // 只在当前槽 fence 完成后重写，不与在飞 compute 共享同一份可写 buffer。
    const std::vector<VL::Hair::TressfxGroupSimulationGpu> groupGpuData =
        VL::Hair::BuildTressfxGroupSimulationGpuData(tressfxGroupSimulationConfigs,
            VL::Hair::TressfxGroupPreviewColorCapacity);
    const vk::DeviceSize groupConfigBytes =
        static_cast<vk::DeviceSize>(VL::Hair::TressfxGroupPreviewColorCapacity) *
        sizeof(VL::Hair::TressfxGroupSimulationGpu);
    std::array<VL::Hair::TressfxGroupSimulationGpu, 1> emptyGroupConfig{};
    const void* groupConfigSource = groupGpuData.empty()
        ? static_cast<const void*>(emptyGroupConfig.data())
        : static_cast<const void*>(groupGpuData.data());
    for (uint32_t slot = 0; slot < VL::Hair::GuideSimulationParamBufferCount; ++slot)
    {
        auto& target = tressfxGuideGroupConfigTargets[slot];
        std::tie(target.buffer, target.memory) = rendererBackend->CreateBuffer(
            groupConfigBytes,
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "TressFXGroupSimulationConfigs: " + name + ":" + std::to_string(slot));
        target.mapped = rendererBackend->MapMemory(target.memory, groupConfigBytes);
        if (groupGpuData.empty())
            std::memset(target.mapped, 0, static_cast<size_t>(groupConfigBytes));
        else
            std::memcpy(target.mapped, groupConfigSource,
                static_cast<size_t>(groupConfigBytes));
        target.info.setBuffer(target.buffer).setOffset(0).setRange(groupConfigBytes);
    }

    std::tie(tressfxPreviousPositionBuffer, tressfxPreviousPositionMemory) =
        VL::RenderableDetail::CreateUploadedDeviceLocalStorageBuffer(
            *rendererBackend, initialPoints.data(), positionBytes,
            "TressFXPreviousPositions: " + name);
    tressfxPreviousPositionInfo
        .setBuffer(tressfxPreviousPositionBuffer).setOffset(0).setRange(positionBytes);

    // previous-previous positions：strand-level 伪加速度与第二阶 Verlet 状态。
    // 初值与 previous 相同，首帧的伪加速度因此为 0。
    std::tie(tressfxPreviousPreviousPositionBuffer,
        tressfxPreviousPreviousPositionMemory) =
        VL::RenderableDetail::CreateUploadedDeviceLocalStorageBuffer(
            *rendererBackend, initialPoints.data(), positionBytes,
            "TressFXPreviousPreviousPositions: " + name);
    tressfxPreviousPreviousPositionInfo
        .setBuffer(tressfxPreviousPreviousPositionBuffer).setOffset(0)
        .setRange(positionBytes);
    }

    // rest positions：xyz 是静止中心线，w 是导入的逆质量；integration pass 用它判断
    // 不可移动点（根前缀或逆质量 <= 0）。
    for (size_t point = 0; point < pointCount; ++point)
    {
        const Eigen::Vector3f& rest = section.vertices[point * 2].position;
        initialPoints[point] = Eigen::Vector4f(rest.x(), rest.y(), rest.z(),
            section.tressfxInverseMass[point]);
    }
    std::tie(tressfxRestPositionBuffer, tressfxRestPositionMemory) =
        VL::RenderableDetail::CreateUploadedDeviceLocalStorageBuffer(
            *rendererBackend, initialPoints.data(), positionBytes,
            "TressFXRestPositions: " + name);
    tressfxRestPositionInfo
        .setBuffer(tressfxRestPositionBuffer).setOffset(0).setRange(positionBytes);

    // 静止段长：第 i 项是 i -> i+1 的 rest 长度，末项 0。LengthConstraintsWind pass
    // 每根 strand 读取，创建时上传一次。
    if (tressfxSimulationConfig.enabled)
    {
    std::vector<float> restLengths(pointCount, 0.0f);
    for (size_t guide = 0; guide < tressfxGuideCount; ++guide)
    {
        const size_t base = guide * tressfxPointsPerGuide;
        for (size_t local = 0; local + 1 < tressfxPointsPerGuide; ++local)
            restLengths[base + local] = (section.vertices[(base + local + 1) * 2].position -
                section.vertices[(base + local) * 2].position).norm();
    }
    const vk::DeviceSize restLengthBytes =
        static_cast<vk::DeviceSize>(pointCount) * sizeof(float);
    std::tie(tressfxRestLengthBuffer, tressfxRestLengthMemory) =
        VL::RenderableDetail::CreateUploadedDeviceLocalStorageBuffer(
            *rendererBackend, restLengths.data(), restLengthBytes,
            "TressFXRestLengths: " + name);
    tressfxRestLengthInfo
        .setBuffer(tressfxRestLengthBuffer).setOffset(0).setRange(restLengthBytes);

    // strand-level 数据：每根 strand 3 个 vec4（VspQuat、VspTranslation + VspCoeff、
    // SkinningQuat），由 StrandLevel pass 每帧写、VSP/LocalShape pass 读。
    std::vector<Eigen::Vector4f> strandLevelData(
        static_cast<size_t>(tressfxGuideCount) * 3u, Eigen::Vector4f::Zero());
    const vk::DeviceSize strandLevelBytes =
        static_cast<vk::DeviceSize>(strandLevelData.size()) * sizeof(Eigen::Vector4f);
    std::tie(tressfxStrandLevelBuffer, tressfxStrandLevelMemory) =
        VL::RenderableDetail::CreateUploadedDeviceLocalStorageBuffer(
            *rendererBackend, strandLevelData.data(), strandLevelBytes,
            "TressFXStrandLevel: " + name);
    tressfxStrandLevelInfo
        .setBuffer(tressfxStrandLevelBuffer).setOffset(0).setRange(strandLevelBytes);
    }

    // params UBO 每帧由主机改写，保持 host-coherent；只有位置 buffer 是 device-local。
    for (uint32_t slot = 0; slot < VL::Hair::GuideSimulationParamBufferCount; ++slot)
    {
        auto& target = tressfxParamsTargets[slot];
        std::tie(target.buffer, target.memory) = rendererBackend->CreateBuffer(
            sizeof(VL::Hair::GuideSimulationParams),
            vk::BufferUsageFlagBits::eUniformBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            "TressFXSimulationParams: " + name + ":" + std::to_string(slot));
        target.mapped = rendererBackend->MapMemory(
            target.memory, sizeof(VL::Hair::GuideSimulationParams));
        target.info.setBuffer(target.buffer).setOffset(0)
            .setRange(sizeof(VL::Hair::GuideSimulationParams));
        // reset 首轮 Integration 的独立 UBO：内容只在 reset/换基帧写入，
        // 但句柄常驻，descriptor set 在对象创建时一次性绑定。
        if (tressfxSimulationConfig.enabled)
        {
        auto& resetTarget = tressfxResetParamsTargets[slot];
        std::tie(resetTarget.buffer, resetTarget.memory) = rendererBackend->CreateBuffer(
            sizeof(VL::Hair::GuideSimulationParams),
            vk::BufferUsageFlagBits::eUniformBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            "TressFXResetParams: " + name + ":" + std::to_string(slot));
        resetTarget.mapped = rendererBackend->MapMemory(
            resetTarget.memory, sizeof(VL::Hair::GuideSimulationParams));
        resetTarget.info.setBuffer(resetTarget.buffer).setOffset(0)
            .setRange(sizeof(VL::Hair::GuideSimulationParams));
        }
    }
    // 多骨：每 strand 4 个 (joint, weight) 槽，共 32 bytes；没有 .tfxbone 时
    // 全零，shader 用 motion.w bit2 判断是否读取。palette 每槽固定 256 mat4。
    constexpr uint32_t MaxPaletteJoints = 256;
    const vk::DeviceSize boneWeightBytes =
        static_cast<vk::DeviceSize>(tressfxGuideCount) *
        sizeof(VL::Hair::TressfxBoneWeightGpu);
    std::tie(tressfxBoneWeightsBuffer, tressfxBoneWeightsMemory) =
        rendererBackend->CreateBuffer(
            std::max<vk::DeviceSize>(boneWeightBytes,
                sizeof(VL::Hair::TressfxBoneWeightGpu)),
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "TressFXBoneWeights: " + name);
    {
        const vk::DeviceSize bytes = std::max<vk::DeviceSize>(boneWeightBytes,
            sizeof(VL::Hair::TressfxBoneWeightGpu));
        void* mapped = rendererBackend->MapMemory(tressfxBoneWeightsMemory, bytes);
        std::memset(mapped, 0, static_cast<size_t>(bytes));
        rendererBackend->UnmapMemory(tressfxBoneWeightsMemory);
    }
    tressfxBoneWeightsInfo.setBuffer(tressfxBoneWeightsBuffer).setOffset(0)
        .setRange(std::max<vk::DeviceSize>(boneWeightBytes,
            sizeof(VL::Hair::TressfxBoneWeightGpu)));
    tressfxPaletteJointCapacity = MaxPaletteJoints;
    for (uint32_t slot = 0; slot < VL::Hair::GuideSimulationParamBufferCount; ++slot)
    {
        auto& target = tressfxPaletteTargets[slot];
        const vk::DeviceSize bytes =
            static_cast<vk::DeviceSize>(MaxPaletteJoints) * sizeof(Eigen::Matrix4f);
        std::tie(target.buffer, target.memory) = rendererBackend->CreateBuffer(
            bytes, vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "TressFXSkinningPalette: " + name + ":" + std::to_string(slot));
        target.mapped = rendererBackend->MapMemory(target.memory, bytes);
        target.info.setBuffer(target.buffer).setOffset(0).setRange(bytes);
    }
    // 每帧碰撞前的最大穿透深度（uint 位模式；0=无穿透）。host 清零、shader
    // atomicMax、trace 通过 transfer 拷到 readback，不参与渲染。
    // off 蒙皮只需要 current/rest/params/weights/palette，不持有求解器状态。
    if (!tressfxSimulationConfig.enabled)
        return;
    for (uint32_t slot = 0; slot < VL::Hair::GuideSimulationParamBufferCount; ++slot)
    {
        auto& target = tressfxPenetrationStatsTargets[slot];
        constexpr vk::DeviceSize bytes = 2 * sizeof(uint32_t);
        std::tie(target.buffer, target.memory) = rendererBackend->CreateBuffer(
            bytes, vk::BufferUsageFlagBits::eStorageBuffer |
                vk::BufferUsageFlagBits::eTransferSrc,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "TressFXPenetrationStats: " + name + ":" + std::to_string(slot));
        target.mapped = rendererBackend->MapMemory(target.memory, bytes);
        std::memset(target.mapped, 0, static_cast<size_t>(bytes));
        target.info.setBuffer(target.buffer).setOffset(0).setRange(bytes);
    }
    // 解析胶囊：每个 frame-in-flight 槽一份 host-coherent SSBO。声明了 bone 的
    // 胶囊每帧由 HairSimulationPass 重写；无 bone 的胶囊内容保持创建时的作者值。
    // 空列表也保留合法 buffer，因为 shader 的 binding 7 是静态声明，不能用空
    // descriptor；count 由 params.counts.w 控制。
    const vk::DeviceSize colliderBytes =
        sizeof(VL::Hair::GuideColliderGpu) * VL::Hair::TressfxMaxColliders;
    std::array<VL::Hair::GuideColliderGpu, VL::Hair::TressfxMaxColliders> gpuColliders{};
    const auto& capsules = tressfxSimulationConfig.capsules;
    if (capsules.size() > VL::Hair::TressfxMaxColliders)
        throw std::runtime_error("TressFX collider count exceeds the GPU limit: " + name);
    for (size_t collider = 0; collider < capsules.size(); ++collider)
        gpuColliders[collider] = VL::Hair::MakeGuideColliderGpu(capsules[collider]);
    for (uint32_t slot = 0; slot < VL::Hair::GuideSimulationParamBufferCount; ++slot)
    {
        auto& target = tressfxColliderTargets[slot];
        std::tie(target.buffer, target.memory) = rendererBackend->CreateBuffer(
            colliderBytes,
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            "TressFXColliders: " + name + ":" + std::to_string(slot));
        target.mapped = rendererBackend->MapMemory(target.memory, colliderBytes);
        std::memcpy(target.mapped, gpuColliders.data(), static_cast<size_t>(colliderBytes));
        target.info.setBuffer(target.buffer).setOffset(0).setRange(colliderBytes);
    }

    // `collision.mesh` 的距离场资源：只在声明了代理时创建；失败让整个
    // renderable 构造失败（构造函数的 catch 会回收已建 buffer）。
    CreateTressfxSdfBuffers();

    tressfxGpuSimulation = true;
}

void RenderableObject::CreateTressfxSdfBuffers()
{
    const auto& section = imported->modelResource.sections[importedSectionIndex];
    if (!tressfxSimulationConfig.collisionMesh)
        return;
    const TressfxCollisionMeshConfig& config =
        *tressfxSimulationConfig.collisionMesh;
    if (!tressfxPlacementResources->collisionMesh)
        throw std::runtime_error(
            "TressFX collision.mesh was declared but its OBJ was not imported: " +
            config.path);

    // 绑定姿态只用来确定固定分辨率（作者 cellSize 是目标格距）；域本身每帧
    // 从蒙皮后的代理包围盒重算，buffer 大小由分辨率决定、永不改变。
    const auto layoutResult = VL::Hair::MakeTressfxSdfLayout(
        *tressfxPlacementResources->collisionMesh, config.cellSize, config.margin);
    if (layoutResult.IsFailure())
        throw std::runtime_error("TressFX collision mesh distance field is invalid: " +
            layoutResult.Error().message + " path=" + config.path);
    const VL::Hair::TressfxSdfLayout& layout = layoutResult.Value();
    const uint32_t triangleCount =
        static_cast<uint32_t>(tressfxPlacementResources->collisionMesh->indices.size() / 3);
    const VL::Hair::TressfxSdfParams sdfParams =
        VL::Hair::MakeTressfxSdfParams(layout, triangleCount);

    // 64-byte std140 参数块按 frame-in-flight 分槽：域（origin/cellSize/truncation）
    // 每帧变化，不能与仍在飞的上帧共用一个槽。创建时先写绑定姿态域作初值。
    const vk::DeviceSize paramsBytes = sizeof(VL::Hair::TressfxSdfParams);
    for (uint32_t slot = 0; slot < VL::Hair::GuideSimulationParamBufferCount; ++slot)
    {
        auto& target = tressfxSdfParamsTargets[slot];
        std::tie(target.buffer, target.memory) = rendererBackend->CreateBuffer(
            paramsBytes,
            vk::BufferUsageFlagBits::eUniformBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            "TressFXSdfParams: " + name + ":" + std::to_string(slot));
        target.mapped = rendererBackend->MapMemory(target.memory, paramsBytes);
        std::memcpy(target.mapped, &sdfParams, sizeof(sdfParams));
        target.info.setBuffer(target.buffer).setOffset(0).setRange(paramsBytes);
    }

    // 代理顶点/索引：device-local，仅创建时经 staging 上传一次。splat pass 每帧
    // 重新蒙皮，因此这里存的是绑定姿态坐标。
    std::vector<Eigen::Vector4f> proxyPositions;
    proxyPositions.reserve(tressfxPlacementResources->collisionMesh->positions.size());
    for (const Eigen::Vector3f& position : tressfxPlacementResources->collisionMesh->positions)
        proxyPositions.emplace_back(position.x(), position.y(), position.z(), 0.0f);
    const vk::DeviceSize triangleBytes =
        static_cast<vk::DeviceSize>(proxyPositions.size()) * sizeof(Eigen::Vector4f);
    std::tie(tressfxSdfTriangleBuffer, tressfxSdfTriangleMemory) =
        VL::RenderableDetail::CreateUploadedDeviceLocalStorageBuffer(*rendererBackend,
            proxyPositions.data(),
            triangleBytes, "TressFXSdfProxyPositions: " + name);
    tressfxSdfTriangleInfo
        .setBuffer(tressfxSdfTriangleBuffer).setOffset(0).setRange(triangleBytes);

    const std::vector<std::uint32_t>& proxyIndices = tressfxPlacementResources->collisionMesh->indices;
    const vk::DeviceSize indexBytes =
        static_cast<vk::DeviceSize>(proxyIndices.size()) * sizeof(std::uint32_t);
    std::tie(tressfxSdfIndexBuffer, tressfxSdfIndexMemory) =
        VL::RenderableDetail::CreateUploadedDeviceLocalStorageBuffer(*rendererBackend,
            proxyIndices.data(),
            indexBytes, "TressFXSdfProxyIndices: " + name);
    tressfxSdfIndexInfo
        .setBuffer(tressfxSdfIndexBuffer).setOffset(0).setRange(indexBytes);

    // 距离格子 buffer 只由 compute 写：clear pass 每帧先填 INITIAL_DISTANCE
    // 哨兵，因此创建时不需要上传初值。分辨率固定，所以 buffer 大小不随帧变化。
    const vk::DeviceSize gridBytes =
        static_cast<vk::DeviceSize>(layout.CellCount()) * sizeof(std::uint32_t);
    std::tie(tressfxSdfGridBuffer, tressfxSdfGridMemory) = rendererBackend->CreateBuffer(
        gridBytes,
        vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        "TressFXSdfGrid: " + name);
    tressfxSdfGridInfo
        .setBuffer(tressfxSdfGridBuffer).setOffset(0).setRange(gridBytes);

    tressfxSdfTriangleCount = triangleCount;
    tressfxSdfCellCount = layout.CellCount();
    tressfxSdfResolution = layout.resolution;
    tressfxSdfCollision = true;

    // SDF splat 的 shader 静态声明 bindings 12/13，因此即使没有权重转移也必须
    // 提供有效 descriptor。占位影响流是 16 字节零；占位 palette 是 current/
    // previous 两个单位矩阵（128 bytes），shader 在 counts.y=0 时不读取它们。
    const std::array<std::uint32_t, 4> placeholderInfluences{0, 0, 0, 0};
    const vk::DeviceSize influenceBytes = sizeof(placeholderInfluences);
    std::tie(tressfxSdfProxyInfluenceBuffer, tressfxSdfProxyInfluenceMemory) =
        VL::RenderableDetail::CreateUploadedDeviceLocalStorageBuffer(*rendererBackend,
            placeholderInfluences.data(), influenceBytes,
            "TressFXSdfProxyInfluences: " + name);
    tressfxSdfProxyInfluenceInfo.setBuffer(tressfxSdfProxyInfluenceBuffer)
        .setOffset(0).setRange(influenceBytes);

    const std::array<Eigen::Matrix4f, 2> identityPalette{
        Eigen::Matrix4f::Identity(), Eigen::Matrix4f::Identity()};
    const vk::DeviceSize paletteBytes = sizeof(identityPalette);
    for (uint32_t slot = 0; slot < VL::Hair::GuideSimulationParamBufferCount; ++slot)
    {
        auto& target = tressfxSdfProxyPaletteTargets[slot];
        std::tie(target.buffer, target.memory) = rendererBackend->CreateBuffer(
            paletteBytes,
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            "TressFXSdfProxyPalette: " + name + ":" + std::to_string(slot));
        target.mapped = rendererBackend->MapMemory(target.memory, paletteBytes);
        std::memcpy(target.mapped, identityPalette.data(),
            static_cast<size_t>(paletteBytes));
        target.info.setBuffer(target.buffer).setOffset(0).setRange(paletteBytes);
    }

    std::cout << "[TressFX] SDF collision buffer: object=\"" << name
              << "\" proxyTriangles=" << triangleCount
              << " resolution=" << layout.resolution << "^3"
              << " cells=" << layout.CellCount()
              << " bindCellSize=" << layout.cellSize << " margin=" << layout.margin
              << " (domain is rebuilt per frame)" << std::endl;
}

void RenderableObject::DestroyTressfxBuffers()
{
    if (rendererBackend)
    {
        if (tressfxPositionBuffer)
            rendererBackend->DestroyBuffer(tressfxPositionBuffer, tressfxPositionMemory);
        if (tressfxGuideGroupIdBuffer)
            rendererBackend->DestroyBuffer(
                tressfxGuideGroupIdBuffer, tressfxGuideGroupIdMemory);
        if (tressfxGuideGroupColorBuffer)
            rendererBackend->DestroyBuffer(
                tressfxGuideGroupColorBuffer, tressfxGuideGroupColorMemory);
        for (size_t slot = 0; slot < tressfxBrushWeightBuffers.size(); ++slot)
            if (tressfxBrushWeightBuffers[slot])
                rendererBackend->DestroyBuffer(
                    tressfxBrushWeightBuffers[slot], tressfxBrushWeightMemories[slot]);
        tressfxBrushWeightBuffers.clear();
        tressfxBrushWeightMemories.clear();
        tressfxBrushWeightInfos.clear();
        tressfxBrushActive = false;
        if (tressfxPreviousPositionBuffer)
            rendererBackend->DestroyBuffer(
                tressfxPreviousPositionBuffer, tressfxPreviousPositionMemory);
        if (tressfxPreviousPreviousPositionBuffer)
            rendererBackend->DestroyBuffer(
                tressfxPreviousPreviousPositionBuffer,
                tressfxPreviousPreviousPositionMemory);
        if (tressfxRestPositionBuffer)
            rendererBackend->DestroyBuffer(
                tressfxRestPositionBuffer, tressfxRestPositionMemory);
        if (tressfxRestLengthBuffer)
            rendererBackend->DestroyBuffer(
                tressfxRestLengthBuffer, tressfxRestLengthMemory);
        if (tressfxStrandLevelBuffer)
            rendererBackend->DestroyBuffer(
                tressfxStrandLevelBuffer, tressfxStrandLevelMemory);
        for (auto& target : tressfxColliderTargets)
        {
            if (target.mapped)
                rendererBackend->UnmapMemory(target.memory);
            if (target.buffer)
                rendererBackend->DestroyBuffer(target.buffer, target.memory);
            target = {};
        }
        for (auto& target : tressfxGuideGroupConfigTargets)
        {
            if (target.mapped)
                rendererBackend->UnmapMemory(target.memory);
            if (target.buffer)
                rendererBackend->DestroyBuffer(target.buffer, target.memory);
            target = {};
        }
        if (tressfxSdfGridBuffer)
            rendererBackend->DestroyBuffer(
                tressfxSdfGridBuffer, tressfxSdfGridMemory);
        if (tressfxSdfTriangleBuffer)
            rendererBackend->DestroyBuffer(
                tressfxSdfTriangleBuffer, tressfxSdfTriangleMemory);
        if (tressfxSdfIndexBuffer)
            rendererBackend->DestroyBuffer(
                tressfxSdfIndexBuffer, tressfxSdfIndexMemory);
        if (tressfxSdfProxyInfluenceBuffer)
            rendererBackend->DestroyBuffer(tressfxSdfProxyInfluenceBuffer,
                tressfxSdfProxyInfluenceMemory);
        for (auto& target : tressfxSdfProxyPaletteTargets)
        {
            if (target.mapped)
                rendererBackend->UnmapMemory(target.memory);
            if (target.buffer)
                rendererBackend->DestroyBuffer(target.buffer, target.memory);
            target = {};
        }
        for (auto& target : tressfxSdfParamsTargets)
        {
            if (target.mapped)
                rendererBackend->UnmapMemory(target.memory);
            if (target.buffer)
                rendererBackend->DestroyBuffer(target.buffer, target.memory);
            target = {};
        }
        for (auto& target : tressfxParamsTargets)
        {
            if (target.mapped)
                rendererBackend->UnmapMemory(target.memory);
            if (target.buffer)
                rendererBackend->DestroyBuffer(target.buffer, target.memory);
            target = {};
        }
        for (auto& target : tressfxResetParamsTargets)
        {
            if (target.mapped)
                rendererBackend->UnmapMemory(target.memory);
            if (target.buffer)
                rendererBackend->DestroyBuffer(target.buffer, target.memory);
            target = {};
        }
        for (auto& target : tressfxPaletteTargets)
        {
            if (target.mapped)
                rendererBackend->UnmapMemory(target.memory);
            if (target.buffer)
                rendererBackend->DestroyBuffer(target.buffer, target.memory);
            target = {};
        }
        for (auto& target : tressfxPenetrationStatsTargets)
        {
            if (target.mapped)
                rendererBackend->UnmapMemory(target.memory);
            if (target.buffer)
                rendererBackend->DestroyBuffer(target.buffer, target.memory);
            target = {};
        }
        if (tressfxBoneWeightsBuffer || tressfxBoneWeightsMemory)
            rendererBackend->DestroyBuffer(
                tressfxBoneWeightsBuffer, tressfxBoneWeightsMemory);
    }
    tressfxBoneWeightsBuffer = nullptr;
    tressfxBoneWeightsMemory = nullptr;
    tressfxBoneWeightsInfo = vk::DescriptorBufferInfo{};
    tressfxPaletteJointCapacity = 0;
    tressfxPositionBuffer = nullptr;
    tressfxPositionMemory = nullptr;
    tressfxPreviousPositionBuffer = nullptr;
    tressfxPreviousPositionMemory = nullptr;
    tressfxPreviousPreviousPositionBuffer = nullptr;
    tressfxPreviousPreviousPositionMemory = nullptr;
    tressfxRestPositionBuffer = nullptr;
    tressfxRestPositionMemory = nullptr;
    tressfxRestLengthBuffer = nullptr;
    tressfxRestLengthMemory = nullptr;
    tressfxStrandLevelBuffer = nullptr;
    tressfxStrandLevelMemory = nullptr;
    tressfxColliderBuffer = nullptr;
    tressfxColliderMemory = nullptr;
    tressfxSdfGridBuffer = nullptr;
    tressfxSdfGridMemory = nullptr;
    tressfxSdfTriangleBuffer = nullptr;
    tressfxSdfTriangleMemory = nullptr;
    tressfxSdfIndexBuffer = nullptr;
    tressfxSdfIndexMemory = nullptr;
    tressfxSdfTriangleCount = 0;
    tressfxSdfCellCount = 0;
    tressfxSdfResolution = 0;
    tressfxSdfCollision = false;
    reportedSdfDomain = false;
    reportedSdfDomainFailure = false;
    tressfxGpuSimulation = false;
    tressfxBrushWeightBuffers.clear();
    tressfxBrushWeightMemories.clear();
    tressfxBrushWeightInfos.clear();
}
