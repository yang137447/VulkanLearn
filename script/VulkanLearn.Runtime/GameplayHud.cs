namespace VulkanLearn;

// HUD 只提交文本值；RmlUi 文档、布局和渲染仍由原生 UI owner 决定。
public sealed class GameplayHud
{
    internal Action<string, string> SetTextRequest = null!;
    internal Action<string> RemoveRequest = null!;
    public void SetText(string key, string text) => SetTextRequest(key, text);
    public void Remove(string key) => RemoveRequest(key);
}

public abstract partial class Blueprint
{
    public GameplayHud Hud { get; internal set; } = new();
}

public static partial class NativeBridge
{
    private sealed record HudEntry(Blueprint Owner, Component? Component, string Key, string Text);

    private sealed partial class GameplayScheduler
    {
        private readonly Dictionary<(Blueprint, Component?, string), HudEntry> hud = [];

        public void BindHud(Blueprint owner)
        {
            owner.Hud = new()
            {
                SetTextRequest = (key, text) =>
                {
                    Require(owner); ValidateHudKey(key);
                    if (text == null || text.Length > 1024 || text.Contains('\0'))
                        throw new ArgumentException("HUD text requires at most 1024 characters without NUL.");
                    var identity = (owner, callbackComponent, key);
                    hud.TryGetValue(identity, out var previous);
                    if (previous != null && previous.Text == text) return;
                    if (previous == null &&
                        (hud.Count >= 128 || hud.Values.Count(e => e.Owner == owner) >= 32 ||
                         hud.Values.Count(e => e.Owner == owner && e.Component == callbackComponent) >= 8))
                        throw new InvalidOperationException("HUD line budget exceeded: context 128, Blueprint 32, publisher 8.");
                    hud[identity] = new(owner, callbackComponent, key, text);
                    undo.Add(() =>
                    {
                        if (previous == null) hud.Remove(identity);
                        else hud[identity] = previous;
                    });
                    InvalidateHud(owner);
                },
                RemoveRequest = key =>
                {
                    Require(owner); ValidateHudKey(key);
                    var identity = (owner, callbackComponent, key);
                    if (!hud.Remove(identity, out var previous)) return;
                    undo.Add(() => hud.Add(identity, previous));
                    InvalidateHud(owner);
                }
            };
        }

        private static void ValidateHudKey(string key)
        {
            if (string.IsNullOrWhiteSpace(key) || key.Length > 64 || key.Any(char.IsControl))
                throw new ArgumentException("HUD keys require 1..64 characters without control characters.");
        }

        private void InvalidateHud(Blueprint owner)
        {
            if (AssetContext.InstanceIds.TryGetValue(owner.Handle.Id, out var instance)) instance.Dirty = true;
        }

        public object[] ExportHud(Blueprint owner) => hud.Count == 0 || !owner.Enabled || owner.PendingDestroy ? Array.Empty<object>() :
            hud.Values.Where(e => e.Owner == owner && (e.Component == null || e.Component.Enabled && !e.Component.Failed))
                .OrderBy(e => e.Component?.Id ?? "", StringComparer.Ordinal).ThenBy(e => e.Key, StringComparer.Ordinal)
                .Select(e => (object)new { component = e.Component?.Id ?? "", key = e.Key, text = e.Text }).ToArray();

        private void RemoveHud(Blueprint owner, Component? component = null)
        {
            var entries = hud.Where(p => p.Value.Owner == owner &&
                (component == null || p.Value.Component == component)).Select(p => p.Key).ToArray();
            foreach (var identity in entries) hud.Remove(identity);
            if (entries.Length != 0) InvalidateHud(owner);
        }
    }
}
