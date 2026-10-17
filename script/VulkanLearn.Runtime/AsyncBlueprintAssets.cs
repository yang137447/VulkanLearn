using System.Text.Json;

namespace VulkanLearn;

public readonly record struct AssetLoadHandle(ulong Context, ulong Id);
public readonly record struct BlueprintAssetLease(ulong Context, ulong Id, AssetId Asset);
public readonly record struct BlueprintAssetLoadResult(BlueprintAssetLease? Lease, string Error)
{
    public bool Success => Lease.HasValue;
    public AssetId Asset => Lease?.Asset ?? default;
}

// 完成回调仍在所属 Blueprint 的 GT 边界内；lease 与请求句柄不能跨 Play 使用。
public sealed class BlueprintAssets
{
    internal Func<string, Action<BlueprintAssetLoadResult>, AssetLoadHandle> LoadRequest = null!;
    internal Action<AssetLoadHandle> CancelRequest = null!;
    internal Action<BlueprintAssetLease> UnloadRequest = null!;
    public AssetLoadHandle LoadBlueprintAsync(string path, Action<BlueprintAssetLoadResult> completed) =>
        LoadRequest(path, completed);
    public void Cancel(AssetLoadHandle request) => CancelRequest(request);
    public void Unload(BlueprintAssetLease lease) => UnloadRequest(lease);
}

public static partial class NativeBridge
{
    private static ulong nextAssetRequest;
    private sealed record PendingAsset(ulong Id, Blueprint Owner, string Path,
        Action<BlueprintAssetLoadResult> Completed, bool Loaded = false, Component? Component = null);
    private sealed partial class GameplayScheduler
    {
        private readonly Dictionary<ulong, PendingAsset> assets = [];
        private readonly HashSet<ulong> sentAssets = [];
        private readonly List<(Blueprint Owner, ulong Id)> cancelledAssets = [];
        private readonly HashSet<string> dynamicTemplates = new(StringComparer.Ordinal);

