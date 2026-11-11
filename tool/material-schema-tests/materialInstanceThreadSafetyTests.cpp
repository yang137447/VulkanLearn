#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "materialInstance.h"
#include "render/backend/rendererBackendVulkan.h"

// material_schema_tests 不链接 Vulkan 后端。materialInstance.cpp 的状态机
// 本身只依赖 Buffer 容器的清空/描述符信息刷新；这里提供最小实现，让参数与
// 纹理状态可以在不启动 Vulkan 设备的情况下被并发测试。
namespace VL
{

void RendererBackendVulkan::CreatePerSwapchainBufferSet(
    Buffer&,
    vk::DeviceSize,
    vk::BufferUsageFlags,
    vk::MemoryPropertyFlags,
    const std::string&,
    bool)
{
}

void RendererBackendVulkan::DestroyBufferSet(Buffer&)
{
}

void RendererBackendVulkan::SetupDescriptorBufferInfos(Buffer&)
{
}

} // namespace VL

namespace
{

void RequireAllEqual(
    const MaterialInstanceNumericParameterValues& values,
    const std::vector<std::string>& names,
    float expected)
{
    for (const std::string& name : names)
    {
        const auto it = values.find(name);
        EXPECT_NE(it, values.end()) << name;
        if (it == values.end())
        {
            continue;
        }
        const float* value = std::get_if<float>(&it->second);
        EXPECT_NE(value, nullptr) << name;
        if (value != nullptr)
        {
            EXPECT_FLOAT_EQ(*value, expected) << name;
        }
    }
}

} // namespace

TEST(MaterialInstanceThreadSafety, NumericAndTextureSemanticsPreserved)
{
    MaterialInstance instance;
    instance.SetName("Common/MI_ThreadSafety.json");

    instance.SetParameter("u_float", 0.25f);
    instance.SetParameter("u_vec2", Eigen::Vector2f(1.0f, 2.0f));
    instance.SetParameter("u_vec3", Eigen::Vector3f(3.0f, 4.0f, 5.0f));
    instance.SetParameter("u_vec4", Eigen::Vector4f(6.0f, 7.0f, 8.0f, 9.0f));

    EXPECT_TRUE(instance.HasParameter("u_float"));
    EXPECT_FALSE(instance.HasParameter("u_missing"));
    EXPECT_FLOAT_EQ(instance.GetParameter<float>("u_float"), 0.25f);
    EXPECT_EQ(
        instance.GetParameter<Eigen::Vector2f>("u_vec2"),
        Eigen::Vector2f(1.0f, 2.0f));
    EXPECT_EQ(
        instance.GetParameter<Eigen::Vector3f>("u_vec3"),
        Eigen::Vector3f(3.0f, 4.0f, 5.0f));
    EXPECT_EQ(
        instance.GetParameter<Eigen::Vector4f>("u_vec4"),
        Eigen::Vector4f(6.0f, 7.0f, 8.0f, 9.0f));
    EXPECT_THROW(
        instance.GetParameter<Eigen::Vector2f>("u_float"),
        std::runtime_error);

    // GetParameters 返回副本，调用方修改副本不能影响实例本身。
    auto parameters = instance.GetParameters();
    ASSERT_EQ(parameters.size(), 4u);
    EXPECT_EQ(parameters.at("u_vec3").type, ParamType::Vec3);
    parameters.erase("u_float");
    EXPECT_TRUE(instance.HasParameter("u_float"));

    const MaterialInstanceNumericParameterValues snapshot =
        instance.CaptureNumericParameterValues();
    ASSERT_EQ(snapshot.size(), 4u);
    EXPECT_FLOAT_EQ(std::get<float>(snapshot.at("u_float")), 0.25f);
    EXPECT_EQ(
        std::get<Eigen::Vector4f>(snapshot.at("u_vec4")),
        Eigen::Vector4f(6.0f, 7.0f, 8.0f, 9.0f));

    const std::shared_ptr<Texture> nullTexture;
    instance.SetTexture("albedo", nullTexture);
    EXPECT_TRUE(instance.HasTexture("albedo"));
    EXPECT_EQ(instance.GetTexture("albedo"), nullptr);
    EXPECT_FALSE(instance.HasTexture("normal"));

    instance.SetTexture(
        "albedoWithIdentity",
        nullTexture,
        std::string("./Common/../Common/T_albedo.png"),
        std::string("cache/tex"));

    const MaterialInstanceStateSnapshot state = instance.CaptureStateSnapshot();
    EXPECT_EQ(
        state.normalizedMaterialInstanceIdentity,
        "Common/MI_ThreadSafety.json");
    ASSERT_EQ(state.textures.size(), 2u);
    const auto& identity =
        state.textures.at("albedoWithIdentity").textureAssetIdentity;
    ASSERT_TRUE(identity.has_value());
    EXPECT_EQ(*identity, "Common/T_albedo.png");
}

