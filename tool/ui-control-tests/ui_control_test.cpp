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

class EditorWorkspaceParser : public Rml::BaseXMLParser
{
public:
    std::unordered_map<std::string, Rml::XMLAttributes> inputs;
    std::unordered_map<std::string, Rml::XMLAttributes> buttons;
    std::unordered_map<std::string, Rml::XMLAttributes> selects;

    void HandleElementStart(
        const Rml::String& name, const Rml::XMLAttributes& attributes) override
    {
        if (name == "input" || name == "button" || name == "select")
        {
            const auto id = attributes.find("id");
            if (id != attributes.end())
            {
                auto& elements = name == "input" ? inputs : name == "button" ? buttons : selects;
                elements.emplace(id->second.Get<Rml::String>(), attributes);
            }
        }
    }
};

class EditorWorkspaceAssets : public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::ifstream file(VL_EDITOR_WORKSPACE_DOCUMENT);
        ASSERT_TRUE(file.is_open());
        const std::string document(
            (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        Rml::StreamMemory stream(
            reinterpret_cast<const Rml::byte*>(document.data()), document.size());
        parser.Parse(&stream);
    }

    EditorWorkspaceParser parser;
};

// 动态 sun/exposure 范围由 editor_ui_tests 覆盖；此处只核对现役工作区的静态声明。
TEST_F(EditorWorkspaceAssets, ScriptPlaybackUsesTypedStateValues)
{
    for (const auto& state : {std::pair{"script-stop", "0"},
        std::pair{"script-play", "1"}, std::pair{"script-pause", "2"}})
    {
        const auto control = parser.buttons.find(state.first);
        ASSERT_NE(control, parser.buttons.end());
        EXPECT_EQ(control->second.at("data-action").Get<Rml::String>(), "script-control");
        EXPECT_EQ(control->second.at("data-value").Get<Rml::String>(), state.second);
    }
}

TEST_F(EditorWorkspaceAssets, ScriptTimeScaleIsTextWithoutRangeRefreshClamping)
{
    const auto control = parser.inputs.find("script-time-scale");
    ASSERT_NE(control, parser.inputs.end());
    const auto& attributes = control->second;
    EXPECT_EQ(attributes.at("type").Get<Rml::String>(), "text");
    EXPECT_EQ(attributes.at("value").Get<Rml::String>(), "1");
    EXPECT_EQ(attributes.at("data-action").Get<Rml::String>(), "script-time-scale");
}

TEST_F(EditorWorkspaceAssets, LogFiltersUseIndependentValueActions)
{
    for (const auto* id : {"log-source", "log-search", "log-scripts-search"})
    {
        const auto control = parser.inputs.find(id);
        ASSERT_NE(control, parser.inputs.end());
        EXPECT_EQ(control->second.at("data-action").Get<Rml::String>(), id);
        EXPECT_EQ(control->second.at("type").Get<Rml::String>(), "text");
    }
    const auto severity = parser.selects.find("log-severity");
    ASSERT_NE(severity, parser.selects.end());
    EXPECT_EQ(severity->second.at("data-action").Get<Rml::String>(), "log-severity");
}

TEST_F(EditorWorkspaceAssets, LogAutoScrollStartsEnabledAsCheckbox)
{
    const auto control = parser.inputs.find("log-autoscroll");
    ASSERT_NE(control, parser.inputs.end());
    EXPECT_EQ(control->second.at("type").Get<Rml::String>(), "checkbox");
    EXPECT_EQ(control->second.at("checked").Get<Rml::String>(), "checked");
    EXPECT_EQ(control->second.at("data-action").Get<Rml::String>(), "log-autoscroll");
}

TEST_F(EditorWorkspaceAssets, LogClearIsAnExplicitIconCommand)
{
    const auto control = parser.buttons.find("log-clear");
    ASSERT_NE(control, parser.buttons.end());
    EXPECT_EQ(control->second.at("data-action").Get<Rml::String>(), "log-clear");
    EXPECT_EQ(control->second.at("class").Get<Rml::String>(), "browser-icon");
    EXPECT_FALSE(control->second.at("title").Get<Rml::String>().empty());
}

TEST_F(EditorWorkspaceAssets, LogViewsAndScriptPagingHaveExplicitCommands)
{
    for (const auto* id : {"log-output-tab", "log-scripts-tab", "log-scripts-prev", "log-scripts-next"})
    {
        const auto control = parser.buttons.find(id);
        ASSERT_NE(control, parser.buttons.end());
        EXPECT_FALSE(control->second.at("data-action").Get<Rml::String>().empty());
    }
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
