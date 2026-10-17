using System.Reflection;
using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;

namespace VulkanLearn;

public static partial class NativeBridge
{
    private static string[] BlueprintSourceFiles(Type type, MetadataReader? symbols)
    {
        if (symbols == null) return [];
        var files = new SortedSet<string>(StringComparer.Ordinal);
        var definition = MetadataTokens.TypeDefinitionHandle(type.MetadataToken & 0x00ffffff);
        // Portable PDB 的类型文档记录也覆盖空类、自动属性与 partial；不按文件名猜类名。
        var typeDocuments = new Guid("932E74BC-DBA9-4478-8D46-0F32A7BAB3D3");
        foreach (var handle in symbols.GetCustomDebugInformation(definition))
        {
            var info = symbols.GetCustomDebugInformation(handle);
            if (symbols.GetGuid(info.Kind) != typeDocuments) continue;
            var blob = symbols.GetBlobReader(info.Value);
            while (blob.RemainingBytes > 0)
            {
                var document = symbols.GetDocument(MetadataTokens.DocumentHandle(blob.ReadCompressedInteger()));
                files.Add(symbols.GetString(document.Name));
            }
        }
        // 较旧编译器没有类型文档记录时，使用本类方法的序列点，不借用父类源码。
        foreach (var method in type.GetMethods(BindingFlags.Public | BindingFlags.NonPublic |
                     BindingFlags.Instance | BindingFlags.Static | BindingFlags.DeclaredOnly)
                     .Cast<MethodBase>().Concat(type.GetConstructors(BindingFlags.Public |
                         BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static)))
        {
            var debug = symbols.GetMethodDebugInformation(
                MetadataTokens.MethodDefinitionHandle(method.MetadataToken & 0x00ffffff));
            foreach (var point in debug.GetSequencePoints())
                if (!point.IsHidden && !point.Document.IsNil)
                    files.Add(symbols.GetString(symbols.GetDocument(point.Document).Name));
        }
        return files.ToArray();
    }

    private static object DescribeBlueprintClass(KeyValuePair<string, Type> entry, MetadataReader? symbols) =>
        new
        {
            name = entry.Key,
            assemblyName = entry.Value.Assembly.GetName().Name,
            assemblyQualifiedName = entry.Value.AssemblyQualifiedName,
            baseClass = entry.Value.BaseType?.FullName ?? "",
            sourceFiles = BlueprintSourceFiles(entry.Value, symbols),
            properties = DescribeProperties(entry.Value)
        };
}
