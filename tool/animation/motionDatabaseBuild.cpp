#include "motionDatabaseBuild.h"
#include "animation/motionRuntimeDatabase.h"

namespace VL::AnimationPreflight
{
nlohmann::json BuildMotionDatabaseReport(
    const std::filesystem::path& root, const std::string& source)
{
    return Animation::BuildMotionDatabaseReport(root, source);
}

nlohmann::json CaptureMotionDatabaseInputs(
    const std::filesystem::path& root, const std::string& source)
{
    return Animation::CaptureMotionDatabaseInputs(root, source);
}
}
