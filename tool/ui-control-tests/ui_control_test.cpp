#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <gtest/gtest.h>
#include <RmlUi/Core/BaseXMLParser.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/StreamMemory.h>

#include "ui/uiBindingUpdateScope.h"

namespace VL
{
namespace
{

class RuntimeControlParser : public Rml::BaseXMLParser
{
public:
    std::unordered_map<std::string, Rml::XMLAttributes> inputs;
    std::unordered_map<std::string, Rml::XMLAttributes> buttons;

    void HandleElementStart(
        const Rml::String& name, const Rml::XMLAttributes& attributes) override
    {
        if (name == "input" || name == "button")
        {
            const auto id = attributes.find("id");
            if (id != attributes.end())
            {
                auto& elements = name == "input" ? inputs : buttons;
                elements.emplace(id->second.Get<Rml::String>(), attributes);
            }
        }
    }
};

class RuntimeControlAssets : public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::ifstream file(VL_RUNTIME_CONTROL_DOCUMENT);
        ASSERT_TRUE(file.is_open());
        const std::string document(
            (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        Rml::StreamMemory stream(
            reinterpret_cast<const Rml::byte*>(document.data()), document.size());
        parser.Parse(&stream);
    }

    RuntimeControlParser parser;
};

TEST_F(RuntimeControlAssets, SunRangePreservesMigratedLuxValues)
{
    const auto control = parser.inputs.find("sun-intensity-slider");
    ASSERT_NE(control, parser.inputs.end());
    const auto& attributes = control->second;
    EXPECT_LE(attributes.at("min").Get<float>(), 1000.0f);
    EXPECT_GE(attributes.at("max").Get<float>(), 100000.0f);
    EXPECT_EQ(attributes.at("data-value").Get<Rml::String>(), "sun_intensity");
}

TEST_F(RuntimeControlAssets, ShadowRangePreservesCentimeterDistances)
{
    const auto control = parser.inputs.find("shadow-dynamic-distance-slider");
    ASSERT_NE(control, parser.inputs.end());
    const auto& attributes = control->second;
    EXPECT_LE(attributes.at("min").Get<float>(), 30000.0f);
    EXPECT_GE(attributes.at("max").Get<float>(), 100000.0f);
    EXPECT_EQ(attributes.at("data-value").Get<Rml::String>(), "csm_dynamic_shadow_distance");
}

TEST_F(RuntimeControlAssets, CameraSpeedRangeUsesCentimetersPerSecond)
{
    const auto control = parser.inputs.find("camera-move-speed-slider");
    ASSERT_NE(control, parser.inputs.end());
    const auto& attributes = control->second;
    EXPECT_FLOAT_EQ(attributes.at("min").Get<float>(), 50.0f);
    EXPECT_FLOAT_EQ(attributes.at("max").Get<float>(), 10000.0f);
    EXPECT_FLOAT_EQ(attributes.at("step").Get<float>(), 50.0f);
    EXPECT_EQ(attributes.at("data-value").Get<Rml::String>(), "camera_move_speed");
}

TEST_F(RuntimeControlAssets, ExposureModesOnlyOfferManualAndAuto)
{
    const auto manual = parser.buttons.find("exposure-manual");
    const auto automatic = parser.buttons.find("exposure-auto");
    ASSERT_NE(manual, parser.buttons.end());
    ASSERT_NE(automatic, parser.buttons.end());
    EXPECT_EQ(parser.buttons.count("exposure-physical"), 0u);
    EXPECT_EQ(manual->second.at("data-event-click").Get<Rml::String>(),
        "set_exposure_mode(0)");
    EXPECT_EQ(automatic->second.at("data-event-click").Get<Rml::String>(),
        "set_exposure_mode(2)");
}

TEST_F(RuntimeControlAssets, PhysicalCameraIsCategorizedAsManual)
{
    const auto manual = parser.buttons.find("exposure-manual");
    ASSERT_NE(manual, parser.buttons.end());
    EXPECT_EQ(manual->second.at("data-class-selected").Get<Rml::String>(),
        "exposure_mode != 2");
}

TEST_F(RuntimeControlAssets, ExposureControlsExposeManualAndAutoRanges)
{
    const auto manualEv = parser.inputs.find("exposure-ev-slider");
    ASSERT_NE(manualEv, parser.inputs.end());
    EXPECT_FLOAT_EQ(manualEv->second.at("min").Get<float>(), -24.0f);
    EXPECT_FLOAT_EQ(manualEv->second.at("max").Get<float>(), 32.0f);
    EXPECT_EQ(
        manualEv->second.at("data-value").Get<Rml::String>(),
        "exposure_ev");

    const auto compensation = parser.inputs.find("exposure-compensation-slider");
    ASSERT_NE(compensation, parser.inputs.end());
    EXPECT_EQ(
        compensation->second.at("data-value").Get<Rml::String>(),
        "exposure_compensation");

    const auto minEv = parser.inputs.find("exposure-min-ev-slider");
    const auto maxEv = parser.inputs.find("exposure-max-ev-slider");
    ASSERT_NE(minEv, parser.inputs.end());
    ASSERT_NE(maxEv, parser.inputs.end());
    EXPECT_EQ(
        minEv->second.at("data-value").Get<Rml::String>(),
        "exposure_min_ev");
    EXPECT_EQ(
        maxEv->second.at("data-value").Get<Rml::String>(),
        "exposure_max_ev");
}

TEST(UiBindingUpdate, RestoresUserInputAfterUpdate)
{
    bool updating = false;
    {
        UiBindingUpdateScope scope(updating);
        EXPECT_TRUE(updating);
    }
    EXPECT_FALSE(updating);
}

TEST(UiBindingUpdate, NestedReloadKeepsOuterUpdateSuppressed)
{
    bool updating = false;
    {
        UiBindingUpdateScope outer(updating);
        {
            UiBindingUpdateScope reload(updating);
            EXPECT_TRUE(updating);
        }
        EXPECT_TRUE(updating);
    }
    EXPECT_FALSE(updating);
}

TEST(UiBindingUpdate, RestoresInputAfterFailedUpdate)
{
    bool updating = false;
    try
    {
        UiBindingUpdateScope scope(updating);
        throw std::runtime_error("failed binding update");
    }
    catch (const std::runtime_error&)
    {
    }
    EXPECT_FALSE(updating);
}

class RmlRangeChanges : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(Rml::Initialise());
    }

