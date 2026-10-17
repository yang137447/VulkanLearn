using System.Text.Json;

namespace VulkanLearn;

public static partial class NativeBridge
{
    private static void ApplyContextTime(Context context, JsonElement value)
    {
        var time = context.Time;
        time.DeltaTime = value.GetProperty("deltaTime").GetSingle();
        time.UnscaledDeltaTime = value.GetProperty("unscaledDeltaTime").GetSingle();
        time.TimeScale = value.GetProperty("timeScale").GetSingle();
        time.TimeSinceStartup = value.GetProperty("timeSinceStartup").GetDouble();
        time.TimeSincePlay = value.GetProperty("timeSincePlay").GetDouble();
        time.UnscaledTimeSincePlay = value.GetProperty("unscaledTimeSincePlay").GetDouble();
        time.FrameIndex = value.GetProperty("frameIndex").GetUInt64();
        time.FixedFrameIndex = value.GetProperty("fixedFrameIndex").GetUInt64();
        time.IsFixedTick = false;
    }

    private static float BeginFrameTime(Context context, JsonElement request)
    {
        float dt = request.GetProperty("dt").GetSingle();
        if (!float.IsFinite(dt) || dt < 0) throw new ArgumentException("Invalid delta time.");
        if (request.TryGetProperty("time", out var time)) ApplyContextTime(context, time);
        else
        {
            // 直接 ABI 模块调用也使用相同的值语义；引擎生产路径始终发送完整原生时钟。
            context.Time.DeltaTime = dt;
            context.Time.UnscaledDeltaTime = dt;
            context.Time.TimeSincePlay += dt;
            context.Time.UnscaledTimeSincePlay += dt;
            context.Time.TimeSinceStartup += dt;
            context.Time.FrameIndex++;
        }
        return context.Time.DeltaTime;
    }

    private static object RunFixed(Context context, JsonElement request)
    {
        if (!context.Playing) throw new InvalidOperationException("Context is not playing.");
        Synchronize(context, request.GetProperty("states"));
        var time = context.Time;
        float delta = time.DeltaTime, unscaled = time.UnscaledDeltaTime;
        time.DeltaTime = time.FixedDeltaTime;
        time.UnscaledDeltaTime = time.TimeScale > 0 ? time.FixedDeltaTime / time.TimeScale : 0;
        time.FixedFrameIndex = request.GetProperty("fixedFrameIndex").GetUInt64();
        time.IsFixedTick = true;
        try
        {
            foreach (var instance in context.Instances)
            {
                Callback(context, instance, "FixedTick", time.FixedDeltaTime, EndPlayReason.Stopped);
                RunScriptComponents(context, instance, "FixedTick", time.FixedDeltaTime, EndPlayReason.Stopped);
            }
            DrainCommands(context);
            return Export(context, DeltaRequested(request));
        }
        finally
        {
            time.DeltaTime = delta;
            time.UnscaledDeltaTime = unscaled;
            time.IsFixedTick = false;
        }
    }
}
