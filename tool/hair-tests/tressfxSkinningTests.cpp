#include "hair/tressfxSkinning.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace
{
const std::vector<std::string> JointNames = {
    "FemaleRoot", "Spine1_M", "Spine2_M", "Neck_M", "Head_M", "HeadEnd_M"};
}

TEST(TressfxSkinning, FindsRootJointByName)
{
    EXPECT_EQ(VL::Hair::ResolveTressfxRootJoint(
                  JointNames, "FemaleCharacter", "Head_M", "SM_hair.json"),
        4u);
    EXPECT_EQ(VL::Hair::ResolveTressfxRootJoint(
                  JointNames, "FemaleCharacter", "FemaleRoot", "SM_hair.json"),
        0u);
}

TEST(TressfxSkinning, MissingBoneErrorNamesBoneAndAsset)
{
    try
    {
        VL::Hair::ResolveTressfxRootJoint(
            JointNames, "FemaleCharacter", "Head_NotFound", "Maps/SC_x/Meshes/SM_hair_skinned.json");
        FAIL() << "Missing root bone was accepted";
    }
    catch (const std::runtime_error& error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("Head_NotFound"), std::string::npos);
        EXPECT_NE(message.find("FemaleCharacter"), std::string::npos);
        EXPECT_NE(message.find("SM_hair_skinned.json"), std::string::npos);
    }
}

TEST(TressfxSkinning, MissingSkeletonErrorNamesObjectAndAsset)
{
    try
    {
        VL::Hair::ResolveTressfxRootJoint(
            {}, "FemaleCharacter", "Head_M", "Maps/SC_x/Meshes/SM_hair_skinned.json");
        FAIL() << "Source object without a skeleton was accepted";
    }
    catch (const std::runtime_error& error)
    {
        const std::string message = error.what();
        EXPECT_NE(message.find("FemaleCharacter"), std::string::npos);
        EXPECT_NE(message.find("skeleton"), std::string::npos);
        EXPECT_NE(message.find("SM_hair_skinned.json"), std::string::npos);
    }
}

TEST(TressfxSkinning, RootPrefixUsesSkinningMatrixOnly)
{
    Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
    matrix.block<3, 3>(0, 0) =
        Eigen::AngleAxisf(0.5f, Eigen::Vector3f::UnitY()).toRotationMatrix();
    matrix.block<3, 1>(0, 3) = Eigen::Vector3f(11.0f, 22.0f, 33.0f);
    const Eigen::Vector3f restRoot0(1.0f, 2.0f, 3.0f);
    const Eigen::Vector3f restRoot1(4.0f, 5.0f, 6.0f);
    const Eigen::Vector3f restSimulated(7.0f, 8.0f, 9.0f);
    const Eigen::Vector3f restPinnedBeyondPrefix(10.0f, 11.0f, 12.0f);

    const auto root0 =
        VL::Hair::TressfxFixedPointPosition(matrix, restRoot0, 0, 2);
    const auto root1 =
        VL::Hair::TressfxFixedPointPosition(matrix, restRoot1, 1, 2);
    EXPECT_TRUE(root0.isApprox((matrix * restRoot0.homogeneous()).head<3>()));
    EXPECT_TRUE(root1.isApprox((matrix * restRoot1.homogeneous()).head<3>()));

    // 前缀之外的点（模拟点，或以逆质量固定但不属于根前缀的点）不消费 M。
    EXPECT_TRUE(VL::Hair::TressfxFixedPointPosition(matrix, restSimulated, 2, 2)
            .isApprox(restSimulated));
    EXPECT_TRUE(VL::Hair::TressfxFixedPointPosition(matrix, restPinnedBeyondPrefix, 3, 2)
            .isApprox(restPinnedBeyondPrefix));
}
