#pragma once

#include <Eigen/Geometry>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace VL::Debug
{
using Color = std::array<float, 4>;
using Handle = uint64_t;
enum class Shape { Point, Line, Polyline, Arrow, Circle, Sphere, Box, Capsule, Axes, Text };
enum class DepthMode { Overlay, DepthTest };

// 纯值绘制请求：世界坐标为厘米/Y-up，orientation 为局部到世界的单位四元数。
// duration=0 单个 GT 帧，-1 持续至清理，正值使用游戏秒；厚度和文字大小使用像素。
struct Style
{
    Color color{0, 1, 1, 1};
    float thickness = 2;
    float duration = 0;
    std::string category = "General";
    std::string owner;
    std::string publisher;
    std::string key;
    DepthMode depth = DepthMode::Overlay;
};

// 公共生产边界与 CLR 适配共用请求格式；不含 World、UI、CLR 或 GPU 引用。
struct Request
{
    Shape shape = Shape::Line;
    Style style;
    Eigen::Vector3f begin = Eigen::Vector3f::Zero(), end = Eigen::Vector3f::Zero();
    Eigen::Vector3f extents = Eigen::Vector3f::Ones();
    Eigen::Quaternionf orientation = Eigen::Quaternionf::Identity();
    float radius = 1, halfHeight = 1, size = 8;
    unsigned segments = 24;
    bool closed = false;
    std::vector<Eigen::Vector3f> points;
    std::string text;
};

// 展开后的冻结几何由显示适配器消费；颜色为线性 RGBA，Axes 固定红 X/绿 Y/蓝 Z。
struct Line { Eigen::Vector3f begin, end; Color color; float thickness; DepthMode depth = DepthMode::Overlay; };
struct Point { Eigen::Vector3f position; Color color; float size; DepthMode depth = DepthMode::Overlay; };
struct Text { Eigen::Vector3f position; Color color; float size; std::string value; DepthMode depth = DepthMode::Overlay; };
struct Geometry
{
    std::vector<Line> lines;
    std::vector<Point> points;
    std::vector<Text> texts;
};
struct Category { std::string name; bool enabled = true; size_t count = 0; };
struct Snapshot : Geometry
{
    bool enabled = true;
    size_t storedCount = 0, visibleCount = 0;
    std::vector<Category> categories;
    std::vector<std::string> owners;
    std::string ownerFilter;
};

void Validate(const Request& request);
Geometry BuildGeometry(const Request& request);

// World 独占的 GT 收集器。入口验证、生成和预算预检完成后才替换旧项；
// Freeze 深拷贝值供 GT/RT 使用，清理和后续提交不会修改已发布快照。
class DrawDebug
{
public:
    static constexpr size_t MaxItems = 1024, MaxLines = 16384, MaxPoints = 1024, MaxTexts = 128;
    Handle Submit(Request request);
    Handle DrawDebugPoint(const Eigen::Vector3f& position, float pixels = 8, Style style = {});
    Handle DrawDebugLine(const Eigen::Vector3f& begin, const Eigen::Vector3f& end, Style style = {});
    Handle DrawDebugPolyline(std::vector<Eigen::Vector3f> points, bool closed = false, Style style = {});
    Handle DrawDebugArrow(const Eigen::Vector3f& begin, const Eigen::Vector3f& end, float headLength = 10, Style style = {});
    Handle DrawDebugCircle(const Eigen::Vector3f& center, float radius,
        const Eigen::Quaternionf& orientation = Eigen::Quaternionf::Identity(), Style style = {}, unsigned segments = 24);
    Handle DrawDebugSphere(const Eigen::Vector3f& center, float radius, Style style = {}, unsigned segments = 24);
    Handle DrawDebugBox(const Eigen::Vector3f& center, const Eigen::Vector3f& halfExtents,
        const Eigen::Quaternionf& orientation = Eigen::Quaternionf::Identity(), Style style = {});
    Handle DrawDebugCapsule(const Eigen::Vector3f& center, float radius, float cylinderHalfHeight,
        const Eigen::Quaternionf& orientation = Eigen::Quaternionf::Identity(), Style style = {}, unsigned segments = 24);
    Handle DrawDebugCoordinateSystem(const Eigen::Vector3f& origin, float length = 50,
        const Eigen::Quaternionf& orientation = Eigen::Quaternionf::Identity(), Style style = {});
    Handle DrawDebugString(const Eigen::Vector3f& position, std::string text, float pixels = 14, Style style = {});
    void BeginFrame();
    // 完整 GT 帧结束后标记已显示请求；下一帧命令生产的新项不能被帧清理误删。
    void EndFrame() { completedRevision = revision; }
    void AdvanceGameTime(float seconds);
    bool Remove(Handle handle);
    void Clear();
    void ClearOwner(const std::string& owner);
    void ClearPublisher(const std::string& owner, const std::string& publisher);
    void ClearPublisherCategory(const std::string& owner, const std::string& publisher, const std::string& category);
    bool RemoveKey(const std::string& owner, const std::string& publisher, const std::string& category, const std::string& key);
    void ClearCategory(const std::string& category);
    void SetEnabled(bool value) { enabled = value; }
    void SetCategoryEnabled(const std::string& category, bool value);
    void SetOwnerFilter(std::string owner);
    std::shared_ptr<const Snapshot> Freeze() const;
    size_t Count() const { return items.size(); }
    static constexpr bool SupportsDepthTest() { return true; }

private:
    struct Item { Handle handle; Style style; double expires; Geometry geometry; uint64_t revision; };
    std::vector<Item> items;
    std::map<std::string, bool> categoryVisibility;
    std::string ownerFilter;
    double gameTime = 0;
    uint64_t revision = 0, completedRevision = 0;
    bool enabled = true;
};
}