    void TearDown() override
    {
        Rml::Shutdown();
    }
};

class RangeChangeListener : public Rml::EventListener
{
public:
    RangeChangeListener(bool& updating, float& authoredValue)
        : updating(updating), authoredValue(authoredValue)
    {
    }

    void ProcessEvent(Rml::Event& event) override
    {
        ++changeCount;
        if (!updating)
            authoredValue = event.GetParameter<float>("value", authoredValue);
    }

    int changeCount = 0;

private:
    bool& updating;
    float& authoredValue;
};

TEST_F(RmlRangeChanges, OldSunRangeClampsAndWritesBackWithoutScope)
{
    bool updating = false;
    float illuminance = 1000.0f;
    RangeChangeListener listener(updating, illuminance);
    auto input = Rml::Factory::InstanceElement(
        nullptr, "input", "input",
        {{"type", Rml::Variant("range")}, {"min", Rml::Variant(0)},
         {"max", Rml::Variant(10)}, {"step", Rml::Variant(1)}});
    ASSERT_NE(input, nullptr);
    input->AddEventListener("change", &listener);
    input->SetAttribute("value", illuminance);

    EXPECT_GT(listener.changeCount, 0);
    EXPECT_FLOAT_EQ(illuminance, 10.0f);
}

TEST_F(RmlRangeChanges, ScopedRefreshPreservesValueAndStillAllowsUserChanges)
{
    bool updating = false;
    float illuminance = 1000.0f;
    RangeChangeListener listener(updating, illuminance);
    auto input = Rml::Factory::InstanceElement(
        nullptr, "input", "input",
        {{"type", Rml::Variant("range")}, {"min", Rml::Variant(0)},
         {"max", Rml::Variant(10)}, {"step", Rml::Variant(1)}});
    ASSERT_NE(input, nullptr);
    input->AddEventListener("change", &listener);
    {
        UiBindingUpdateScope scope(updating);
        input->SetAttribute("value", illuminance);
    }
    EXPECT_GT(listener.changeCount, 0);
    EXPECT_FLOAT_EQ(input->GetAttribute<float>("value", 0), 10.0f);
    EXPECT_FLOAT_EQ(illuminance, 1000.0f);

    input->SetAttribute("value", 7.0f);
    EXPECT_FLOAT_EQ(illuminance, 7.0f);
}

} // namespace
} // namespace VL
