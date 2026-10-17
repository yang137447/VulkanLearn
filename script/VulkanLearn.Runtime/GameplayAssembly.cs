using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.Loader;
using System.Diagnostics.CodeAnalysis;

namespace VulkanLearn;

public static partial class NativeBridge
{
    // 桥接/API 常驻，Gameplay 及托管依赖按代隔离；共享 Blueprint 类型身份才能跨上下文绑定。
    private sealed class GameplayLoadContext(string path) : AssemblyLoadContext(isCollectible: true)
    {
        private readonly AssemblyDependencyResolver resolver = new(path);
        protected override Assembly? Load(AssemblyName name)
        {
            if (name.Name == typeof(Blueprint).Assembly.GetName().Name) return typeof(Blueprint).Assembly;
            string? dependency = resolver.ResolveAssemblyToPath(name);
            return dependency == null ? null : ReadAssembly(dependency);
        }
        public Assembly ReadAssembly(string path)
        {
            // 读取字节后立即关闭文件，不让编辑器占用正在被编译器替换的 DLL/PDB。
            using var image = new MemoryStream(File.ReadAllBytes(path));
            string symbols = Path.ChangeExtension(path, ".pdb");
            using var pdb = File.Exists(symbols) ? new MemoryStream(File.ReadAllBytes(symbols)) : null;
            return LoadFromStream(image, pdb);
        }
    }
    private sealed record GameplayAssembly(GameplayLoadContext Loader, Dictionary<string, Type> Types,
        Dictionary<string, Type> ComponentTypes);
    private static readonly Dictionary<ulong, GameplayAssembly> Assemblies = [];
    private static readonly List<WeakReference> RetiredAssemblies = [];
    private static ulong nextAssembly;

    private static string NormalizeGameplayTypeIdentity(string value) =>
        string.Join(",", value.Split(',').Select(part => part.Trim()));

    private static bool TryResolveGameplayType(Dictionary<string, Type> types, string requested,
        [NotNullWhen(true)] out Type? type)
    {
        string identity = NormalizeGameplayTypeIdentity(requested);
        int separator = identity.IndexOf(',');
        string name = separator < 0 ? identity : identity[..separator];
        if (types.TryGetValue(name, out type))
        {
            // 只接受本代已注册 Type 的三种真实身份，不调用 Type.GetType 或加载外部程序集。
            if (identity == name || identity == name + "," + type.Assembly.GetName().Name ||
                identity == NormalizeGameplayTypeIdentity(type.AssemblyQualifiedName!)) return true;
        }
        type = null;
        return false;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static object LoadGameplay(string path)
    {
        path = Path.GetFullPath(path);
        var loader = new GameplayLoadContext(path);
        try
        {
            var assembly = loader.ReadAssembly(path);
            var types = assembly.GetTypes().Where(t => t.IsPublic && !t.IsAbstract &&
                typeof(Blueprint).IsAssignableFrom(t) && t.GetConstructor(Type.EmptyTypes) != null)
                .ToDictionary(t => t.FullName!, StringComparer.Ordinal);
            var componentTypes = assembly.GetTypes().Where(t => t.IsPublic && !t.IsAbstract &&
                typeof(Component).IsAssignableFrom(t) && t.GetConstructor(Type.EmptyTypes) != null)
                .ToDictionary(t => t.FullName!, StringComparer.Ordinal);
            // 默认属性 getter/构造异常也属于候选失败；类目录完整成功才登记该代。
            var classes = types.OrderBy(p => p.Key, StringComparer.Ordinal).Select(p => new
                { name = p.Key, assemblyName = p.Value.Assembly.GetName().Name,
                    assemblyQualifiedName = p.Value.AssemblyQualifiedName,
                    properties = DescribeProperties(p.Value) }).ToArray();
            var componentClasses = componentTypes.OrderBy(p => p.Key, StringComparer.Ordinal).Select(p => new
                { name = p.Key, assemblyName = p.Value.Assembly.GetName().Name,
                    assemblyQualifiedName = p.Value.AssemblyQualifiedName,
                    properties = DescribeProperties(p.Value) }).ToArray();
            if (System.Text.Json.JsonSerializer.SerializeToUtf8Bytes(new { classes, componentClasses }).Length > 3 * 1024 * 1024)
                throw new InvalidOperationException("Gameplay class catalog exceeds the ABI budget.");
            ulong generation = ++nextAssembly;
            Assemblies.Add(generation, new(loader, types, componentTypes));
            return new { generation, classes, componentClasses };
        }
        catch
        {
            RetiredAssemblies.Add(new(loader, trackResurrection: true));
            loader.Unload();
            throw;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void UnloadGameplay(ulong generation)
    {
        if (!Assemblies.Remove(generation, out var assembly)) return;
        RetiredAssemblies.Add(new(assembly.Loader, trackResurrection: true));
        assembly.Loader.Unload();
    }

    private static object AssemblyStatus(bool collect)
    {
        // 显式诊断才触发 GC；正常帧和重载提交不等待收集，卸载完成由弱引用观测。
        if (collect)
            for (int index = 0; index < 10 && RetiredAssemblies.Any(r => r.IsAlive); index++)
            { GC.Collect(); GC.WaitForPendingFinalizers(); }
        int alive = RetiredAssemblies.Count(r => r.IsAlive);
        RetiredAssemblies.RemoveAll(r => !r.IsAlive);
        return new { loaded = Assemblies.Count, retiredAlive = alive, contexts = Contexts.Count };
    }
}
