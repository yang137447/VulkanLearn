using System.Numerics;
using System.Text;

namespace VulkanLearn;

public enum DebugDepthMode { Overlay, DepthTest }

// 每次调用复制样式；Duration: 0=单 GT 帧，-1=直到清理，正值=倍率后的游戏秒。
public sealed record DebugDrawStyle
{
    public Vector4 Color { get; init; } = new(0, 1, 1, 1);
    public float Thickness { get; init; } = 2;
    public float Duration { get; init; }
    public string Category { get; init; } = "General";
    public string Key { get; init; } = "";
    public DebugDepthMode Depth { get; init; } = DebugDepthMode.Overlay;
}

// 句柄绑定本次 Play 与当前根/逻辑组件发布者；不能跨 owner、组件或上下文删除。
public readonly struct DebugDrawHandle
{
    internal readonly ulong Context;
    internal readonly string Owner, Publisher, Category, Key;
    internal DebugDrawHandle(ulong context, string owner, string publisher, string category, string key)
        => (Context, Owner, Publisher, Category, Key) = (context, owner, publisher, category, key);
}

internal sealed record DebugDrawRequest(int Shape, Vector3 Begin, Vector3 End, Vector3 Extents,
    Quaternion Orientation, float Radius, float HalfHeight, float Size, int Segments, bool Closed,
    Vector3[] Points, string Text, DebugDrawStyle Style);

// 公共绘图生产接口。GT 回调只传世界空间值，不持有 UI、骨骼、原生指针或渲染资源。
public sealed class DebugDraw
{
    internal Func<DebugDrawRequest, DebugDrawHandle> Submit = null!;
    internal Action<DebugDrawHandle> RemoveRequest = null!;
    internal Action<string?> ClearRequest = null!;
    public static bool SupportsDepthTest => true;
    private DebugDrawHandle Draw(int shape, Vector3 begin, Vector3 end = default, Vector3 extents = default,
        Quaternion? orientation = null, float radius = 1, float halfHeight = 1, float size = 8,
        int segments = 24, bool closed = false, Vector3[]? points = null, string text = "", DebugDrawStyle? style = null)
        => Submit(new(shape, begin, end, extents, orientation ?? Quaternion.Identity, radius, halfHeight,
            size, segments, closed, points ?? [], text, style ?? new()));
    public DebugDrawHandle DrawDebugPoint(Vector3 position, float pixels = 8, DebugDrawStyle? style = null)
        => Draw(0, position, size: pixels, style: style);
    public DebugDrawHandle DrawDebugLine(Vector3 begin, Vector3 end, DebugDrawStyle? style = null)
        => Draw(1, begin, end, style: style);
    public DebugDrawHandle DrawDebugPolyline(IEnumerable<Vector3> points, bool closed = false, DebugDrawStyle? style = null)
        => Draw(2, default, points: points.Take(1025).ToArray(), closed: closed, style: style);
    public DebugDrawHandle DrawDebugArrow(Vector3 begin, Vector3 end, float headLength = 10, DebugDrawStyle? style = null)
        => Draw(3, begin, end, size: headLength, style: style);
    public DebugDrawHandle DrawDebugCircle(Vector3 center, float radius, Quaternion? orientation = null,
        DebugDrawStyle? style = null, int segments = 24)
        => Draw(4, center, orientation: orientation, radius: radius, style: style, segments: segments);
    public DebugDrawHandle DrawDebugSphere(Vector3 center, float radius, DebugDrawStyle? style = null, int segments = 24)
        => Draw(5, center, radius: radius, style: style, segments: segments);
    public DebugDrawHandle DrawDebugBox(Vector3 center, Vector3 halfExtents, Quaternion? orientation = null, DebugDrawStyle? style = null)
        => Draw(6, center, extents: halfExtents, orientation: orientation, style: style);
    public DebugDrawHandle DrawDebugCapsule(Vector3 center, float radius, float cylinderHalfHeight,
        Quaternion? orientation = null, DebugDrawStyle? style = null, int segments = 24)
        => Draw(7, center, orientation: orientation, radius: radius, halfHeight: cylinderHalfHeight, style: style, segments: segments);
    public DebugDrawHandle DrawDebugCoordinateSystem(Vector3 origin, float length = 50,
        Quaternion? orientation = null, DebugDrawStyle? style = null)
        => Draw(8, origin, orientation: orientation, size: length, style: style);
    public DebugDrawHandle DrawDebugString(Vector3 position, string text, float pixels = 14, DebugDrawStyle? style = null)
        => Draw(9, position, text: text, size: pixels, style: style);
    public void Remove(DebugDrawHandle handle) => RemoveRequest(handle);
    public void Clear(string? category = null) => ClearRequest(category);
}

