using System.Collections;
using System.Numerics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace VulkanLearn;

public static partial class NativeBridge
{
    private sealed class DataMember(MemberInfo member, Type type)
    {
        public string Name => member.Name;
        public Type Type => type;
        public object? Get(object owner) => member is PropertyInfo property ? property.GetValue(owner) : ((FieldInfo)member).GetValue(owner);
        public void Set(object owner, object value)
        {
            if (member is PropertyInfo property) property.SetValue(owner, value);
            else ((FieldInfo)member).SetValue(owner, value);
        }
    }

    private sealed class DataSchema(Type type, string kind, DataMember[]? members = null, DataSchema? element = null)
    {
        public readonly Type Type = type;
        public readonly string Kind = kind;
        public readonly DataMember[] Members = members ?? [];
        public readonly DataSchema? Element = element;
    }

    private sealed class DataBudget
    {
        private int nodes;
        public void Visit(int depth, string path)
        {
            if (depth > 16) throw DataError(path, "Data depth exceeds 16.");
            if (++nodes > 4096) throw DataError(path, "Data value exceeds 4096 nodes.");
        }
        public static void Container(int count, string path)
        {
            if (count > 1024) throw DataError(path, "Data container exceeds 1024 elements.");
        }
    }

    private sealed class DataAccessors(Type type)
    {
        public readonly PropertyInfo[] Exposed = RootProperties(type, typeof(ExposeAttribute));
        public readonly PropertyInfo[] Migrated = RootProperties(type, typeof(MigrateAttribute));
    }

    // Ephemeron 缓存不能把可回收 Gameplay Type 反向固定在常驻 Runtime 上。
    private static readonly ConditionalWeakTable<Type, DataSchema> DataSchemas = new();
    private static readonly ConditionalWeakTable<Type, DataAccessors> DataAccessorCache = new();
    private static DataAccessors Accessors(Type type) => DataAccessorCache.GetValue(type, key => new(key));
    private static DataSchema Schema(Type type) => DataSchemas.GetValue(type,
        key => BuildDataSchema(key, [], new(), 0, key.FullName ?? key.Name));
    private static DataSchema PropertySchema(Type type, string path)
    {
        try { return Schema(type); }
        catch (Exception error) { throw DataError(path, Unwrap(error).Message); }
    }
    private static ArgumentException DataError(string path, string message) => new($"{path}: {message}");
    private static bool Composite(DataSchema schema) => schema.Kind is "object" or "array" or "dictionary";

    private static PropertyInfo[] RootProperties(Type type, Type attribute) => type.GetProperties()
        .Where(p => p.IsDefined(attribute) && p.GetMethod?.IsPublic == true && p.SetMethod?.IsPublic == true &&
            !p.GetMethod.IsStatic && p.GetIndexParameters().Length == 0)
        .OrderBy(p => p.Name, StringComparer.Ordinal).ToArray();

    private static string? ScalarKind(Type type) => type == typeof(float) || type == typeof(double) ? "number"
        : type == typeof(int) ? "int" : type == typeof(uint) ? "uint"
        : type == typeof(bool) ? "bool" : type == typeof(string) ? "string"
        : type == typeof(Vector2) ? "vector2" : type == typeof(Vector3) ? "vector3" : type == typeof(Vector4) ? "vector4"
        : type == typeof(Quaternion) ? "quaternion"
        : type == typeof(AssetId) ? "asset" : type == typeof(BlueprintReference) ? "blueprintReference"
        : type.IsEnum && !type.IsDefined(typeof(FlagsAttribute)) ? "enum" : null;

    private static bool IgnoreDataMember(MemberInfo member) => member.IsDefined(typeof(NonSerializedAttribute)) ||
        member.GetCustomAttribute<JsonIgnoreAttribute>() is { Condition: not JsonIgnoreCondition.Never };

