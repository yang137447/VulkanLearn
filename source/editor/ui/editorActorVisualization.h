#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "editor/ui/editorSceneActorTypes.h"
#include "script/scriptComponents.h"
#include "debug/debugProjection.h"

namespace VL::EditorUi
{

// 只消费冻结的相机/actor 值；输出 UI 像素线段，没有 World/GPU 引用。
// 辅助线是始终可见的编辑叠层，不宣称参与场景深度测试。
struct EditorActorVisualLine
{
    Eigen::Vector2f begin;
    Eigen::Vector2f end;
};

inline Eigen::Vector3f ActorVisualViewPoint(const EditorSceneCameraActor& view,
    const Eigen::Vector3f& point)
{
    const Eigen::Vector3f delta = point - view.position;
    return {delta.dot(view.right), delta.dot(view.up), delta.dot(view.forward)};
}

inline Eigen::Vector2f ActorVisualScreenPoint(const EditorSceneCameraActor& view,
    const Eigen::Vector3f& point, float width, float height)
{
    const float focal = width * 0.5f /
        std::tan(view.horizontalFovDegrees * 0.008726646259971648f);
    // VL 世界 +Y 向上，UI 像素 +Y 向下；使用水平 FOV，focal 两轴相等。
    return {width * 0.5f + point.x() * focal / point.z(),
        height * 0.5f - point.y() * focal / point.z()};
}

inline bool ProjectActorVisualPoint(const EditorSceneCameraActor& view,
    const Eigen::Vector3f& point, float width, float height, Eigen::Vector2f& pixel)
{
    const Debug::Camera camera{view.position, view.forward, view.right, view.up,
        view.horizontalFovDegrees, view.clipNear, view.clipFar};
    return Debug::ProjectPoint(camera, point, width, height, pixel);
}

inline void AddActorVisualLine(std::vector<EditorActorVisualLine>& lines,
    const EditorSceneCameraActor& view, Eigen::Vector3f begin, Eigen::Vector3f end,
    float width, float height)
{
    const Debug::Camera camera{view.position, view.forward, view.right, view.up,
        view.horizontalFovDegrees, view.clipNear, view.clipFar};
    Debug::ScreenLine result;
    if (Debug::ProjectLine(camera, begin, end, width, height, result)) lines.push_back({result.begin, result.end});
}

inline std::vector<EditorActorVisualLine> BuildActorVisualLines(
    const EditorSceneActorSnapshot& actor, const EditorSceneCameraActor& view,
    float width, float height)
{
    std::vector<EditorActorVisualLine> lines;
    if (width <= 0.0f || height <= 0.0f) return lines;
    if (actor.type == EditorSceneActorType::Script && !actor.scriptPhysics.is_null())
    {
        const auto& physics = actor.scriptPhysics;
        const auto rotation = ScriptRotation({{"rotation", {actor.rotation.x(), actor.rotation.y(), actor.rotation.z()}}});
        const Eigen::Matrix3f basis = rotation.toRotationMatrix() * actor.scale.x();
        const auto shape = physics.value("shape", "box");
        if (shape == "box")
        {
            const auto extents = ScriptVector(physics, "halfExtents");
            std::array<Eigen::Vector3f, 8> corners;
            for (int index = 0; index < 8; ++index)
                corners[index] = actor.position + basis * Eigen::Vector3f(
                    (index & 1) ? extents.x() : -extents.x(), (index & 2) ? extents.y() : -extents.y(),
                    (index & 4) ? extents.z() : -extents.z());
            for (int index = 0; index < 8; ++index)
                for (int bit : {1, 2, 4})
                    if (!(index & bit)) AddActorVisualLine(lines, view, corners[index], corners[index | bit], width, height);
        }
        else if (shape == "sphere" || shape == "capsule")
        {
            const float radius = physics.at("radius");
            const float halfHeight = shape == "capsule" ? physics.at("halfHeight").get<float>() : 0;
            // 球用三个大圆；胶囊显式分开圆环与半球，避免跨接缝产生斜线。
            if (halfHeight == 0)
            for (int plane = 0; plane < 3; ++plane)
                for (int index = 0; index < 32; ++index)
                {
                    Eigen::Vector3f endpoints[2];
                    for (int end = 0; end < 2; ++end)
                    {
                        const float angle = (index + end) * 6.28318530718f / 32;
                        Eigen::Vector3f point = Eigen::Vector3f::Zero();
                        point[(plane + 1) % 3] = radius * std::cos(angle);
                        point[(plane + 2) % 3] = radius * std::sin(angle);
                        endpoints[end] = actor.position + basis * point;
                    }
                    AddActorVisualLine(lines, view, endpoints[0], endpoints[1], width, height);
                }
            if (halfHeight > 0)
            {
                for (float sign : {-1.0f, 1.0f})
                {
                    for (int index = 0; index < 24; ++index)
                    {
                        const float first = index * 6.28318530718f / 24;
                        const float last = (index + 1) * 6.28318530718f / 24;
                        AddActorVisualLine(lines, view,
                            actor.position + basis * Eigen::Vector3f(radius * std::cos(first), sign * halfHeight, radius * std::sin(first)),
                            actor.position + basis * Eigen::Vector3f(radius * std::cos(last), sign * halfHeight, radius * std::sin(last)), width, height);
                    }
                    for (int axis : {0, 2})
                        for (int index = 0; index < 12; ++index)
                        {
                            Eigen::Vector3f endpoints[2];
                            for (int end = 0; end < 2; ++end)
                            {
                                const float angle = (index + end) * 3.14159265359f / 12;
                                Eigen::Vector3f point(0, sign * (halfHeight + radius * std::sin(angle)), 0);
                                point[axis] = radius * std::cos(angle);
                                endpoints[end] = actor.position + basis * point;
                            }
                            AddActorVisualLine(lines, view, endpoints[0], endpoints[1], width, height);
                        }
                }
                for (const Eigen::Vector3f offset : std::array<Eigen::Vector3f, 4>{
                    Eigen::Vector3f(radius, 0, 0), Eigen::Vector3f(-radius, 0, 0),
                    Eigen::Vector3f(0, 0, radius), Eigen::Vector3f(0, 0, -radius)})
                    AddActorVisualLine(lines, view, actor.position + basis * (offset + Eigen::Vector3f(0, halfHeight, 0)),
                        actor.position + basis * (offset - Eigen::Vector3f(0, halfHeight, 0)), width, height);
            }
        }
        return lines;
    }
    const Eigen::Vector3f origin = actor.type == EditorSceneActorType::Camera
        ? actor.camera.position : actor.light.position;
    if (actor.type == EditorSceneActorType::PointLight)
    {
        // 小圆仅标识灯具位置，不使用尚未参与光照的旧 radius 冒充衰减球。
        for (int plane = 0; plane < 3; ++plane)
            for (int index = 0; index < 24; ++index)
            {
                const float a = index * 6.28318530718f / 24.0f;
                const float b = (index + 1) * 6.28318530718f / 24.0f;
                Eigen::Vector3f first = Eigen::Vector3f::Zero(), last = Eigen::Vector3f::Zero();
                first[(plane + 1) % 3] = std::cos(a) * 25.0f;
                first[(plane + 2) % 3] = std::sin(a) * 25.0f;
                last[(plane + 1) % 3] = std::cos(b) * 25.0f;
                last[(plane + 2) % 3] = std::sin(b) * 25.0f;
                AddActorVisualLine(lines, view, origin + first, origin + last, width, height);
            }
        return lines;
    }
    if (actor.type == EditorSceneActorType::DirectionalLight)
    {
        const Eigen::Vector3f end = origin + actor.light.direction * 150.0f;
        AddActorVisualLine(lines, view, origin, end, width, height);
        const Eigen::Vector3f side = actor.light.direction.unitOrthogonal() * 20.0f;
        const Eigen::Vector3f back = end - actor.light.direction * 35.0f;
        AddActorVisualLine(lines, view, end, back + side, width, height);
        AddActorVisualLine(lines, view, end, back - side, width, height);
        return lines;
    }
    if (actor.type == EditorSceneActorType::SpotLight)
    {
        const Eigen::Vector3f forward = actor.light.direction;
        const Eigen::Vector3f right = forward.unitOrthogonal();
        const Eigen::Vector3f up = forward.cross(right);
        // 200 cm 是图示长度，不是光照截止距离；角度使用真实 inner/outer 半锥角。
        const Eigen::Vector3f center = origin + forward * 200.0f;
        for (float angle : {actor.light.coneAngleInner, actor.light.coneAngleOuter})
        {
            const float radius = std::tan(angle) * 200.0f;
            for (int index = 0; index < 32; ++index)
            {
                const float a = index * 6.28318530718f / 32.0f;
                const float b = (index + 1) * 6.28318530718f / 32.0f;
                const Eigen::Vector3f first = center + radius * (right * std::cos(a) + up * std::sin(a));
                const Eigen::Vector3f last = center + radius * (right * std::cos(b) + up * std::sin(b));
                AddActorVisualLine(lines, view, first, last, width, height);
                if (index % 8 == 0) AddActorVisualLine(lines, view, origin, first, width, height);
            }
        }
        return lines;
    }
    if (actor.type != EditorSceneActorType::Camera && actor.type != EditorSceneActorType::SquareAreaLight)
        return lines;
    std::array<Eigen::Vector3f, 4> corners;
    if (actor.type == EditorSceneActorType::Camera)
    {
        // 图示视锥截面不延伸到任意大的 far plane；形状严格使用 Camera 的水平 FOV。
        const float depth = std::min(actor.camera.clipFar, std::max(200.0f, actor.camera.clipNear * 1.1f));
        const float halfWidth = depth * std::tan(actor.camera.horizontalFovDegrees * 0.008726646259971648f);
        const float halfHeight = halfWidth * height / width;
        const Eigen::Vector3f center = origin + actor.camera.forward * depth;
        corners = {center - actor.camera.right * halfWidth - actor.camera.up * halfHeight,
            center + actor.camera.right * halfWidth - actor.camera.up * halfHeight,
            center + actor.camera.right * halfWidth + actor.camera.up * halfHeight,
            center - actor.camera.right * halfWidth + actor.camera.up * halfHeight};
        for (const auto& corner : corners)
            AddActorVisualLine(lines, view, origin, corner, width, height);
    }
    else
    {
        const Eigen::Vector3f right = actor.light.direction.unitOrthogonal() * actor.light.squareSize * 0.5f;
        const Eigen::Vector3f up = actor.light.direction.cross(right);
        corners = {origin - right - up, origin + right - up, origin + right + up, origin - right + up};
        AddActorVisualLine(lines, view, origin, origin + actor.light.direction * 100.0f, width, height);
    }
    for (size_t index = 0; index < corners.size(); ++index)
        AddActorVisualLine(lines, view, corners[index], corners[(index + 1) % corners.size()], width, height);
    return lines;
}
} // namespace VL::EditorUi
