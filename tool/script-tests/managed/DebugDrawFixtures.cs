using System.Numerics;
using VulkanLearn;

namespace ScriptFixtures;

public sealed class DebugDrawProbe : Blueprint
{
    [Expose(InstanceEditable = true)] public int Mode { get; set; }
    private DebugDrawHandle saved;
    private static DebugDraw? oldService;
    private readonly DebugDrawStyle persistent = new() { Duration = -1, Key = "shared", Category = "Physics" };
    protected override void OnCreate()
    {
        if (Mode == 10) DebugDraw.DrawDebugPoint(Vector3.Zero);
    }
    protected override void BeginPlay()
    {
        if (Mode == 11)
        {
            bool rejected = Task.Run(() =>
            {
                try { DebugDraw.DrawDebugPoint(Vector3.Zero); return false; }
                catch (InvalidOperationException) { return true; }
            }).GetAwaiter().GetResult();
            if (rejected) Transform.Position = Vector3.UnitX;
            return;
        }
        if (Mode == 12) { oldService = DebugDraw; return; }
        if (Mode == 13)
        {
            try { oldService!.DrawDebugPoint(Vector3.Zero); }
            catch (InvalidOperationException) { Transform.Position = Vector3.UnitX; }
            finally { oldService = null; }
            return;
        }
        if (Mode == 14)
        {
            try { DebugDraw.DrawDebugSphere(Vector3.Zero, -1); }
            catch (ArgumentException) { Transform.Position = Vector3.UnitX; }
            return;
        }
        if (Mode == 15)
        {
            DebugDraw.DrawDebugPoint(Vector3.Zero, style: persistent);
            throw new InvalidOperationException("rollback-begin-debug");
        }
        saved = DebugDraw.DrawDebugPoint(Vector3.Zero, style: persistent);
        DebugDraw.DrawDebugString(new(0, 10, 0), "timed", style: new() { Duration = .5f });
        if (Mode == 16)
        {
            DebugDraw.DrawDebugLine(Vector3.Zero, Vector3.One);
            DebugDraw.DrawDebugPolyline([Vector3.Zero, Vector3.One]);
            DebugDraw.DrawDebugArrow(Vector3.Zero, Vector3.One);
            DebugDraw.DrawDebugCircle(Vector3.Zero, 10);
            DebugDraw.DrawDebugSphere(Vector3.Zero, 10);
            DebugDraw.DrawDebugBox(Vector3.Zero, Vector3.One);
            DebugDraw.DrawDebugCapsule(Vector3.Zero, 10, 20);
            DebugDraw.DrawDebugCoordinateSystem(Vector3.Zero);
        }
    }
    protected override void Tick(float dt)
    {
        switch (Mode)
        {
            case 1: DebugDraw.DrawDebugPoint(Vector3.UnitX, style: persistent); break;
            case 2: DebugDraw.Remove(saved); break;
            case 3: DebugDraw.Clear("Physics"); break;
            case 4: Destroy(); break;
            case 5:
                DebugDraw.Clear(); DebugDraw.DrawDebugPoint(Vector3.One, style: persistent);
                throw new InvalidOperationException("rollback-tick-debug");
            case 6: GetScriptComponent("first").Enabled = false; break;
            case 7: RemoveScriptComponent(GetScriptComponent("first").Handle); break;
            case 8:
                try { DebugDraw.DrawDebugPoint(Vector3.One, style: new() { Depth = (DebugDepthMode)2 }); }
                catch (ArgumentException) { Transform.Position = Vector3.UnitX; }
                break;
            case 17:
                // 每帧绘制，端点读取本帧 FrameIndex；不再存在 fixed 回调专用帧号。
                DebugDraw.DrawDebugLine(Vector3.Zero, new(Time.FrameIndex, 1, 0));
                break;
            case 20:
                DebugDraw.DrawDebugPoint(Vector3.One, style: new() { Depth = DebugDepthMode.DepthTest });
                DebugDraw.DrawDebugLine(Vector3.Zero, Vector3.One, new() { Depth = DebugDepthMode.DepthTest });
                DebugDraw.DrawDebugString(Vector3.One, "depth", style: new() { Depth = DebugDepthMode.DepthTest });
                break;
            case 9:
                try
                {
                    DebugDraw.DrawDebugPoint(Vector3.One, style: persistent with { Key = "last-valid" });
                    DebugDraw.DrawDebugPoint(Vector3.Zero, float.NaN);
                }
                catch (ArgumentException) { throw new InvalidOperationException("discard-entire-callback-debug"); }
                break;
            case 19: Enabled = false; break;
        }
    }
    protected override void OnReload()
    {
        if (Mode == 18) DebugDraw.DrawDebugPoint(Vector3.One);
    }
}
public sealed class DebugDrawProbeComponent : Component
{
    [Expose(InstanceEditable = true)] public bool Fail { get; set; }
    protected override void BeginPlay() =>
        DebugDraw.DrawDebugPoint(new(20, 0, 0), style: new() { Duration = -1, Key = "shared", Category = "Physics" });
    protected override void Tick(float dt)
    {
        if (!Fail) return;
        DebugDraw.Clear();
        throw new InvalidOperationException("component-debug-failure");
    }
}