TEST(MaterialInstanceThreadSafety, CommitNumericParameterValuesStaysTransactional)
{
    MaterialInstance instance;
    instance.SetParameter("u_a", 0.0f);
    instance.SetParameter("u_b", 0.0f);

    MaterialInstanceNumericParameterValues good;
    good.emplace("u_a", 1.0f);
    good.emplace("u_b", 1.0f);
    instance.CommitNumericParameterValues(good);
    EXPECT_FLOAT_EQ(instance.GetParameter<float>("u_a"), 1.0f);
    EXPECT_FLOAT_EQ(instance.GetParameter<float>("u_b"), 1.0f);

    MaterialInstanceNumericParameterValues unknown;
    unknown.emplace("u_missing", 2.0f);
    EXPECT_THROW(
        instance.CommitNumericParameterValues(unknown),
        std::invalid_argument);

    MaterialInstanceNumericParameterValues wrongType;
    wrongType.emplace("u_a", Eigen::Vector2f(2.0f, 3.0f));
    EXPECT_THROW(
        instance.CommitNumericParameterValues(wrongType),
        std::invalid_argument);

    MaterialInstanceNumericParameterValues notFinite;
    notFinite.emplace(
        "u_a",
        std::numeric_limits<float>::quiet_NaN());
    EXPECT_THROW(
        instance.CommitNumericParameterValues(notFinite),
        std::invalid_argument);

    // 三次失败提交都不能留下部分更新。
    EXPECT_FLOAT_EQ(instance.GetParameter<float>("u_a"), 1.0f);
    EXPECT_FLOAT_EQ(instance.GetParameter<float>("u_b"), 1.0f);
}

TEST(MaterialInstanceThreadSafety, SnapshotNeverSeesTornCommit)
{
    const std::vector<std::string> names = {"u_a", "u_b", "u_c", "u_d"};
    constexpr int kCommitIterations = 20000;

    MaterialInstance instance;
    for (const std::string& name : names)
    {
        instance.SetParameter(name, 0.0f);
    }

    MaterialInstanceNumericParameterValues allZero;
    MaterialInstanceNumericParameterValues allOne;
    for (const std::string& name : names)
    {
        allZero.emplace(name, 0.0f);
        allOne.emplace(name, 1.0f);
    }

    std::atomic<int> tornSnapshots{0};
    std::atomic<int> observedSnapshots{0};

    std::thread writer(
        [&]()
        {
            for (int iteration = 0; iteration < kCommitIterations; ++iteration)
            {
                instance.CommitNumericParameterValues(
                    (iteration & 1) == 0 ? allOne : allZero);
            }
        });

    std::vector<std::thread> readers;
    readers.reserve(2);
    for (int readerIndex = 0; readerIndex < 2; ++readerIndex)
    {
        readers.emplace_back(
            [&]()
            {
                for (int snapshotIndex = 0;
                     snapshotIndex < kCommitIterations;
                     ++snapshotIndex)
                {
                    const MaterialInstanceNumericParameterValues snapshot =
                        instance.CaptureNumericParameterValues();
                    if (snapshot.size() != names.size())
                    {
                        tornSnapshots.fetch_add(1);
                        continue;
                    }

                    bool sawZero = false;
                    bool sawOne = false;
                    bool invalid = false;
                    for (const std::string& name : names)
                    {
                        const auto it = snapshot.find(name);
                        if (it == snapshot.end())
                        {
                            invalid = true;
                            break;
                        }
                        const float* value = std::get_if<float>(&it->second);
                        if (value == nullptr)
                        {
                            invalid = true;
                            break;
                        }
                        if (*value == 0.0f)
                        {
                            sawZero = true;
                        }
                        else if (*value == 1.0f)
                        {
                            sawOne = true;
                        }
                        else
                        {
                            invalid = true;
                            break;
                        }
                    }

                    if (invalid || (sawZero && sawOne))
                    {
                        tornSnapshots.fetch_add(1);
                    }
                    observedSnapshots.fetch_add(1);
                }
            });
    }

    writer.join();
    for (std::thread& reader : readers)
    {
        reader.join();
    }

    EXPECT_EQ(tornSnapshots.load(), 0);
    EXPECT_GT(observedSnapshots.load(), 0);
    RequireAllEqual(
        instance.CaptureNumericParameterValues(),
        names,
        (kCommitIterations & 1) == 0 ? 0.0f : 1.0f);
}

TEST(MaterialInstanceThreadSafety, StateSnapshotSharesTheCommitBoundary)
{
    constexpr int kCommitIterations = 5000;
    const std::vector<std::string> names = {"u_a", "u_b", "u_c"};

    MaterialInstance instance;
    for (const std::string& name : names)
    {
        instance.SetParameter(name, 0.0f);
    }

    MaterialInstanceNumericParameterValues allOne;
    for (const std::string& name : names)
    {
        allOne.emplace(name, 1.0f);
    }

    std::atomic<int> tornSnapshots{0};

    std::thread writer(
        [&]()
        {
            for (int iteration = 0; iteration < kCommitIterations; ++iteration)
            {
                instance.CommitNumericParameterValues(
                    (iteration & 1) == 0
                        ? allOne
                        : MaterialInstanceNumericParameterValues{
                            {"u_a", 0.0f},
                            {"u_b", 0.0f},
                            {"u_c", 0.0f}});
            }
        });

    for (int snapshotIndex = 0;
         snapshotIndex < kCommitIterations;
         ++snapshotIndex)
    {
        const MaterialInstanceStateSnapshot snapshot =
            instance.CaptureStateSnapshot();
        float firstValue = -1.0f;
        bool mixed = false;
        for (const std::string& name : names)
        {
            const auto it = snapshot.parameters.find(name);
            if (it == snapshot.parameters.end())
            {
                tornSnapshots.fetch_add(1);
                mixed = true;
                break;
            }
            const float value = std::get<float>(it->second);
            if (firstValue < 0.0f)
            {
                firstValue = value;
            }
            else if (value != firstValue)
            {
                mixed = true;
            }
        }
        if (mixed)
        {
            tornSnapshots.fetch_add(1);
        }
    }

    writer.join();
    EXPECT_EQ(tornSnapshots.load(), 0);
}