    private static DataSchema BuildDataSchema(Type type, HashSet<Type> ancestors, DataBudget budget, int depth, string path)
    {
        budget.Visit(depth, path);
        if (ScalarKind(type) is { } scalar) return new(type, scalar);
        if (!ancestors.Add(type)) throw DataError(path, "Recursive data types are unsupported.");
        try
        {
            if (type.IsArray)
            {
                if (!type.IsSZArray) throw DataError(path, "Only one-dimensional zero-based arrays are supported.");
                return new(type, "array", element: BuildDataSchema(type.GetElementType()!, ancestors, budget, depth + 1, path + "[]"));
            }
            if (type.IsGenericType && type.GetGenericTypeDefinition() == typeof(List<>))
                return new(type, "array", element: BuildDataSchema(type.GenericTypeArguments[0], ancestors, budget, depth + 1, path + "[]"));
            if (type.IsGenericType && type.GetGenericTypeDefinition() == typeof(Dictionary<,>))
            {
                if (type.GenericTypeArguments[0] != typeof(string)) throw DataError(path, "Dictionary keys must be strings.");
                return new(type, "dictionary", element: BuildDataSchema(type.GenericTypeArguments[1], ancestors, budget, depth + 1, path + "[key]"));
            }
            if (type == typeof(object) || type.IsEnum || type.IsInterface || type.IsAbstract ||
                typeof(Delegate).IsAssignableFrom(type) || type.Assembly == typeof(Blueprint).Assembly ||
                typeof(Blueprint).IsAssignableFrom(type) || typeof(Component).IsAssignableFrom(type) ||
                typeof(SceneComponent).IsAssignableFrom(type) ||
                typeof(IEnumerable).IsAssignableFrom(type) ||
                type.Namespace is { } space && (space == "System" || space.StartsWith("System.", StringComparison.Ordinal)) ||
                !(type.IsClass || type.IsValueType) || !type.IsVisible ||
                type.IsClass && type.GetConstructor(Type.EmptyTypes) == null)
                throw DataError(path, $"Unsupported data type {type.FullName}; use a plain public data type with a public parameterless constructor.");
            var members = type.GetProperties(BindingFlags.Public | BindingFlags.Instance)
                .Where(p => p.GetMethod?.IsPublic == true && p.SetMethod?.IsPublic == true &&
                    p.GetIndexParameters().Length == 0 && !IgnoreDataMember(p))
                .Select(p => new DataMember(p, p.PropertyType))
                .Concat(type.GetFields(BindingFlags.Public | BindingFlags.Instance)
                    .Where(f => !f.IsInitOnly && !f.IsLiteral && !IgnoreDataMember(f)).Select(f => new DataMember(f, f.FieldType)))
                .OrderBy(m => m.Name, StringComparer.Ordinal).ToArray();
            if (members.Select(m => m.Name).Distinct(StringComparer.Ordinal).Count() != members.Length)
                throw DataError(path, "Duplicate public data member names.");
            foreach (var member in members)
                _ = BuildDataSchema(member.Type, ancestors, budget, depth + 1, path + "." + member.Name);
            return new(type, "object", members);
        }
        finally { ancestors.Remove(type); }
    }

    private static object DefaultData(DataSchema schema, string path)
    {
        try
        {
            if (schema.Type == typeof(string)) return "";
            if (schema.Type == typeof(Quaternion)) return Quaternion.Identity;
            if (schema.Type.IsEnum) return Enum.GetValues(schema.Type).GetValue(0)
                ?? throw DataError(path, "Enum has no declared values.");
            if (schema.Type.IsArray) return Array.CreateInstance(schema.Type.GetElementType()!, 0);
            return Activator.CreateInstance(schema.Type) ?? throw DataError(path, "Default data construction returned null.");
        }
        catch (Exception error) when (error is not ArgumentException)
        { throw DataError(path, $"Default data construction failed: {Unwrap(error).Message}"); }
    }

    private static object RequireDataValue(DataSchema schema, object? value, string path, bool materialize)
    {
        value ??= materialize ? DefaultData(schema, path) : throw DataError(path, "Null data is unsupported.");
        if (value.GetType() != schema.Type) throw DataError(path, "Polymorphic data values are unsupported.");
        return value;
    }

