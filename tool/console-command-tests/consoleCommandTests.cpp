#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "engine/consoleCommandParsing.h"
#include "render/camera/cameraExposure.h"

namespace
{
using VL::ConsoleCommandParsing::ParseMaterialParameter;
using VL::ConsoleCommandParsing::ParseSunIntensity;

TEST(ConsoleCommandParsing, SunAcceptsPositiveFiniteValue)
{
    const auto parsed = ParseSunIntensity(" 70 ");
    ASSERT_TRUE(parsed.ok);
    EXPECT_FLOAT_EQ(parsed.lux, 70.0f);
}

TEST(ConsoleCommandParsing, SunRejectsNonPositiveAndMalformedValues)
{
    EXPECT_FALSE(ParseSunIntensity("-5").ok);
    EXPECT_FALSE(ParseSunIntensity("0").ok);
    EXPECT_FALSE(ParseSunIntensity("abc").ok);
    EXPECT_FALSE(ParseSunIntensity("nan").ok);
    EXPECT_FALSE(ParseSunIntensity("inf").ok);
    EXPECT_FALSE(ParseSunIntensity("70 80").ok);
    EXPECT_FALSE(ParseSunIntensity("").ok);
}

TEST(ConsoleCommandParsing, MaterialParameterKeepsKeyAndScalarValue)
{
    const auto parsed = ParseMaterialParameter(
        "Maps/SC_gun_outline_demo/Materials/MI_gun_body.json#bp/__blueprint/dropped-gun/gun/0 "
        "u_specular 0.35");
    ASSERT_TRUE(parsed.ok);
    EXPECT_EQ(parsed.materialInstanceKey,
        "Maps/SC_gun_outline_demo/Materials/MI_gun_body.json#bp/__blueprint/dropped-gun/gun/0");
    EXPECT_EQ(parsed.parameterName, "u_specular");
    EXPECT_EQ(parsed.valueCount, 1u);
    EXPECT_FLOAT_EQ(parsed.values[0], 0.35f);
}

TEST(ConsoleCommandParsing, MaterialParameterAcceptsUpToFourComponents)
{
    const auto four = ParseMaterialParameter("key#bp/a/0 u_tintColor 1 0.5 0.25 1");
    ASSERT_TRUE(four.ok);
    EXPECT_EQ(four.valueCount, 4u);
    EXPECT_FLOAT_EQ(four.values[3], 1.0f);

    const auto three = ParseMaterialParameter("key#bp/a/0 u_x 1 2 3");
    ASSERT_TRUE(three.ok);
    EXPECT_EQ(three.valueCount, 3u);
}

TEST(ConsoleCommandParsing, MaterialParameterRejectsMalformedArguments)
{
    EXPECT_FALSE(ParseMaterialParameter("").ok);
    EXPECT_FALSE(ParseMaterialParameter("key#bp/a/0").ok);
    EXPECT_FALSE(ParseMaterialParameter("key#bp/a/0 u_x").ok);
    EXPECT_FALSE(ParseMaterialParameter("key#bp/a/0 u_x 1 2 3 4 5").ok);
    EXPECT_FALSE(ParseMaterialParameter("key#bp/a/0 u_x abc").ok);
    EXPECT_FALSE(ParseMaterialParameter("key#bp/a/0 u_x nan").ok);
}

TEST(ConsoleCommandParsing, ExposureFieldWhitelistMatchesEditorPath)
{
    struct FieldValue { const char* field; double value; };
    const FieldValue cases[] = {
        { "mode", 1.0 }, { "ev100", 4.0 }, { "aperture", 4.0 },
        { "shutterSeconds", 0.008 }, { "iso", 100.0 }, { "compensation", 0.0 },
        { "autoMinEv100", -10.0 }, { "autoMaxEv100", 20.0 },
        { "autoSpeedUp", 3.0 }, { "autoSpeedDown", 1.0 }, { "autoTargetGray", 0.18 },
    };
    for (const FieldValue& entry : cases)
    {
        EXPECT_TRUE(VL::IsCameraExposureField(entry.field)) << entry.field;
        EXPECT_NO_THROW((void)VL::EditCameraExposure(VL::CameraExposure{}, entry.field, entry.value))
            << entry.field;
    }
}

TEST(ConsoleCommandParsing, ExposureRejectsUnknownField)
{
    EXPECT_FALSE(VL::IsCameraExposureField("bogus"));
    EXPECT_THROW(
        (void)VL::EditCameraExposure(VL::CameraExposure{}, "bogus", 1.0),
        std::invalid_argument);
}

} // namespace
