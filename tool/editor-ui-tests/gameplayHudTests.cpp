#include <gtest/gtest.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/SystemInterface.h>
#include <filesystem>
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
        // 布局断言需要真实字形宽度；没有字体时 RmlUi 会把所有文本量成 0。
        const std::filesystem::path font("C:/Windows/Fonts/segoeui.ttf");
        if (!std::filesystem::exists(font))
            GTEST_SKIP() << "Segoe UI is unavailable.";
        ASSERT_TRUE(Rml::LoadFontFace(font.string()));
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

TEST_F(GameplayHudBindingTest, HudCardHugsWorldNameAndWrapsLongGameplayText)
{
    Rml::String world = "SC_tressfx_female_skinned", gameplay;
    auto model = context->CreateDataModel("game_ui");
    ASSERT_TRUE(model.Bind("world_text", &world));
    ASSERT_TRUE(model.Bind("gameplay_text", &gameplay));
    auto* document = context->LoadDocument(std::string(VL_EDITOR_ASSET_ROOT) + "/runtime-hud.rml");
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();

    auto* hud = document->GetElementById("hud");
    auto* panel = document->GetElementById("hud-panel");
    auto* gameplayHud = document->GetElementById("gameplay-hud");
    ASSERT_NE(hud, nullptr);
    ASSERT_NE(panel, nullptr);
    ASSERT_NE(gameplayHud, nullptr);

    // 单行世界名：卡片高度就是一行行高。百分比 max-width 若落回卡片自身，
    // RmlUi 会把宽度按 0 解析，世界名变成每字一行（高度 = 字数 × 行高）。
    const float lineHeight = hud->GetLineHeight();
    EXPECT_NEAR(panel->GetBox().GetSize(Rml::BoxArea::Content).y, lineHeight, 0.5f);
    // 外层宽度是可解析的确定值，卡片上限由它决定。
    EXPECT_NEAR(hud->GetBox().GetSize(Rml::BoxArea::Content).x, 960.f * 0.75f, 1.f);

    gameplay = "Health: 100\nAmmo: 3\nThis is a deliberately long gameplay status line that has to wrap inside the hud.";
    model.GetModelHandle().DirtyVariable("gameplay_text");
    context->Update();

    // 长文本在 75% 上限处换行：卡片不超过外层内容宽，且高度超过一行。
    EXPECT_LE(panel->GetBox().GetSize(Rml::BoxArea::Content).x,
        hud->GetBox().GetSize(Rml::BoxArea::Content).x + 0.5f);
    EXPECT_GT(gameplayHud->GetBox().GetSize(Rml::BoxArea::Content).y, lineHeight);
}
}