    private static object WriteData(DataSchema schema, object? value, string path, bool materialize,
        DataBudget budget, int depth, HashSet<object> ancestors)
    {
        budget.Visit(depth, path);
        value = RequireDataValue(schema, value, path, materialize);
        if (!Composite(schema)) return WriteScalar(schema, value, path);
        bool reference = !schema.Type.IsValueType;
        if (reference && !ancestors.Add(value)) throw DataError(path, "Cyclic data values are unsupported.");
        try
        {
            if (schema.Kind == "object")
            {
                var result = new Dictionary<string, object>(StringComparer.Ordinal);
                foreach (var member in schema.Members)
                    result.Add(member.Name, WriteData(Schema(member.Type), member.Get(value), path + "." + member.Name,
                        materialize, budget, depth + 1, ancestors));
                return result;
            }
            if (schema.Kind == "dictionary")
            {
                var dictionary = (IDictionary)value;
                DataBudget.Container(dictionary.Count, path);
                var result = new Dictionary<string, object>(StringComparer.Ordinal);
                foreach (DictionaryEntry entry in dictionary)
                {
                    string key = (string)entry.Key;
                    result.Add(key, WriteData(schema.Element!, entry.Value, path + "[" + key + "]",
                        materialize, budget, depth + 1, ancestors));
                }
                return result;
            }
            var sequence = (IList)value;
            DataBudget.Container(sequence.Count, path);
            var values = new object[sequence.Count];
            for (int index = 0; index < values.Length; ++index)
                values[index] = WriteData(schema.Element!, sequence[index], path + "[" + index + "]",
                    materialize, budget, depth + 1, ancestors);
            return values;
        }
        finally { if (reference) ancestors.Remove(value); }
    }

    private static object WriteScalar(DataSchema schema, object value, string path)
    {
        object result = value switch
        {
            Vector2 v when float.IsFinite(v.X) && float.IsFinite(v.Y) => new[] { v.X, v.Y },
            Vector3 v when Finite(v) => Values(v),
            Vector4 v when float.IsFinite(v.X) && float.IsFinite(v.Y) && float.IsFinite(v.Z) && float.IsFinite(v.W) =>
                new[] { v.X, v.Y, v.Z, v.W },
            Quaternion q when float.IsFinite(q.LengthSquared()) && MathF.Abs(q.LengthSquared() - 1) <= 1e-4f =>
                new[] { q.X, q.Y, q.Z, q.W },
            Vector2 or Vector3 or Vector4 or Quaternion => throw DataError(path, "Vector must be finite and quaternion must have unit length."),
            float f when !float.IsFinite(f) => throw DataError(path, "Number must be finite."),
            double d when !double.IsFinite(d) => throw DataError(path, "Number must be finite."),
            Enum e when Enum.IsDefined(schema.Type, e) => e.ToString(),
            Enum => throw DataError(path, "Enum requires a declared value."),
            AssetId a => a.Path ?? "",
            BlueprintReference r => r.Id ?? "",
            _ => value
        };
        if (schema.Kind is "asset" or "blueprintReference")
            ValidateDataReference((string)result, schema.Kind, path);
        return result;
    }

    private static void ValidateDataReference(string text, string kind, string path)
    {
        bool invalid = text.Contains('\0') || (kind == "blueprintReference" ? text.IndexOfAny(['/', '\\', '|']) >= 0 :
            text.Length > 0 && (text.IndexOfAny(['\\', ':']) >= 0 || text.Split('/').Any(p => p is "" or "." or "..")));
        if (invalid) throw DataError(path, "Invalid reference identity.");
    }

