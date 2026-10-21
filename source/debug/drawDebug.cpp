#include "drawDebug.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <set>
#include <stdexcept>

namespace VL::Debug
{
namespace
{
constexpr float Pi = 3.14159265358979323846f;
// 身份不随 World/候选副本重置，旧 World 的句柄不能误删新 World 图形。
std::atomic<Handle> nextHandle{0};
void Require(bool condition, const char* message)
{
    if (!condition) throw std::invalid_argument(message);
}
void ValidateName(const std::string& text, size_t limit, bool empty)
{
    Require((empty || !text.empty()) && text.size() <= limit, "DrawDebug: invalid name length");
    for (unsigned char c : text) Require(c >= 32 && c != 127, "DrawDebug: control character in name");
}
bool ValidPosition(const Eigen::Vector3f& position)
{
    return position.allFinite() && position.cwiseAbs().maxCoeff() <= 1.0e7f;
}
void AddLine(Geometry& geometry, const Request& request, const Eigen::Vector3f& begin, const Eigen::Vector3f& end)
{
    geometry.lines.push_back({begin, end, request.style.color, request.style.thickness, request.style.depth});
}
Eigen::Vector3f WorldPoint(const Request& request, const Eigen::Vector3f& local)
{
    return request.begin + request.orientation * local;
}
void AddCircle(Geometry& geometry, const Request& request, int axis, float y = 0)
{
    for (unsigned index = 0; index < request.segments; ++index)
    {
        Eigen::Vector3f points[2];
        for (int end = 0; end < 2; ++end)
        {
            const float angle = (index + end) * 2 * Pi / request.segments;
            Eigen::Vector3f local = Eigen::Vector3f::Zero();
            local[axis] = y;
            local[(axis + 1) % 3] = request.radius * std::cos(angle);
            local[(axis + 2) % 3] = request.radius * std::sin(angle);
            points[end] = WorldPoint(request, local);
        }
        AddLine(geometry, request, points[0], points[1]);
    }
}
void AddArrow(Geometry& geometry, const Request& request)
{
    AddLine(geometry, request, request.begin, request.end);
    const Eigen::Vector3f direction = (request.end - request.begin).normalized();
    const Eigen::Vector3f side = direction.unitOrthogonal();
    const Eigen::Vector3f up = direction.cross(side);
    const Eigen::Vector3f base = request.end - direction * request.size;
    for (const Eigen::Vector3f offset : std::array<Eigen::Vector3f, 4>{
        side, Eigen::Vector3f(-side), up, Eigen::Vector3f(-up)})
        AddLine(geometry, request, request.end, base + offset * (request.size * 0.4f));
}
}

void Validate(const Request& r)
{
    const auto& s = r.style;
    Require(ValidPosition(r.begin) && ValidPosition(r.end), "DrawDebug: invalid position");
    Require(std::isfinite(s.thickness) && s.thickness >= 0.5f && s.thickness <= 32, "DrawDebug: thickness outside [0.5,32]");
    Require(std::isfinite(s.duration) && (s.duration == -1 || (s.duration >= 0 && s.duration <= 3600)),
        "DrawDebug: duration must be -1 or [0,3600]");
    for (float value : s.color) Require(std::isfinite(value) && value >= 0 && value <= 1, "DrawDebug: color outside [0,1]");
    ValidateName(s.category, 64, false); ValidateName(s.owner, 128, true);
    ValidateName(s.publisher, 128, true); ValidateName(s.key, 64, true);
    Require(s.depth == DepthMode::Overlay || s.depth == DepthMode::DepthTest, "DrawDebug: invalid depth mode");
    Require(r.orientation.coeffs().allFinite() && std::abs(r.orientation.squaredNorm() - 1) <= 1e-4f,
        "DrawDebug: orientation must be a unit quaternion");
    Require(r.segments >= 8 && r.segments <= 128 && r.segments % 4 == 0, "DrawDebug: segments must be a multiple of 4 in [8,128]");
    Require(r.points.size() <= 1024, "DrawDebug: polyline point budget exceeded");
    for (const auto& point : r.points) Require(ValidPosition(point), "DrawDebug: invalid polyline point");
    switch (r.shape)
    {
    case Shape::Polyline: Require(r.points.size() >= 2, "DrawDebug: polyline needs two points"); break;
    case Shape::Arrow:
        Require((r.end - r.begin).squaredNorm() > 1e-8f, "DrawDebug: arrow needs distinct endpoints");
        [[fallthrough]];
    case Shape::Point: case Shape::Axes:
        Require(std::isfinite(r.size) && r.size > 0 && r.size <= (r.shape == Shape::Point ? 128 : 1e6f), "DrawDebug: invalid size");
        break;
    case Shape::Text:
        Require(std::isfinite(r.size) && r.size >= 8 && r.size <= 48, "DrawDebug: text size outside [8,48]");
        Require(!r.text.empty() && r.text.size() <= 512 && r.text.find('\0') == std::string::npos, "DrawDebug: text needs 1..512 UTF-8 bytes without NUL");
        break;
    case Shape::Box:
        Require(r.extents.allFinite() && r.extents.minCoeff() > 0 && r.extents.maxCoeff() <= 1e6f, "DrawDebug: invalid box half extents");
        break;
    case Shape::Capsule:
        Require(std::isfinite(r.halfHeight) && r.halfHeight >= 0 && r.halfHeight <= 1e6f, "DrawDebug: invalid cylinder half height");
        [[fallthrough]];
    case Shape::Sphere: case Shape::Circle:
        Require(std::isfinite(r.radius) && r.radius > 0 && r.radius <= 1e6f, "DrawDebug: invalid radius"); break;
    case Shape::Line: break;
    default: throw std::invalid_argument("DrawDebug: unknown shape");
    }
}

Geometry BuildGeometry(const Request& r)
{
    Validate(r);
    Geometry geometry;
    switch (r.shape)
    {
    case Shape::Point: geometry.points.push_back({r.begin, r.style.color, r.size, r.style.depth}); break;
    case Shape::Text: geometry.texts.push_back({r.begin, r.style.color, r.size, r.text, r.style.depth}); break;
    case Shape::Line: AddLine(geometry, r, r.begin, r.end); break;
    case Shape::Polyline:
        for (size_t index = 1; index < r.points.size(); ++index) AddLine(geometry, r, r.points[index - 1], r.points[index]);
        if (r.closed) AddLine(geometry, r, r.points.back(), r.points.front());
        break;
    case Shape::Arrow: AddArrow(geometry, r); break;
    case Shape::Circle: AddCircle(geometry, r, 1); break;
    case Shape::Sphere:
        for (int axis = 0; axis < 3; ++axis) AddCircle(geometry, r, axis);
        break;
    case Shape::Box:
    {
        std::array<Eigen::Vector3f, 8> corners;
        for (int i = 0; i < 8; ++i)
            corners[i] = WorldPoint(r, {r.extents.x() * ((i & 1) ? 1 : -1),
                r.extents.y() * ((i & 2) ? 1 : -1), r.extents.z() * ((i & 4) ? 1 : -1)});
        for (int i = 0; i < 8; ++i)
            for (int bit : {1, 2, 4}) if (!(i & bit)) AddLine(geometry, r, corners[i], corners[i | bit]);
        break;
    }
    case Shape::Capsule:
        // halfHeight 只表示圆柱半高，球冠另加 radius；分离圆环/半球防止接缝斜线。
        if (r.halfHeight == 0)
        {
            for (int axis = 0; axis < 3; ++axis) AddCircle(geometry, r, axis);
            break;
        }
        for (float sign : {-1.f, 1.f})
        {
            AddCircle(geometry, r, 1, sign * r.halfHeight);
            for (int axis : {0, 2})
                for (unsigned i = 0; i < r.segments / 2; ++i)
                {
                    Eigen::Vector3f points[2];
                    for (int end = 0; end < 2; ++end)
                    {
                        const float angle = (i + end) * 2 * Pi / r.segments;
                        Eigen::Vector3f local(0, sign * (r.halfHeight + r.radius * std::sin(angle)), 0);
                        local[axis] = r.radius * std::cos(angle);
                        points[end] = WorldPoint(r, local);
                    }
                    AddLine(geometry, r, points[0], points[1]);
                }
        }
        for (Eigen::Vector3f offset : std::array<Eigen::Vector3f, 4>{
            Eigen::Vector3f(r.radius, 0, 0), Eigen::Vector3f(-r.radius, 0, 0),
            Eigen::Vector3f(0, 0, r.radius), Eigen::Vector3f(0, 0, -r.radius)})
            AddLine(geometry, r, WorldPoint(r, offset + Eigen::Vector3f(0, r.halfHeight, 0)),
                WorldPoint(r, offset - Eigen::Vector3f(0, r.halfHeight, 0)));
        break;
    case Shape::Axes:
        for (int axis = 0; axis < 3; ++axis)
        {
            auto arrow = r;
            arrow.end = WorldPoint(r, Eigen::Vector3f::Unit(axis) * r.size);
            arrow.size = r.size * 0.15f;
            arrow.style.color = {axis == 0 ? 1.f : 0.15f, axis == 1 ? 1.f : 0.15f, axis == 2 ? 1.f : 0.15f, r.style.color[3]};
            AddArrow(geometry, arrow);
        }
        break;
    }
    return geometry;
}

Handle DrawDebug::Submit(Request request)
{
    auto geometry = BuildGeometry(request);
    auto replace = items.end();
    if (!request.style.key.empty())
        for (auto it = items.begin(); it != items.end(); ++it)
            if (it->style.owner == request.style.owner && it->style.publisher == request.style.publisher &&
                it->style.category == request.style.category && it->style.key == request.style.key)
            { replace = it; break; }
    size_t lines = geometry.lines.size(), points = geometry.points.size(), texts = geometry.texts.size();
    std::set<std::string> categories{request.style.category};
    for (auto it = items.begin(); it != items.end(); ++it)
    {
        categories.insert(it->style.category);
        if (it == replace) continue;
        lines += it->geometry.lines.size(); points += it->geometry.points.size(); texts += it->geometry.texts.size();
    }
    Require((replace != items.end() || items.size() < MaxItems) &&
        lines <= MaxLines && points <= MaxPoints && texts <= MaxTexts && categories.size() <= 128, "DrawDebug: World geometry budget exceeded");
    const Handle handle = replace == items.end() ? nextHandle.fetch_add(1) + 1 : replace->handle;
    Item item{handle, std::move(request.style), 0, std::move(geometry), ++revision};
    item.expires = gameTime + item.style.duration;
    if (replace == items.end()) items.push_back(std::move(item)); else *replace = std::move(item);
    return handle;
}

Handle DrawDebug::DrawDebugPoint(const Eigen::Vector3f& p, float size, Style style)
{ Request r; r.shape = Shape::Point; r.begin = p; r.size = size; r.style = std::move(style); return Submit(std::move(r)); }
Handle DrawDebug::DrawDebugLine(const Eigen::Vector3f& a, const Eigen::Vector3f& b, Style style)
{ Request r; r.begin = a; r.end = b; r.style = std::move(style); return Submit(std::move(r)); }
Handle DrawDebug::DrawDebugPolyline(std::vector<Eigen::Vector3f> points, bool closed, Style style)
{ Request r; r.shape = Shape::Polyline; r.points = std::move(points); r.closed = closed; r.style = std::move(style); return Submit(std::move(r)); }
Handle DrawDebug::DrawDebugArrow(const Eigen::Vector3f& a, const Eigen::Vector3f& b, float head, Style style)
{ Request r; r.shape = Shape::Arrow; r.begin = a; r.end = b; r.size = head; r.style = std::move(style); return Submit(std::move(r)); }
Handle DrawDebug::DrawDebugCircle(const Eigen::Vector3f& p, float radius, const Eigen::Quaternionf& q, Style style, unsigned segments)
{ Request r; r.shape = Shape::Circle; r.begin = p; r.radius = radius; r.orientation = q; r.segments = segments; r.style = std::move(style); return Submit(std::move(r)); }
Handle DrawDebug::DrawDebugSphere(const Eigen::Vector3f& p, float radius, Style style, unsigned segments)
{ Request r; r.shape = Shape::Sphere; r.begin = p; r.radius = radius; r.segments = segments; r.style = std::move(style); return Submit(std::move(r)); }
Handle DrawDebug::DrawDebugBox(const Eigen::Vector3f& p, const Eigen::Vector3f& extents, const Eigen::Quaternionf& q, Style style)
{ Request r; r.shape = Shape::Box; r.begin = p; r.extents = extents; r.orientation = q; r.style = std::move(style); return Submit(std::move(r)); }
Handle DrawDebug::DrawDebugCapsule(const Eigen::Vector3f& p, float radius, float height, const Eigen::Quaternionf& q, Style style, unsigned segments)
{ Request r; r.shape = Shape::Capsule; r.begin = p; r.radius = radius; r.halfHeight = height; r.orientation = q; r.segments = segments; r.style = std::move(style); return Submit(std::move(r)); }
Handle DrawDebug::DrawDebugCoordinateSystem(const Eigen::Vector3f& p, float length, const Eigen::Quaternionf& q, Style style)
{ Request r; r.shape = Shape::Axes; r.begin = p; r.size = length; r.orientation = q; r.style = std::move(style); return Submit(std::move(r)); }
Handle DrawDebug::DrawDebugString(const Eigen::Vector3f& p, std::string text, float size, Style style)
{ Request r; r.shape = Shape::Text; r.begin = p; r.text = std::move(text); r.size = size; r.style = std::move(style); return Submit(std::move(r)); }

void DrawDebug::BeginFrame()
{
    items.erase(std::remove_if(items.begin(), items.end(),
        [this](const Item& item) { return item.style.duration == 0 && item.revision <= completedRevision; }), items.end());
}
void DrawDebug::AdvanceGameTime(float seconds)
{
    Require(std::isfinite(seconds) && seconds >= 0, "DrawDebug: invalid game delta");
    gameTime += seconds;
    items.erase(std::remove_if(items.begin(), items.end(),
        [this](const Item& item) { return item.style.duration > 0 && item.expires <= gameTime; }), items.end());
}
bool DrawDebug::Remove(Handle handle)
{
    for (auto it = items.begin(); it != items.end(); ++it)
        if (it->handle == handle) { items.erase(it); return true; }
    return false;
}
void DrawDebug::Clear() { items.clear(); }
void DrawDebug::ClearOwner(const std::string& owner)
{
    items.erase(std::remove_if(items.begin(), items.end(),
        [&owner](const Item& item) { return item.style.owner == owner; }), items.end());
}
void DrawDebug::ClearPublisher(const std::string& owner, const std::string& publisher)
{
    ClearPublisherCategory(owner, publisher, "");
}
void DrawDebug::ClearPublisherCategory(const std::string& owner, const std::string& publisher, const std::string& category)
{
    items.erase(std::remove_if(items.begin(), items.end(),
        [&owner, &publisher, &category](const Item& item) { return item.style.owner == owner &&
            item.style.publisher == publisher && (category.empty() || item.style.category == category); }), items.end());
}
bool DrawDebug::RemoveKey(const std::string& owner, const std::string& publisher, const std::string& category, const std::string& key)
{
    for (auto it = items.begin(); it != items.end(); ++it)
        if (it->style.owner == owner && it->style.publisher == publisher && it->style.category == category && it->style.key == key)
        { items.erase(it); return true; }
    return false;
}
void DrawDebug::ClearCategory(const std::string& category)
{
    items.erase(std::remove_if(items.begin(), items.end(),
        [&category](const Item& item) { return item.style.category == category; }), items.end());
}
void DrawDebug::SetCategoryEnabled(const std::string& category, bool value)
{
    ValidateName(category, 64, false);
    Require(categoryVisibility.count(category) || categoryVisibility.size() < 128, "DrawDebug: category filter budget exceeded");
    categoryVisibility[category] = value;
}
void DrawDebug::SetOwnerFilter(std::string owner)
{ ValidateName(owner, 128, true); ownerFilter = std::move(owner); }
std::shared_ptr<const Snapshot> DrawDebug::Freeze() const
{
    auto result = std::make_shared<Snapshot>();
    result->enabled = enabled; result->ownerFilter = ownerFilter; result->storedCount = items.size();
    std::map<std::string, Category> categories;
    for (const auto& entry : categoryVisibility) categories[entry.first] = {entry.first, entry.second, 0};
    std::set<std::string> owners;
    for (const auto& item : items)
    {
        auto found = categories.find(item.style.category);
        if (found == categories.end())
            found = categories.emplace(item.style.category, Category{item.style.category, true, 0}).first;
        ++found->second.count;
        owners.insert(item.style.owner);
        if (!enabled || !found->second.enabled || (!ownerFilter.empty() && item.style.owner != ownerFilter)) continue;
        ++result->visibleCount;
        result->lines.insert(result->lines.end(), item.geometry.lines.begin(), item.geometry.lines.end());
        result->points.insert(result->points.end(), item.geometry.points.begin(), item.geometry.points.end());
        result->texts.insert(result->texts.end(), item.geometry.texts.begin(), item.geometry.texts.end());
    }
    for (auto& entry : categories) result->categories.push_back(std::move(entry.second));
    result->owners.assign(owners.begin(), owners.end());
    return result;
}
}
