#include "hair/tressfxSkinning.h"
#include "hair/tressfxSolverMath.h"

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

TEST(TressfxSkinning, ImmovablePointsUseSkinningMatrix)
{
    Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
    matrix.block<3, 3>(0, 0) =
        Eigen::AngleAxisf(0.5f, Eigen::Vector3f::UnitY()).toRotationMatrix();
    matrix.block<3, 1>(0, 3) = Eigen::Vector3f(11.0f, 22.0f, 33.0f);
    const Eigen::Vector3f restRoot0(1.0f, 2.0f, 3.0f);
    const Eigen::Vector3f restRoot1(4.0f, 5.0f, 6.0f);
    const Eigen::Vector3f restPinnedBeyondPrefix(10.0f, 11.0f, 12.0f);

    // TressFX 的不可移动点（根前缀 ∪ 逆质量 <= 0）整步写 InitialPos = M * rest。
    EXPECT_TRUE(VL::Hair::TressfxInitialPosition(matrix, restRoot0)
            .isApprox((matrix * restRoot0.homogeneous()).head<3>()));
    EXPECT_TRUE(VL::Hair::TressfxInitialPosition(matrix, restRoot1)
            .isApprox((matrix * restRoot1.homogeneous()).head<3>()));
    EXPECT_TRUE(VL::Hair::TressfxInitialPosition(matrix, restPinnedBeyondPrefix)
            .isApprox((matrix * restPinnedBeyondPrefix.homogeneous()).head<3>()));

    // 可移动点不消费上面这条 pin 规则；不可移动判定是根前缀或逆质量 <= 0。
    EXPECT_FALSE(VL::Hair::TressfxIsImmovable(2, 2, 1.0f));
    EXPECT_TRUE(VL::Hair::TressfxIsImmovable(1, 2, 1.0f));
    EXPECT_TRUE(VL::Hair::TressfxIsImmovable(3, 2, 0.0f));
}