    private static object ReadData(DataSchema schema, JsonElement? input, object? defaults, string path,
        DataBudget budget, int depth)
    {
        budget.Visit(depth, path);
        if (input is { ValueKind: JsonValueKind.Null or JsonValueKind.Undefined })
            throw DataError(path, "Null data is unsupported.");
        try
        {
            if (!Composite(schema))
            {
                if (input == null)
                {
                    var current = RequireDataValue(schema, defaults, path, true);
                    _ = WriteScalar(schema, current, path);
                    return current;
                }
                return ReadScalar(schema, input.Value, path);
            }
            if (schema.Kind == "object")
            {
                if (input is { ValueKind: not JsonValueKind.Object }) throw DataError(path, "Expected an object.");
                var values = new Dictionary<string, JsonElement>(StringComparer.Ordinal);
                if (input != null)
                    foreach (var field in input.Value.EnumerateObject())
                        if (!values.TryAdd(field.Name, field.Value) || !schema.Members.Any(m => m.Name == field.Name))
                            throw DataError(path + "." + field.Name, "Unknown or duplicate data member.");
                object baseline = RequireDataValue(schema, defaults, path, true);
                object result = DefaultData(schema, path);
                foreach (var member in schema.Members)
                    member.Set(result, ReadData(Schema(member.Type), values.TryGetValue(member.Name, out var field) ? field : null,
                        member.Get(baseline), path + "." + member.Name, budget, depth + 1));
                return result;
            }
            if (schema.Kind == "dictionary")
            {
                if (input is { ValueKind: not JsonValueKind.Object }) throw DataError(path, "Expected a dictionary object.");
                var result = (IDictionary)DefaultData(schema, path);
                if (input == null)
                {
                    var baseline = (IDictionary)RequireDataValue(schema, defaults, path, true);
                    DataBudget.Container(baseline.Count, path);
                    foreach (DictionaryEntry entry in baseline)
                        result.Add(entry.Key, ReadData(schema.Element!, null, entry.Value, path + "[" + entry.Key + "]", budget, depth + 1));
                }
                else
                {
                    var entries = input.Value.EnumerateObject().ToArray();
                    DataBudget.Container(entries.Length, path);
                    foreach (var entry in entries)
                    {
                        if (result.Contains(entry.Name)) throw DataError(path + "[" + entry.Name + "]", "Duplicate dictionary key.");
                        result.Add(entry.Name, ReadData(schema.Element!, entry.Value, null, path + "[" + entry.Name + "]", budget, depth + 1));
                    }
                }
                return result;
            }
            if (input is { ValueKind: not JsonValueKind.Array }) throw DataError(path, "Expected an array.");
            var sequence = input == null ? (IList)RequireDataValue(schema, defaults, path, true) : null;
            int length = input?.GetArrayLength() ?? sequence!.Count;
            DataBudget.Container(length, path);
            IList output = schema.Type.IsArray ? Array.CreateInstance(schema.Element!.Type, length) : (IList)DefaultData(schema, path);
            for (int index = 0; index < length; ++index)
            {
                object item = ReadData(schema.Element!, input == null ? null : input.Value[index],
                    sequence?[index], path + "[" + index + "]", budget, depth + 1);
                if (schema.Type.IsArray) output[index] = item; else output.Add(item);
            }
            return output;
        }
        catch (Exception error) when (error is not ArgumentException)
        { throw DataError(path, Unwrap(error).Message); }
    }

    private static object ReadScalar(DataSchema schema, JsonElement value, string path)
    {
        string kind = schema.Kind;
        if (kind.StartsWith("vector", StringComparison.Ordinal) || kind == "quaternion")
        {
            int count = kind == "quaternion" ? 4 : kind[^1] - '0';
            if (value.ValueKind != JsonValueKind.Array || value.GetArrayLength() != count)
                throw DataError(path, "Vector has wrong component count.");
            float[] v = value.EnumerateArray().Select(n => n.GetSingle()).ToArray();
            object converted = kind == "quaternion" ? new Quaternion(v[0], v[1], v[2], v[3]) :
                count == 2 ? (object)new Vector2(v[0], v[1]) :
                count == 3 ? new Vector3(v[0], v[1], v[2]) : new Vector4(v[0], v[1], v[2], v[3]);
            _ = WriteScalar(schema, converted, path);
            return converted;
        }
        if (kind == "enum")
        {
            string text = value.GetString()!;
            if (!Enum.GetNames(schema.Type).Contains(text, StringComparer.Ordinal))
                throw DataError(path, "Enum requires a declared name.");
            return Enum.Parse(schema.Type, text);
        }
        if (kind is "asset" or "blueprintReference")
        {
            string text = value.GetString()!;
            ValidateDataReference(text, kind, path);
            return kind == "asset" ? (object)new AssetId(text) : new BlueprintReference(text);
        }
        object result = JsonSerializer.Deserialize(value.GetRawText(), schema.Type)
            ?? throw DataError(path, "Null data is unsupported.");
        _ = WriteScalar(schema, result, path);
        return result;
    }

