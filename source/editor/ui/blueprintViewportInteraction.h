#pragma once

#include <array>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "editor/ui/editorActorVisualization.h"

namespace VL::EditorUi
{
// 仅消费已冻结的组件局部包围盒和完整父链矩阵；拾取返回稳定组件 ID，不访问 World/GPU。
// 包围盒是宽阶段近似，不宣称命中了模型三角形或读取了场景深度。
struct BlueprintViewportBounds
{
    std::string componentId;
    Eigen::Matrix4f worldFromLocal = Eigen::Matrix4f::Identity();
    Eigen::Vector3f minimum = Eigen::Vector3f::Zero();
    Eigen::Vector3f maximum = Eigen::Vector3f::Zero();
};

struct BlueprintViewportPick
{
    std::string componentId;
    float distance = 0;
};

enum class BlueprintGizmoMode { Translate, Rotate, Scale };

// 已校验作者草稿的非网格图示；线段和原点标记共用拾取口径，空 ID 表示 Blueprint(Self)。
struct BlueprintViewportHelper
{
    std::string componentId;
    Eigen::Vector2f marker = Eigen::Vector2f::Zero();
    bool markerVisible = false;
    std::vector<EditorActorVisualLine> lines;
};

struct BlueprintViewportGizmoAxis
{
    bool visible = false;
    Eigen::Vector2f begin = Eigen::Vector2f::Zero();
    Eigen::Vector2f end = Eigen::Vector2f::Zero();
    Eigen::Vector3f worldAxis = Eigen::Vector3f::Zero();
    float worldLength = 0;
    float parentAxisLength = 1;
    std::vector<EditorActorVisualLine> lines;
};

// 一次拖动冻结起始相机、局部 TRS 和父矩阵，输出新的局部作者值；不分解含 shear 的世界矩阵。
// Translate 沿父坐标轴，Rotate 修改 YXZ Euler 的对应分量，Scale 保持局部缩放符号。
struct BlueprintViewportGizmo
{
    bool valid = false;
    BlueprintGizmoMode mode = BlueprintGizmoMode::Translate;
    Eigen::Vector3f originWorld = Eigen::Vector3f::Zero();
    Eigen::Vector2f originPixel = Eigen::Vector2f::Zero();
    std::array<BlueprintViewportGizmoAxis, 3> axes;
    EditorSceneCameraActor camera;
    float width = 0;
    float height = 0;
    Eigen::Vector3f initialValue = Eigen::Vector3f::Zero();
};

namespace BlueprintViewportDetail
{
inline bool ValidCamera(const EditorSceneCameraActor& camera, float width, float height)
{
    const bool valid = std::isfinite(width) && std::isfinite(height) && width > 0 && height > 0 &&
        camera.position.allFinite() && camera.forward.allFinite() &&
        camera.right.allFinite() && camera.up.allFinite() &&
        std::isfinite(camera.horizontalFovDegrees) && camera.horizontalFovDegrees > 0 &&
        camera.horizontalFovDegrees < 179 && std::isfinite(camera.clipNear) &&
        std::isfinite(camera.clipFar) && camera.clipNear > 0 && camera.clipFar > camera.clipNear &&
        std::abs(camera.forward.squaredNorm() - 1) < 1e-3f &&
        std::abs(camera.right.squaredNorm() - 1) < 1e-3f &&
        std::abs(camera.up.squaredNorm() - 1) < 1e-3f &&
        std::abs(camera.forward.dot(camera.right)) < 1e-3f &&
        std::abs(camera.forward.dot(camera.up)) < 1e-3f &&
        std::abs(camera.right.dot(camera.up)) < 1e-3f;
    if (!valid) return false;
    const float focal = width * 0.5f / std::tan(camera.horizontalFovDegrees * 0.008726646259971648f);
    return std::isfinite(focal) && focal > 0;
}

inline float Focal(const EditorSceneCameraActor& camera, float width)
{
    return width * 0.5f / std::tan(camera.horizontalFovDegrees * 0.008726646259971648f);
}

inline Eigen::Vector3f RayDirection(const EditorSceneCameraActor& camera,
    float width, float height, const Eigen::Vector2f& pixel)
{
    const float focal = Focal(camera, width);
    // 不归一化：射线参数直接等于观察空间深度，near/far 可用于精确裁剪。
    return camera.forward + camera.right * ((pixel.x() - width * 0.5f) / focal) -
        camera.up * ((pixel.y() - height * 0.5f) / focal);
}

inline float SegmentDistance(const Eigen::Vector2f& point,
    const Eigen::Vector2f& begin, const Eigen::Vector2f& end)
{
    const Eigen::Vector2f delta = end - begin;
    if (delta.squaredNorm() < 1e-8f) return (point - begin).norm();
    const float fraction = std::clamp((point - begin).dot(delta) / delta.squaredNorm(), 0.0f, 1.0f);
    return (point - begin - delta * fraction).norm();
}

inline bool AxisParameter(const BlueprintViewportGizmo& gizmo, size_t axis,
    const Eigen::Vector2f& pixel, float& parameter)
{
    const Eigen::Vector3f ray = RayDirection(gizmo.camera, gizmo.width, gizmo.height, pixel);
    const auto& direction = gizmo.axes[axis].worldAxis;
    const Eigen::Vector3f offset = gizmo.camera.position - gizmo.originWorld;
    const float projection = direction.dot(ray);
    const float denominator = ray.squaredNorm() - projection * projection;
    if (denominator < ray.squaredNorm() * 1e-5f) return false;
    parameter = (ray.squaredNorm() * direction.dot(offset) -
        projection * ray.dot(offset)) / denominator;
    return std::isfinite(parameter);
}

inline bool PlaneDirection(const BlueprintViewportGizmo& gizmo, size_t axis,
    const Eigen::Vector2f& pixel, Eigen::Vector3f& direction)
{
    const Eigen::Vector3f ray = RayDirection(gizmo.camera, gizmo.width, gizmo.height, pixel);
    const auto& normal = gizmo.axes[axis].worldAxis;
    const float denominator = ray.dot(normal);
    if (std::abs(denominator) < ray.norm() * 1e-5f) return false;
    const float depth = (gizmo.originWorld - gizmo.camera.position).dot(normal) / denominator;
    if (depth < gizmo.camera.clipNear || depth > gizmo.camera.clipFar) return false;
    direction = gizmo.camera.position + ray * depth - gizmo.originWorld;
    const float length = direction.norm();
    if (!std::isfinite(length) || length < gizmo.axes[axis].worldLength * 0.02f) return false;
    direction /= length;
    return true;
}
} // namespace BlueprintViewportDetail

inline bool ProjectBlueprintViewportPoint(const EditorSceneCameraActor& camera,
    float width, float height, const Eigen::Vector3f& world, Eigen::Vector2f& pixel)
{
    if (!BlueprintViewportDetail::ValidCamera(camera, width, height) || !world.allFinite()) return false;
    return ProjectActorVisualPoint(camera, world, width, height, pixel) && pixel.allFinite();
}

inline std::optional<BlueprintViewportPick> PickBlueprintViewportComponent(
    const std::vector<BlueprintViewportBounds>& bounds, const EditorSceneCameraActor& camera,
    float width, float height, const Eigen::Vector2f& pixel)
{
    if (!BlueprintViewportDetail::ValidCamera(camera, width, height) || !pixel.allFinite() ||
        pixel.x() < 0 || pixel.y() < 0 || pixel.x() >= width || pixel.y() >= height) return {};
    const Eigen::Vector3d ray = BlueprintViewportDetail::RayDirection(camera, width, height, pixel).cast<double>();
    if (!ray.allFinite()) return {};
    double nearestDepth = camera.clipFar;
    std::optional<BlueprintViewportPick> hit;
    for (const auto& bound : bounds)
    {
        if (bound.componentId.empty() || !bound.worldFromLocal.allFinite() ||
            !bound.minimum.allFinite() || !bound.maximum.allFinite() ||
            (bound.minimum.array() > bound.maximum.array()).any() ||
            !bound.worldFromLocal.row(3).isApprox(Eigen::RowVector4f(0, 0, 0, 1))) continue;
        // 在局部空间做 slab，保留完整逆矩阵；负缩放、非均匀父缩放及 shear 都无需 TRS 分解。
        const Eigen::Matrix4d world = bound.worldFromLocal.cast<double>();
        const Eigen::FullPivLU<Eigen::Matrix4d> decomposition(world);
        if (!decomposition.isInvertible()) continue;
        const Eigen::Matrix4d inverse = decomposition.inverse();
        const Eigen::Vector3d origin = (inverse * camera.position.cast<double>().homogeneous()).head<3>();
        const Eigen::Vector3d direction = inverse.block<3, 3>(0, 0) * ray;
        double lower = camera.clipNear, upper = nearestDepth;
        bool intersects = true;
        for (int axis = 0; axis < 3; ++axis)
        {
            if (std::abs(direction[axis]) < 1e-12)
            {
                if (origin[axis] < bound.minimum[axis] || origin[axis] > bound.maximum[axis])
                    intersects = false;
                continue;
            }
            const double first = (bound.minimum[axis] - origin[axis]) / direction[axis];
            const double last = (bound.maximum[axis] - origin[axis]) / direction[axis];
            lower = std::max(lower, std::min(first, last));
            upper = std::min(upper, std::max(first, last));
            if (lower > upper) intersects = false;
        }
        if (!intersects || (hit && lower >= nearestDepth)) continue;
        nearestDepth = lower;
        hit = BlueprintViewportPick{bound.componentId, static_cast<float>(lower * ray.norm())};
    }
    return hit;
}

inline BlueprintViewportGizmo BuildBlueprintViewportGizmo(const EditorSceneCameraActor& camera,
    float width, float height, const Eigen::Matrix4f& parentWorld, const Eigen::Quaternionf& parentRotation,
    const Eigen::Vector3f& position, const Eigen::Vector3f& rotationDegrees, const Eigen::Vector3f& scale,
    BlueprintGizmoMode mode, float pixels = 80)
{
    BlueprintViewportGizmo result;
    if (!BlueprintViewportDetail::ValidCamera(camera, width, height) || !parentWorld.allFinite() ||
        !parentRotation.coeffs().allFinite() || std::abs(parentRotation.squaredNorm() - 1) > 1e-3f ||
        !position.allFinite() || !rotationDegrees.allFinite() || !scale.allFinite() ||
        !std::isfinite(pixels) || pixels < 8 ||
        !parentWorld.row(3).isApprox(Eigen::RowVector4f(0, 0, 0, 1))) return result;
    const Eigen::Matrix3f linear = parentWorld.block<3, 3>(0, 0);
    if (!linear.fullPivLu().isInvertible()) return result;
    result.originWorld = (parentWorld * position.homogeneous()).head<3>();
    if (!ProjectBlueprintViewportPoint(camera, width, height, result.originWorld, result.originPixel)) return result;
    result.valid = true;
    result.mode = mode;
    result.camera = camera;
    result.width = width;
    result.height = height;
    result.initialValue = mode == BlueprintGizmoMode::Translate ? position :
        mode == BlueprintGizmoMode::Rotate ? rotationDegrees : scale;
    const float worldLength = ActorVisualViewPoint(camera, result.originWorld).z() /
        BlueprintViewportDetail::Focal(camera, width) * pixels;
    if (!std::isfinite(worldLength) || worldLength <= 0)
    {
        result.valid = false;
        return result;
    }
    const Eigen::Vector3f radians = rotationDegrees * 0.017453292519943295f;
    const Eigen::Quaternionf y(Eigen::AngleAxisf(radians.y(), Eigen::Vector3f::UnitY()));
    const Eigen::Quaternionf x(Eigen::AngleAxisf(radians.x(), Eigen::Vector3f::UnitX()));
    const Eigen::Quaternionf rotation = y * x * Eigen::AngleAxisf(radians.z(), Eigen::Vector3f::UnitZ());
    for (size_t index = 0; index < 3; ++index)
    {
        auto& axis = result.axes[index];
        axis.begin = result.originPixel;
        axis.worldLength = worldLength;
        const Eigen::Vector3f unit = Eigen::Vector3f::Unit(static_cast<int>(index));
        Eigen::Vector3f direction;
        if (mode == BlueprintGizmoMode::Translate) direction = linear * unit;
        else if (mode == BlueprintGizmoMode::Scale) direction = linear * (rotation * unit);
        else
        {
            // YXZ 的三条 Euler 编辑轴不是同一个旋转后基：X 先乘 Y，Z 再乘 YX。
            // 朝向只合成父旋转，与现有相机/灯光语义一致；位置仍使用含 shear 的父矩阵。
            if (index == 0) direction = parentRotation * (y * unit);
            else if (index == 1) direction = parentRotation * unit;
            else direction = parentRotation * (y * (x * unit));
        }
        axis.parentAxisLength = direction.norm();
        if (!std::isfinite(axis.parentAxisLength) || axis.parentAxisLength < 1e-8f) continue;
        axis.worldAxis = direction / axis.parentAxisLength;
        if (mode == BlueprintGizmoMode::Rotate)
        {
            const Eigen::Vector3f sight = (result.originWorld - camera.position).normalized();
            // 近乎侧向的环投影成直线，平面反交会放大噪声；隐藏而不是产生跳变角度。
            if (std::abs(sight.dot(axis.worldAxis)) < 0.12f) continue;
            const Eigen::Vector3f right = axis.worldAxis.unitOrthogonal();
            const Eigen::Vector3f up = axis.worldAxis.cross(right);
            for (int segment = 0; segment < 48; ++segment)
            {
                const float a = segment * 6.28318530718f / 48;
                const float b = (segment + 1) * 6.28318530718f / 48;
                AddActorVisualLine(axis.lines, camera,
                    result.originWorld + worldLength * (right * std::cos(a) + up * std::sin(a)),
                    result.originWorld + worldLength * (right * std::cos(b) + up * std::sin(b)), width, height);
            }
        }
        else
        {
            const Eigen::Vector3f endpoint = result.originWorld + axis.worldAxis * worldLength;
            if (!ProjectBlueprintViewportPoint(camera, width, height, endpoint, axis.end)) continue;
            if ((axis.end - axis.begin).norm() < 8) continue;
            axis.lines.push_back({axis.begin, axis.end});
        }
        axis.visible = !axis.lines.empty();
    }
    return result;
}

inline std::optional<size_t> HitBlueprintViewportGizmo(const BlueprintViewportGizmo& gizmo,
    const Eigen::Vector2f& pixel, float tolerance = 7)
{
    if (!gizmo.valid || !pixel.allFinite() || !std::isfinite(tolerance) || tolerance <= 0) return {};
    float nearest = tolerance;
    std::optional<size_t> hit;
    for (size_t index = 0; index < gizmo.axes.size(); ++index)
    {
        const auto& axis = gizmo.axes[index];
        if (!axis.visible) continue;
        for (const auto& line : axis.lines)
        {
            // 平移/缩放中心不抢组件选择；三轴在此重合，无法表达明确轴意图。
            if (gizmo.mode != BlueprintGizmoMode::Rotate && (pixel - gizmo.originPixel).norm() < 10) continue;
            const float distance = BlueprintViewportDetail::SegmentDistance(pixel, line.begin, line.end);
            if (distance >= nearest) continue;
            nearest = distance;
            hit = index;
        }
    }
    return hit;
}

inline std::vector<BlueprintViewportHelper> BuildBlueprintViewportHelpers(const nlohmann::json& draft,
    const EditorSceneCameraActor& camera, float width, float height)
{
    std::vector<BlueprintViewportHelper> helpers;
    if (!BlueprintViewportDetail::ValidCamera(camera, width, height)) return helpers;
    const auto nodes = draft.value("components", nlohmann::json::array());
    for (size_t index = 0; index < nodes.size(); ++index)
    {
        const auto& node = nodes[index];
        const std::string type = node.at("type");
        if (type == "mesh") continue;
        BlueprintViewportHelper helper;
        helper.componentId = ScriptComponentId(node);
        const Eigen::Vector3f position = ScriptComponentMatrix(draft, index).block<3, 1>(0, 3);
        helper.markerVisible = ProjectBlueprintViewportPoint(camera, width, height, position, helper.marker);
        EditorSceneActorSnapshot actor;
        const auto rotation = ScriptComponentRotation(draft, index);
        actor.light.position = position;
        actor.light.direction = rotation * -Eigen::Vector3f::UnitZ();
        if (type == "camera")
        {
            actor.type = EditorSceneActorType::Camera;
            actor.camera.position = position;
            actor.camera.right = rotation * Eigen::Vector3f::UnitX();
            actor.camera.up = rotation * Eigen::Vector3f::UnitY();
            actor.camera.forward = actor.light.direction;
            actor.camera.horizontalFovDegrees = node.value("fov", 60.0f);
            actor.camera.clipNear = node.value("near_clip", 1.0f);
            actor.camera.clipFar = node.value("far_clip", 10000.0f);
        }
        else if (type == "scene")
        {
            if (helper.markerVisible)
            {
                helper.lines.push_back({helper.marker - Eigen::Vector2f(6, 0), helper.marker + Eigen::Vector2f(6, 0)});
                helper.lines.push_back({helper.marker - Eigen::Vector2f(0, 6), helper.marker + Eigen::Vector2f(0, 6)});
            }
        }
        else
        {
            actor.type = type == "pointLight" ? EditorSceneActorType::PointLight :
                type == "spotLight" ? EditorSceneActorType::SpotLight : EditorSceneActorType::DirectionalLight;
            actor.light.coneAngleInner = node.value("coneAngleInner", 30.0f) * 0.01745329252f;
            actor.light.coneAngleOuter = node.value("coneAngleOuter", 45.0f) * 0.01745329252f;
        }
        if (type != "scene") helper.lines = BuildActorVisualLines(actor, camera, width, height);
        // 固定像素原点标记让极小/极远的非网格组件仍可选中，不伪造世界尺寸碰撞盒。
        if (helper.markerVisible)
        {
            const auto p = helper.marker;
            helper.lines.push_back({p + Eigen::Vector2f(0, -5), p + Eigen::Vector2f(5, 0)});
            helper.lines.push_back({p + Eigen::Vector2f(5, 0), p + Eigen::Vector2f(0, 5)});
            helper.lines.push_back({p + Eigen::Vector2f(0, 5), p + Eigen::Vector2f(-5, 0)});
            helper.lines.push_back({p + Eigen::Vector2f(-5, 0), p + Eigen::Vector2f(0, -5)});
        }
        helpers.push_back(std::move(helper));
    }
    if (draft.contains("physics"))
    {
        const auto& physics = draft.at("physics");
        if (physics.value("shape", "box") == "compound")
        {
            auto shapes = physics.value("shapes", nlohmann::json::array());
            if (!physics.contains("shapes"))
                for (const auto& node : nodes)
                    if (node.contains("collision"))
                    {
                        auto shape = node.at("collision");
                        shape["component"] = ScriptComponentId(node);
                        shapes.push_back(std::move(shape));
                    }
            for (const auto& shape : shapes)
                for (size_t index = 0; index < nodes.size(); ++index)
                    if (ScriptComponentId(nodes[index]) == shape.at("component"))
                    {
                        EditorSceneActorSnapshot actor;
                        actor.type = EditorSceneActorType::Script;
                        const Eigen::Matrix4f matrix = ScriptComponentMatrix(draft, index);
                        actor.position = matrix.block<3, 1>(0, 3);
                        actor.rotation = ScriptComponentRotation(draft, index).toRotationMatrix()
                            .eulerAngles(1, 0, 2) * 57.29577951308232f;
                        std::swap(actor.rotation.x(), actor.rotation.y());
                        actor.scale = Eigen::Vector3f::Constant(matrix.block<3, 1>(0, 0).norm());
                        actor.scriptPhysics = shape;
                        BlueprintViewportHelper helper;
                        helper.componentId = ScriptComponentId(nodes[index]);
                        helper.lines = BuildActorVisualLines(actor, camera, width, height);
                        helpers.push_back(std::move(helper));
                    }
            return helpers;
        }
        EditorSceneActorSnapshot actor;
        actor.type = EditorSceneActorType::Script;
        actor.position = ScriptVector(draft, "position");
        actor.rotation = ScriptVector(draft, "rotation");
        actor.scale = ScriptVector(draft, "scale", 1);
        actor.scriptPhysics = draft.at("physics");
        BlueprintViewportHelper helper;
        helper.lines = BuildActorVisualLines(actor, camera, width, height);
        helpers.push_back(std::move(helper));
    }
    return helpers;
}

inline std::optional<std::string> HitBlueprintViewportHelper(const std::vector<BlueprintViewportHelper>& helpers,
    const Eigen::Vector2f& pixel, float tolerance = 7)
{
    if (!pixel.allFinite() || !std::isfinite(tolerance) || tolerance <= 0) return {};
    std::optional<std::string> hit;
    float nearest = tolerance;
    for (const auto& helper : helpers)
    {
        float distance = helper.markerVisible ? (pixel - helper.marker).norm() :
            std::numeric_limits<float>::infinity();
        for (const auto& line : helper.lines)
            distance = std::min(distance, BlueprintViewportDetail::SegmentDistance(pixel, line.begin, line.end));
        if (distance < nearest)
        {
            nearest = distance;
            hit = helper.componentId;
        }
    }
    return hit;
}

inline bool SnapBlueprintViewportValue(BlueprintGizmoMode mode, size_t axis, const Eigen::Vector3f& initial,
    const Eigen::Vector3f& input, float step, Eigen::Vector3f& output)
{
    if (axis >= 3 || !initial.allFinite() || !input.allFinite() || !std::isfinite(step) || step <= 0) return false;
    Eigen::Vector3f value = input;
    if (mode == BlueprintGizmoMode::Scale)
    {
        if (std::abs(initial[axis]) < 1e-6f) return false;
        const float ratio = input[axis] / initial[axis];
        // 缩放步长是相对起始值的倍率增量；绝不舍入到零或翻转镜像符号。
        const float snapped = 1 + std::round((ratio - 1) / step) * step;
        if (snapped <= 0 || std::abs(initial[axis] * snapped) < 1e-6f) return false;
        value[axis] = initial[axis] * snapped;
    }
    else value[axis] = initial[axis] + std::round((input[axis] - initial[axis]) / step) * step;
    if (!value.allFinite()) return false;
    output = value;
    return true;
}

inline bool DragBlueprintViewportGizmo(const BlueprintViewportGizmo& gizmo, size_t axis,
    const Eigen::Vector2f& startPixel, const Eigen::Vector2f& currentPixel, Eigen::Vector3f& value,
    float previousRotationDegrees = std::numeric_limits<float>::quiet_NaN())
{
    if (!gizmo.valid || axis >= gizmo.axes.size() || !gizmo.axes[axis].visible ||
        !startPixel.allFinite() || !currentPixel.allFinite()) return false;
    Eigen::Vector3f candidate = gizmo.initialValue;
    if (gizmo.mode == BlueprintGizmoMode::Rotate)
    {
        Eigen::Vector3f start, current;
        if (!BlueprintViewportDetail::PlaneDirection(gizmo, axis, startPixel, start) ||
            !BlueprintViewportDetail::PlaneDirection(gizmo, axis, currentPixel, current)) return false;
        const float angle = std::atan2(gizmo.axes[axis].worldAxis.dot(start.cross(current)),
            start.dot(current));
        candidate[axis] += angle * 57.29577951308232f;
        // atan2 只返回一圈；宿主传上一帧已接受角度后可连续跨过 +/-180，并保留多圈拖动。
        if (std::isfinite(previousRotationDegrees))
            candidate[axis] += 360 * std::round((previousRotationDegrees - candidate[axis]) / 360);
    }
    else
    {
        float start = 0, current = 0;
        if (!BlueprintViewportDetail::AxisParameter(gizmo, axis, startPixel, start) ||
            !BlueprintViewportDetail::AxisParameter(gizmo, axis, currentPixel, current)) return false;
        if (gizmo.mode == BlueprintGizmoMode::Translate)
            candidate[axis] += (current - start) / gizmo.axes[axis].parentAxisLength;
        else
        {
            // 指数比例保留负缩放且不会跨零；拒绝极端手势，避免写入无效作者数据。
            const float exponent = (current - start) / gizmo.axes[axis].worldLength;
            if (std::abs(exponent) > 12) return false;
            candidate[axis] *= std::exp(exponent);
            if (std::abs(candidate[axis]) < 1e-6f) return false;
        }
    }
    if (!candidate.allFinite()) return false;
    value = candidate;
    return true;
}
} // namespace VL::EditorUi
