#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

#include "particlePanelStringSnapshot.h"
#include "particlePanelStringSnapshotData.h"

#ifndef VL_PROJECT_SOURCE_DIR
#error "editor_ui_tests must define VL_PROJECT_SOURCE_DIR"
#endif

namespace
{
using namespace VL::EditorUi::ParticlePanelSnapshot;
namespace FrozenData = VL::EditorUi::ParticlePanelSnapshot::Frozen;

SourceSet LoadPanel()
{
    return Load(std::filesystem::path(VL_PROJECT_SOURCE_DIR) /
        "source" / "editor" / "ui" / "panel");
}

std::vector<std::string> MakeFrozen(const char* const* values, size_t count)
{
    return std::vector<std::string>(values, values + count);
}

void ExpectSameMultiset(const std::vector<std::string>& actual,
    const std::vector<std::string>& expected, const char* label)
{
    EXPECT_EQ(actual.size(), expected.size()) << label << " count changed";
    if (actual == expected) return;

    size_t reported = 0;
    size_t index = 0;
    size_t other = 0;
    while (index < actual.size() && other < expected.size() && reported < 20)
    {
        if (actual[index] == expected[other])
        {
            ++index;
            ++other;
            continue;
        }
        if (actual[index] < expected[other])
        {
            ADD_FAILURE() << label << " gained: " << actual[index];
            ++index;
        }
        else
        {
            ADD_FAILURE() << label << " lost: " << expected[other];
            ++other;
        }
        ++reported;
    }
    for (; index < actual.size() && reported < 20; ++index, ++reported)
        ADD_FAILURE() << label << " gained: " << actual[index];
    for (; other < expected.size() && reported < 20; ++other, ++reported)
        ADD_FAILURE() << label << " lost: " << expected[other];
}
}

TEST(ParticlePanelStrings, SourceFileSet)
{
    const SourceSet panel = LoadPanel();
    ExpectSameMultiset(panel.files,
        MakeFrozen(FrozenData::kExpectedFiles,
            std::size(FrozenData::kExpectedFiles)),
        "panel source file");
}

TEST(ParticlePanelStrings, VisibleLiterals)
{
    const SourceSet panel = LoadPanel();
    ExpectSameMultiset(panel.literals,
        MakeFrozen(FrozenData::kExpectedLiterals,
            std::size(FrozenData::kExpectedLiterals)),
        "string literal");
}

TEST(ParticlePanelStrings, RmlIds)
{
    const SourceSet panel = LoadPanel();
    ExpectSameMultiset(panel.rmlIds,
        MakeFrozen(FrozenData::kExpectedRmlIds,
            std::size(FrozenData::kExpectedRmlIds)),
        "rml id");
}

TEST(ParticlePanelStrings, RmlIdReferences)
{
    const SourceSet panel = LoadPanel();
    ExpectSameMultiset(panel.idReferences,
        MakeFrozen(FrozenData::kExpectedIdReferences,
            std::size(FrozenData::kExpectedIdReferences)),
        "rml id reference");
}

TEST(ParticlePanelStrings, ActionNames)
{
    const SourceSet panel = LoadPanel();
    ExpectSameMultiset(panel.actions,
        MakeFrozen(FrozenData::kExpectedActions,
            std::size(FrozenData::kExpectedActions)),
        "action");
}

TEST(ParticlePanelStrings, DataAttributes)
{
    const SourceSet panel = LoadPanel();
    ExpectSameMultiset(panel.dataAttributes,
        MakeFrozen(FrozenData::kExpectedDataAttributes,
            std::size(FrozenData::kExpectedDataAttributes)),
        "data attribute");
}