public abstract partial class Blueprint
{
    public DebugDraw DebugDraw { get; internal set; } = new();
}

public static partial class NativeBridge
{
    private sealed record DebugMessage(Blueprint Owner, Component? Component, object Value, bool Cleanup = false, int Cost = 256);
    private sealed partial class GameplayScheduler
    {
        private readonly List<DebugMessage> debugMessages = [];
        private readonly HashSet<(Blueprint, Component?)> debugPublishers = [];
        private ulong nextDebug;
        private static string DebugPublisher(Component? component) => component == null ? "" : $"{component.Id}:{component.Handle.Generation}";
        private void RequireDebug(Blueprint owner)
        {
            Require(owner);
            if (callbackPhase is "" or "OnCreate" or "OnReload" or "EndPlay")
                throw new InvalidOperationException("DrawDebug requires an active gameplay callback.");
            DebugName("Script/" + owner.Handle.Id, 128, false);
            DebugName(DebugPublisher(callbackComponent), 128, true);
        }
        private void QueueDebug(Blueprint owner, object value, bool cleanup = false, Component? component = null, int cost = 256)
        {
            // 预留 JSON 向量/字符串最坏展开，避免大量 polyline 在原生读取前撑破共享 ABI 缓冲。
            if (!cleanup && (debugMessages.Count >= 512 || debugMessages.Sum(m => m.Cost) + cost > 512 * 1024))
                throw new InvalidOperationException("DrawDebug command budget exceeded.");
            var message = new DebugMessage(owner, component ?? callbackComponent, value, cleanup, cost);
            debugMessages.Add(message);
            if (!cleanup) undo.Add(() => debugMessages.Remove(message));
            if (AssetContext.InstanceIds.TryGetValue(owner.Handle.Id, out var instance)) instance.Dirty = true;
        }
        public void BindDebug(Blueprint owner)
        {
            owner.DebugDraw = new()
            {
                Submit = request =>
                {
                    RequireDebug(owner); ValidateDebug(request);
                    var identity = (owner, callbackComponent);
                    if (debugPublishers.Add(identity)) undo.Add(() => debugPublishers.Remove(identity));
                    string publisher = DebugPublisher(callbackComponent);
                    string key = request.Style.Key.Length == 0 ? $"~{++nextDebug}" : request.Style.Key;
                    var s = request.Style;
                    QueueDebug(owner, new { op = "draw", shape = request.Shape, publisher, key,
                        category = s.Category, color = new[] { s.Color.X, s.Color.Y, s.Color.Z, s.Color.W },
                        thickness = s.Thickness, duration = s.Duration, depth = (int)s.Depth,
                        begin = Values(request.Begin), end = Values(request.End),
                        extents = Values(request.Extents), orientation = new[] { request.Orientation.X, request.Orientation.Y,
                            request.Orientation.Z, request.Orientation.W }, radius = request.Radius, halfHeight = request.HalfHeight,
                        size = request.Size, segments = request.Segments, closed = request.Closed,
                        points = request.Points.Select(Values).ToArray(), text = request.Text },
                        cost: 2048 + request.Points.Length * 64 + Encoding.UTF8.GetByteCount(request.Text) * 6);
                    return new(owner.Handle.Context, owner.Handle.Id, publisher, s.Category, key);
                },
                RemoveRequest = handle =>
                {
                    RequireDebug(owner);
                    if (handle.Context != context.Id || handle.Owner != owner.Handle.Id || handle.Publisher != DebugPublisher(callbackComponent))
                        throw new InvalidOperationException("Stale or foreign DrawDebug handle.");
                    QueueDebug(owner, new { op = "remove", publisher = handle.Publisher, category = handle.Category, key = handle.Key });
                },
                ClearRequest = category =>
                {
                    RequireDebug(owner);
                    if (category != null) DebugName(category, 64, false);
                    QueueDebug(owner, new { op = "clear", publisher = DebugPublisher(callbackComponent), category });
                }
            };
        }
        private static void DebugName(string name, int bytes, bool empty)
        {
            if (name == null || !empty && name.Length == 0 || Encoding.UTF8.GetByteCount(name) > bytes ||
                name.Any(char.IsControl)) throw new ArgumentException("DrawDebug: invalid name.");
        }
        private static void ValidateDebug(DebugDrawRequest r)
        {
            var s = r.Style;
            DebugName(s.Category, 64, false); DebugName(s.Key, 64, true);
            if (s.Key.StartsWith('~')) throw new ArgumentException("DrawDebug: '~' keys are reserved.");
            static bool Position(Vector3 p) => Finite(p) && Vector3.Abs(p).X <= 1e7f && Vector3.Abs(p).Y <= 1e7f && Vector3.Abs(p).Z <= 1e7f;
            if (!Position(r.Begin) || !Position(r.End) || r.Points.Length > 1024 || r.Points.Any(p => !Position(p)) ||
                !float.IsFinite(s.Thickness) || s.Thickness < .5f || s.Thickness > 32 ||
                !float.IsFinite(s.Duration) || s.Duration != -1 && (s.Duration < 0 || s.Duration > 3600) ||
                new[] { s.Color.X, s.Color.Y, s.Color.Z, s.Color.W }.Any(v => !float.IsFinite(v) || v < 0 || v > 1) ||
                s.Depth is not (DebugDepthMode.Overlay or DebugDepthMode.DepthTest) || !float.IsFinite(r.Orientation.LengthSquared()) ||
                Math.Abs(r.Orientation.LengthSquared() - 1) > 1e-4f ||
                r.Segments < 8 || r.Segments > 128 || r.Segments % 4 != 0)
                throw new ArgumentException("DrawDebug: invalid geometry/style or unsupported depth mode.");
            if (r.Shape == 2 && r.Points.Length < 2 ||
                r.Shape == 3 && Vector3.DistanceSquared(r.Begin, r.End) <= 1e-8f ||
                r.Shape is 0 or 3 or 8 && (!float.IsFinite(r.Size) || r.Size <= 0 || r.Size > (r.Shape == 0 ? 128 : 1e6f)) ||
                r.Shape is 4 or 5 or 7 && (!float.IsFinite(r.Radius) || r.Radius <= 0 || r.Radius > 1e6f) ||
                r.Shape == 7 && (!float.IsFinite(r.HalfHeight) || r.HalfHeight < 0 || r.HalfHeight > 1e6f) ||
                r.Shape == 6 && (!Finite(r.Extents) || Math.Min(r.Extents.X, Math.Min(r.Extents.Y, r.Extents.Z)) <= 0 ||
                    Math.Max(r.Extents.X, Math.Max(r.Extents.Y, r.Extents.Z)) > 1e6f) ||
                r.Shape == 9 && (!float.IsFinite(r.Size) || r.Size < 8 || r.Size > 48 || string.IsNullOrEmpty(r.Text) ||
                    Encoding.UTF8.GetByteCount(r.Text) > 512 || r.Text.Contains('\0')))
                throw new ArgumentException("DrawDebug: invalid shape dimensions.");
        }
        public object[] TakeDebug(Blueprint owner)
        {
            if (debugMessages.Count == 0) return Array.Empty<object>();
            var values = debugMessages.Where(m => m.Owner == owner && (m.Cleanup ||
                owner.Enabled && !owner.PendingDestroy && (m.Component == null || m.Component.Enabled && !m.Component.Failed)))
                .Select(m => m.Value).ToArray();
            debugMessages.RemoveAll(m => m.Owner == owner);
            return values;
        }
        private void RemoveDebug(Blueprint owner, Component? component = null)
        {
            // EndPlay/外层调度可重复清理同一发布者，必须保留尚未交付的生命周期清理。
            debugMessages.RemoveAll(m => !m.Cleanup && m.Owner == owner && (component == null || m.Component == component));
            if (releasing) return;
            bool hadPublisher = debugPublishers.RemoveWhere(p => p.Item1 == owner && (component == null || p.Item2 == component)) != 0;
            if (!hadPublisher) return;
            // 清理消息不参与失败回调回滚；即使本次绘制失败也必须撤销已发布的旧图形。
            QueueDebug(owner, component == null ? new { op = "clearOwner", cleanup = true } :
                (object)new { op = "clear", publisher = DebugPublisher(component), category = (string?)null, cleanup = true }, true, component);
        }
    }
}
