#pragma once

#include "../../../animation/animationAssets.h"

struct aiScene;

namespace VL::Animation
{
// 将 Assimp 的作者场景一次性校验为 CPU 骨架/蒙皮和 ozz 片段；不复用静态网格的单位/轴向猜测。
// Scene 入口与文件入口共用实现，调用后结果不再引用 Assimp 内存；只支持固定 UE FBX cm 导出策略。
class AssimpSkeletalImporter
{
public:
    static SkeletonMesh ReadMesh(const std::filesystem::path& path, const std::string& root);
    static SkeletonMesh BuildMesh(const aiScene& scene, const std::string& root);
    static Clip ReadClip(
        const std::filesystem::path& path, const SkeletonMesh& mesh, const std::string& id);
    static Clip BuildClip(const aiScene& scene, const SkeletonMesh& mesh, const std::string& id);
};
}