    private static object DescribeData(DataSchema schema, object? defaults, string path, string? name = null, bool editable = false)
    {
        defaults = RequireDataValue(schema, defaults, path, true);
        var description = new Dictionary<string, object>
        {
            ["type"] = schema.Kind, ["value"] = WriteData(schema, defaults, path, true, new(), 0, new(ReferenceEqualityComparer.Instance)),
            ["options"] = schema.Type.IsEnum ? Enum.GetNames(schema.Type) : Array.Empty<string>(),
            ["instanceEditable"] = editable
        };
        if (name != null) description["name"] = name;
        if (Composite(schema)) description["typeName"] = schema.Type.FullName!;
        if (schema.Kind == "object")
            description["fields"] = schema.Members.Select(m => DescribeData(Schema(m.Type), m.Get(defaults), path + "." + m.Name, m.Name)).ToArray();
        if (schema.Element != null)
            description["element"] = DescribeData(schema.Element, null, path + "[]");
        return description;
    }

    private static void BindExposedProperties(object target, JsonElement values)
    {
        var properties = Properties(target.GetType());
        foreach (var entry in values.EnumerateObject())
            if (!properties.Any(p => p.Name == entry.Name))
                throw DataError(target.GetType().FullName + "." + entry.Name, "Unknown exposed property.");
        // 配置归每个实例独占；即使构造默认值来自静态共享集合，也不把引用跨实例传播。
        foreach (var property in properties)
        {
            string path = target.GetType().FullName + "." + property.Name;
            property.SetValue(target, ReadData(PropertySchema(property.PropertyType, path),
                values.TryGetProperty(property.Name, out var value) ? value : null, property.GetValue(target), path, new(), 0));
        }
    }

    private static Dictionary<string, object> DescriptionProperties(Type type, IReadOnlyDictionary<string, object> values)
    {
        var properties = Properties(type);
        var result = new Dictionary<string, object>(StringComparer.Ordinal);
        foreach (var entry in values)
        {
            string path = type.FullName + "." + entry.Key;
            var property = properties.SingleOrDefault(p => p.Name == entry.Key)
                ?? throw DataError(path, "Unknown exposed property.");
            var schema = PropertySchema(property.PropertyType, path);
            if (entry.Value != null && entry.Value.GetType() == property.PropertyType)
                result.Add(entry.Key, PropertyValue(entry.Value, property.PropertyType, path));
            else
            {
                if (Composite(schema) && entry.Value is not JsonElement)
                    throw DataError(path, "Composite configuration requires its declared data type or a JSON value.");
                // 数值字面量与枚举/引用字符串仍接受既有 JSON 转换，向量的 typed 值则走上面的规范编码。
                result.Add(entry.Key, JsonSerializer.SerializeToElement(entry.Value));
            }
        }
        return result;
    }

    private static object DataMigrationSchema(DataSchema schema)
    {
        var result = new Dictionary<string, object> { ["type"] = schema.Kind, ["typeName"] = schema.Type.FullName! };
        if (schema.Kind == "object")
            result["fields"] = schema.Members.Select(m => new { name = m.Name, schema = DataMigrationSchema(Schema(m.Type)) }).ToArray();
        if (schema.Element != null) result["element"] = DataMigrationSchema(schema.Element);
        return result;
    }

    private static void ValidateDataMigrationSchema(DataSchema candidate, JsonElement previous, string path)
    {
        if (previous.GetProperty("typeName").GetString() != candidate.Type.FullName ||
            previous.GetProperty("type").GetString() != candidate.Kind)
            throw DataError(path, "Migrated data changed type.");
        if (candidate.Kind == "object")
            foreach (var field in previous.GetProperty("fields").EnumerateArray())
            {
                string name = field.GetProperty("name").GetString()!;
                var member = candidate.Members.SingleOrDefault(m => m.Name == name)
                    ?? throw DataError(path + "." + name, "Migrated data member was removed.");
                ValidateDataMigrationSchema(Schema(member.Type), field.GetProperty("schema"), path + "." + name);
            }
        if (candidate.Element != null)
            ValidateDataMigrationSchema(candidate.Element, previous.GetProperty("element"), path + "[]");
    }
}
