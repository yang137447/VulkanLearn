using System.Reflection;
using System.Runtime.CompilerServices;

namespace VulkanLearn;

public static partial class NativeBridge
{
    [Flags]
    private enum FrameCallbacks { None = 0, Tick = 1, LateTick = 2 }
    private sealed record FrameCallbackMask(FrameCallbacks Value);

    // 弱键缓存不能保活 Gameplay 类型；值只保留位标记，允许旧程序集正常卸载。
    private static readonly ConditionalWeakTable<Type, FrameCallbackMask> FrameCallbackMasks = new();

    private static bool HasFrameCallback(Type type, string phase)
    {
        FrameCallbacks callback = phase switch
        {
            "Tick" => FrameCallbacks.Tick,
            "LateTick" => FrameCallbacks.LateTick,
            _ => FrameCallbacks.None
        };
        return callback == FrameCallbacks.None ||
            (FrameCallbackMasks.GetValue(type, DiscoverFrameCallbacks).Value & callback) != 0;
    }

    private static FrameCallbackMask DiscoverFrameCallbacks(Type type)
    {
        Type root = typeof(Component).IsAssignableFrom(type) ? typeof(Component) : typeof(Blueprint);
        const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.DeclaredOnly;
        MethodInfo tick = root.GetMethod("Tick", flags)!;
        MethodInfo lateTick = root.GetMethod("LateTick", flags)!;
        FrameCallbacks callbacks = FrameCallbacks.None;
        // new 同名方法不会替换虚函数槽；逐层检查，保留祖先类已经提供的 override。
        for (Type? current = type; current != null && current != root; current = current.BaseType)
            foreach (MethodInfo method in current.GetMethods(flags))
            {
                if (!method.IsVirtual) continue;
                MethodInfo definition = method.GetBaseDefinition();
                if (definition.Equals(tick)) callbacks |= FrameCallbacks.Tick;
                else if (definition.Equals(lateTick)) callbacks |= FrameCallbacks.LateTick;
            }
        return new(callbacks);
    }
}
