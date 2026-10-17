#include <gtest/gtest.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/SystemInterface.h>
#include "ui/gameplayHudView.h"
#include "ui/rmlUiInterfaces.h"

namespace
{
using namespace VL;

TEST(GameplayHudView, EditingHidesValuesAndPausedViewRetainsFrozenText)
{
    ScriptRuntimeView scripts;
    scripts.available = true;
    scripts.hudLines = {{"owner", "", "health", "Health: 100"}, {"owner", "weapon", "ammo", "Ammo: 3"}};
    EXPECT_TRUE(BuildGameplayHudText(scripts).empty());
    scripts.state = ScriptPlayState::Playing;
    EXPECT_EQ(BuildGameplayHudText(scripts), "Health: 100\nAmmo: 3");
    const auto frozen = scripts;
    scripts.hudLines[1].text = "Ammo: 2";
    EXPECT_EQ(BuildGameplayHudText(frozen), "Health: 100\nAmmo: 3");
    scripts.state = ScriptPlayState::Paused;
    EXPECT_EQ(BuildGameplayHudText(scripts), "Health: 100\nAmmo: 2");
    scripts.available = false;
    EXPECT_TRUE(BuildGameplayHudText(scripts).empty());
}

class GameplayHudSystem final : public Rml::SystemInterface
{
public:
    double GetElapsedTime() override { return 0; }
};

class GameplayHudBindingTest : public testing::Test
{
protected:
    void SetUp() override
    {
        Rml::SetSystemInterface(&system);
        Rml::SetRenderInterface(&renderer);
        ASSERT_TRUE(Rml::Initialise());
        context = Rml::CreateContext("gameplay-hud-test", {960, 540}, &renderer);
        ASSERT_NE(context, nullptr);
    }
    void TearDown() override
    {
        Rml::Shutdown();
        Rml::SetRenderInterface(nullptr);
        Rml::SetSystemInterface(nullptr);
    }
    GameplayHudSystem system;
    RmlUiRenderInterface renderer;
    Rml::Context* context = nullptr;
};

TEST_F(GameplayHudBindingTest, RuntimeDocumentDisplaysLiteralTextAndUpdatesWithoutWorldLabelChanges)
{
    Rml::String world = "SC_hud", gameplay = "Ammo: <b>3</b> & ready";
    auto model = context->CreateDataModel("game_ui");
    ASSERT_TRUE(model.Bind("world_text", &world));
    ASSERT_TRUE(model.Bind("gameplay_text", &gameplay));
    auto* document = context->LoadDocument(std::string(VL_EDITOR_ASSET_ROOT) + "/runtime-hud.rml");
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    auto* hud = document->GetElementById("gameplay-hud");
    ASSERT_NE(hud, nullptr);
    EXPECT_EQ(hud->QuerySelector("b"), nullptr);
    EXPECT_NE(hud->GetInnerRML().find("&lt;b&gt;3&lt;/b&gt;"), std::string::npos);
    gameplay = "Ammo: 2";
    model.GetModelHandle().DirtyVariable("gameplay_text");
    context->Update();
    EXPECT_EQ(hud->GetInnerRML(), "Ammo: 2");
    gameplay.clear();
    model.GetModelHandle().DirtyVariable("gameplay_text");
    context->Update();
    EXPECT_FALSE(hud->IsVisible());
}
}