        private void BindAssets(Blueprint owner)
        {
            owner.Assets = new()
            {
                LoadRequest = (path, completed) =>
                {
                    Require(owner); ArgumentNullException.ThrowIfNull(completed);
                    if (string.IsNullOrEmpty(path) || path.Length > 1024 || path.IndexOfAny(['\\', ':', '\0']) >= 0 ||
                        path.Split('/').Any(p => p is "" or "." or "..") ||
                        !path.EndsWith(".json", StringComparison.OrdinalIgnoreCase))
                        throw new ArgumentException("Blueprint load requires a resource-relative JSON path.");
                    if (assets.Count >= 256) throw new InvalidOperationException("Asset lease budget exceeded.");
                    // 资源 worker 可跨 Stop 完成；请求 ID 在进程桥生命周期内不复用。
                    ulong id = ++nextAssetRequest;
                    assets.Add(id, new(id, owner, path, completed, Component: callbackComponent));
                    undo.Add(() => assets.Remove(id));
                    return new(AssetContext.Id, id);
                },
                CancelRequest = handle =>
                {
                    Require(owner);
                    if (handle.Context != AssetContext.Id) throw new InvalidOperationException("Stale asset request context.");
                    if (assets.TryGetValue(handle.Id, out var request) && request.Loaded)
                        throw new InvalidOperationException("Completed assets require Unload with their lease.");
                    RemoveAsset(owner, handle.Id);
                },
                UnloadRequest = lease =>
                {
                    Require(owner);
                    if (lease.Context != AssetContext.Id) throw new InvalidOperationException("Stale asset lease context.");
                    if (assets.TryGetValue(lease.Id, out var asset) && (!asset.Loaded || asset.Path != lease.Asset.Path))
                        throw new InvalidOperationException("Invalid asset lease.");
                    RemoveAsset(owner, lease.Id);
                }
            };
        }
        private void RemoveAsset(Blueprint owner, ulong id)
        {
            if (!assets.TryGetValue(id, out var request)) return;
            if (request.Owner != owner || request.Component != callbackComponent)
                throw new InvalidOperationException("Asset handle belongs to another callback owner.");
            assets.Remove(id);
            var cancellation = (owner, id);
            cancelledAssets.Add(cancellation);
            undo.Add(() => { assets.Add(id, request); cancelledAssets.Remove(cancellation); });
        }
        public object[] TakeAssets(Blueprint owner) => assets.Count == 0 ? [] : assets.Values
            .Where(a => a.Owner == owner && !a.Loaded && !sentAssets.Contains(a.Id)).OrderBy(a => a.Id).Select(a =>
            {
                sentAssets.Add(a.Id);
                return (object)new { id = a.Id, owner = owner.Handle.Id, path = a.Path };
            }).ToArray();
        public ulong[] TakeAssetCancellations(Blueprint owner)
        {
            if (cancelledAssets.Count == 0) return [];
            var ids = cancelledAssets.Where(c => c.Owner == owner).Select(c => c.Id).Distinct().ToArray();
            cancelledAssets.RemoveAll(c => c.Owner == owner);
            foreach (ulong id in ids) sentAssets.Remove(id);
            return ids;
        }
        public void CompleteAssets(JsonElement results)
        {
            foreach (var result in results.EnumerateArray())
            {
                ulong id = result.GetProperty("id").GetUInt64();
                if (!assets.TryGetValue(id, out var request) || request.Loaded) continue;
                if (!CanRun(request.Owner, request.Component))
                {
                    if (request.Component != null) RemoveComponentAssets(request.Component);
                    else RemoveAssetOwner(request.Owner);
                    continue;
                }
                string error = result.TryGetProperty("error", out var errorNode) ? errorNode.GetString()! : "";
                BlueprintAssetLease? lease = null;
                if (error.Length == 0)
                {
                    if (result.GetProperty("path").GetString() != request.Path)
                        throw new InvalidOperationException("Asset completion identity mismatch.");
                    foreach (var template in result.GetProperty("templates").EnumerateArray())
                    {
                        string path = template.GetProperty("__templatePath").GetString()!;
                        if (!AssetContext.Templates.ContainsKey(path)) dynamicTemplates.Add(path);
                        AssetContext.Templates[path] = template.Clone();
                    }
                    assets[id] = request with { Loaded = true };
                    lease = new(AssetContext.Id, id, new(request.Path));
                }
                else { assets.Remove(id); sentAssets.Remove(id); }
                Invoke(request.Owner, "AssetLoaded", () => request.Completed(new(lease, error)), request.Component);
            }
        }
        public bool CanSpawnAsset(string path) => !dynamicTemplates.Contains(path) ||
            assets.Values.Any(a => a.Loaded && a.Path == path);
        public void ForgetAssets(JsonElement paths)
        {
            foreach (var pathNode in paths.EnumerateArray())
            {
                string path = pathNode.GetString()!;
                if (assets.Values.Any(a => a.Path == path)) throw new InvalidOperationException("Cannot unload a leased Blueprint.");
                if (dynamicTemplates.Remove(path)) AssetContext.Templates.Remove(path);
            }
        }
        private void RemoveAssetOwner(Blueprint owner)
        {
            foreach (var asset in assets.Values.Where(a => a.Owner == owner).ToArray())
            {
                assets.Remove(asset.Id); cancelledAssets.Add((owner, asset.Id));
                // 完成交付前 owner 可能已禁用，后续空回调不会标脏；取消仍须跨边界释放原生租约。
                if (context.InstanceIds.TryGetValue(owner.Handle.Id, out var instance)) instance.Dirty = true;
            }
        }
        private void RemoveComponentAssets(Component component)
        {
            foreach (var asset in assets.Values.Where(a => a.Component == component).ToArray())
            {
                assets.Remove(asset.Id); cancelledAssets.Add((asset.Owner, asset.Id));
                if (context.InstanceIds.TryGetValue(asset.Owner.Handle.Id, out var instance)) instance.Dirty = true;
            }
        }
        private void ReleaseAssets()
        {
            assets.Clear(); sentAssets.Clear(); cancelledAssets.Clear(); dynamicTemplates.Clear();
        }
    }
}
