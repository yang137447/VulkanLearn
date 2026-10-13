#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "editor/ui/editorSceneActorTypes.h"

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
    if (width <= 0.0f || height <= 0.0f) return false;
    const Eigen::Vector3f local = ActorVisualViewPoint(view, point);
    if (local.z() < view.clipNear || local.z() > view.clipFar) return false;
    pixel = ActorVisualScreenPoint(view, local, width, height);
    return pixel.x() >= 0 && pixel.x() < width && pixel.y() >= 0 && pixel.y() < height;
}

inline void AddActorVisualLine(std::vector<EditorActorVisualLine>& lines,
    const EditorSceneCameraActor& view, Eigen::Vector3f begin, Eigen::Vector3f end,
    float width, float height)
{
    if (width <= 0.0f || height <= 0.0f) return;
    begin = ActorVisualViewPoint(view, begin);
    end = ActorVisualViewPoint(view, end);
    // 先裁近/远平面再透视除法：跨过眼点的锥边不能变成巨大或倒向的屏幕线段。
    float lower = 0.0f, upper = 1.0f;
    const Eigen::Vector3f delta = end - begin;
    if (std::abs(delta.z()) < 1.0e-8f)
    {
        if (begin.z() < view.clipNear || begin.z() > view.clipFar) return;
    }
    else
    {
        const float a = (view.clipNear - begin.z()) / delta.z();
        const float b = (view.clipFar - begin.z()) / delta.z();
        lower = std::max(lower, std::min(a, b));
        upper = std::min(upper, std::max(a, b));
        if (lower > upper) return;
    }
    Eigen::Vector2f first = ActorVisualScreenPoint(view, begin + delta * lower, width, height);
    Eigen::Vector2f last = ActorVisualScreenPoint(view, begin + delta * upper, width, height);
    const Eigen::Vector2f screenDelta = last - first;
    lower = 0.0f;
    upper = 1.0f;
    for (int axis = 0; axis < 2; ++axis)
    {
        const float extent = axis == 0 ? width : height;
        if (std::abs(screenDelta[axis]) < 1.0e-8f)
        {
            if (first[axis] < 0.0f || first[axis] > extent) return;
            continue;
        }
        const float a = -first[axis] / screenDelta[axis];
        const float b = (extent - first[axis]) / screenDelta[axis];
        lower = std::max(lower, std::min(a, b));
        upper = std::min(upper, std::max(a, b));
        if (lower > upper) return;
    }
    lines.push_back({first + screenDelta * lower, first + screenDelta * upper});
}

inline std::vector<EditorActorVisualLine> BuildActorVisualLines(
    const EditorSceneActorSnapshot& actor, const EditorSceneCameraActor& view,
    float width, float height)
{
    std::vector<EditorActorVisualLine> lines;
    if (width <= 0.0f || height <= 0.0f) return lines;
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
