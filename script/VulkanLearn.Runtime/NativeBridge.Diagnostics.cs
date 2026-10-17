using System.Diagnostics;

namespace VulkanLearn;

public static partial class NativeBridge
{
    private sealed class CallbackTiming
    {
        public ulong Calls;
        public double Last, Total, Maximum;
        public void Add(long started)
        {
            Last = Stopwatch.GetElapsedTime(started).TotalMilliseconds;
            ++Calls;
            Total += Last;
            Maximum = Math.Max(Maximum, Last);
        }
    }

    private sealed class ComponentMetric(string type)
    {
        public string Type = type, Lifecycle = "Created", LastException = "";
        public bool Enabled = true, Failed;
        public readonly Dictionary<string, CallbackTiming> Timings = new(StringComparer.Ordinal);
    }

    private sealed partial class Instance
    {
        public readonly Dictionary<string, CallbackTiming> Timings = new(StringComparer.Ordinal);
        public readonly Dictionary<string, ComponentMetric> ComponentMetrics = new(StringComparer.Ordinal);
        public string Lifecycle = "Created", LastException = "";
    }

    private static long BeginCallbackTiming(Instance instance, string phase) => Stopwatch.GetTimestamp();
    private static void EndCallbackTiming(Instance instance, string phase, long started)
    {
        if (!instance.Timings.TryGetValue(phase, out var timing))
            instance.Timings.Add(phase, timing = new());
        timing.Add(started);
        instance.Lifecycle = instance.Failed ? "Failed" : phase == "EndPlay" ? "Ended" :
            instance.Blueprint.PendingDestroy ? "Destroyed" : instance.Started ? "Playing" : "Created";
    }
    private static void RecordCallbackException(Instance instance, string phase, Exception error)
    {
        // 只保留值文本，不让性能台账持有异常/委托而阻碍 Gameplay 加载上下文回收。
        instance.LastException = Unwrap(error).ToString();
        if (instance.LastException.Length > 4096) instance.LastException = instance.LastException[..4096];
    }
    private static void RunCreate(Context context, Instance instance, Action create)
    {
        long started = BeginCallbackTiming(instance, "OnCreate");
        try { create(); }
        catch (Exception error) { RecordCallbackException(instance, "OnCreate", error); throw; }
        finally { EndCallbackTiming(instance, "OnCreate", started); }
    }
    private static void RecordComponentTiming(Instance instance, string componentId, string type,
        string phase, long started, Exception? error = null)
    {
        if (!instance.ComponentMetrics.TryGetValue(componentId, out var metric))
        {
            if (instance.ComponentMetrics.Count >= 128) instance.ComponentMetrics.Remove(instance.ComponentMetrics.Keys.First());
            instance.ComponentMetrics.Add(componentId, metric = new(type));
        }
        if (!metric.Timings.TryGetValue(phase, out var timing))
            metric.Timings.Add(phase, timing = new());
        timing.Add(started);
        if (error != null) RecordComponentState(instance, componentId, type, phase, metric.Enabled, true, error);
    }
    private static void RecordComponentState(Instance instance, string id, string type, string phase,
        bool enabled, bool failed, Exception? error = null)
    {
        if (!instance.ComponentMetrics.TryGetValue(id, out var metric))
        {
            if (instance.ComponentMetrics.Count >= 128) instance.ComponentMetrics.Remove(instance.ComponentMetrics.Keys.First());
            instance.ComponentMetrics.Add(id, metric = new(type));
        }
        metric.Enabled = enabled;
        metric.Failed = failed;
        metric.Lifecycle = failed ? "Failed" : phase == "EndPlay" ? "Ended" : phase == "OnCreate" ? "Created" : "Playing";
        if (error != null)
        {
            metric.LastException = Unwrap(error).ToString();
            if (metric.LastException.Length > 4096) metric.LastException = metric.LastException[..4096];
        }
    }
    private static object[] TimingValues(Dictionary<string, CallbackTiming> timings) =>
        timings.OrderBy(p => p.Key, StringComparer.Ordinal).Select(p => (object)new
        { phase = p.Key, calls = p.Value.Calls, lastMs = p.Value.Last, totalMs = p.Value.Total, maxMs = p.Value.Maximum }).ToArray();

    private static object RuntimeSnapshot(Context context, bool details = true)
    {
        bool includeDetails = details || context.DetailsDirty;
        var snapshot = new Dictionary<string, object>
        {
            ["blueprintCount"] = context.Instances.Count,
            ["pending"] = GetServiceCounts(context),
            ["assembly"] = new { generation = Assemblies.Single(p => p.Value == context.Assembly).Key,
                loaded = Assemblies.Count, retiredAlive = RetiredAssemblies.Count(r => r.IsAlive), contexts = Contexts.Count },
            ["gc"] = new { generation0 = GC.CollectionCount(0), generation1 = GC.CollectionCount(1),
                generation2 = GC.CollectionCount(2), heapBytes = GC.GetTotalMemory(false),
                allocatedBytes = GC.GetAllocatedBytesForCurrentThread() }
        };
        if (!includeDetails) return snapshot;
        // ABI 与 UI 都有界；按稳定作者顺序截取并报告总数，不把超大性能结果当脚本失败。
        var blueprints = new List<object>();
        int componentBudget = 128;
        bool truncated = context.Instances.Count > 128;
        foreach (var instance in context.Instances.Take(128))
        {
            int count = Math.Min(componentBudget, Math.Min(32, instance.ComponentMetrics.Count));
            componentBudget -= count;
            truncated |= count < instance.ComponentMetrics.Count;
            blueprints.Add(new
            {
                id = instance.Blueprint.Handle.Id, name = instance.Blueprint.Name,
                type = instance.Blueprint.GetType().FullName, enabled = instance.Blueprint.Enabled,
                failed = instance.Failed, destroyed = instance.Blueprint.PendingDestroy,
                lifecycle = instance.Lifecycle, lastException = instance.LastException,
                callbacks = TimingValues(instance.Timings),
                componentCount = instance.ComponentMetrics.Count,
                components = instance.ComponentMetrics.Take(count).Select(p => new
                {
                    id = p.Key, type = p.Value.Type, enabled = p.Value.Enabled, failed = p.Value.Failed,
                    lifecycle = p.Value.Lifecycle, lastException = p.Value.LastException, callbacks = TimingValues(p.Value.Timings)
                }).ToArray()
            });
        }
        snapshot["blueprints"] = blueprints;
        snapshot["truncated"] = truncated;
        snapshot["detailsFrame"] = context.Time.FrameIndex;
        context.DetailsDirty = false;
        return snapshot;
    }
}
