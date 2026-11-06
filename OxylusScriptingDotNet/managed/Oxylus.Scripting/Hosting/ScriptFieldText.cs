using System;
using System.Globalization;
using System.Numerics;

namespace Oxylus.Hosting;

// the text a scene behaviour's fields show as, called by the generated registry. Never runs game code
public static class ScriptFieldText
{
    const int MaxStringLength = 256;

    public static string Value<T>(T value)
    {
        object? boxed = value;
        return boxed switch
        {
            null => "null",
            bool b => b ? "true" : "false",
            float f => f.ToString("G6", CultureInfo.InvariantCulture),
            double d => d.ToString("G6", CultureInfo.InvariantCulture),
            string s => s.Length > MaxStringLength ? string.Concat(s.AsSpan(0, MaxStringLength), "...") : s,
            Enum e => e.ToString(),
            Vector2 v => $"{Component(v.X)}, {Component(v.Y)}",
            Vector3 v => $"{Component(v.X)}, {Component(v.Y)}, {Component(v.Z)}",
            Vector4 v => $"{Component(v.X)}, {Component(v.Y)}, {Component(v.Z)}, {Component(v.W)}",
            Quaternion q => $"{Component(q.X)}, {Component(q.Y)}, {Component(q.Z)}, {Component(q.W)}",
            Entity entity => entity.ToString(),
            sbyte or byte or short or ushort or int or uint or long or ulong or char or decimal =>
                ((IConvertible)boxed).ToString(CultureInfo.InvariantCulture),
            _ => typeof(T).Name,
        };
    }

    public static string Count(int? count, string typeName) =>
        count is { } value ? $"{typeName} ({value.ToString(CultureInfo.InvariantCulture)})" : "null";

    public static string Reference(object? value, string typeName) => value is null ? "null" : typeName;

    static string Component(float value) => value.ToString("G6", CultureInfo.InvariantCulture);
}
