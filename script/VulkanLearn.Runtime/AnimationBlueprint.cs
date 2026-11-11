namespace VulkanLearn;

// 动画蓝图是逻辑组件：它挂在角色 Blueprint 上，只持有该角色 MeshComponent 的
// AnimationPlayback。骨骼、时钟、姿态求值仍由原生动画系统拥有；本类只负责
// “看角色状态 -> 提交动画意图”，不读写 World/GPU 资源。
public abstract class AnimationBlueprint : Component
{
    // 空串表示 owner 只有一个 MeshComponent 时自动选择；多 Mesh 必须显式指定。
    [Expose(InstanceEditable = true)] public string MeshComponentId { get; set; } = "";

    private MeshComponent? mesh;

    protected MeshComponent Mesh => mesh ?? throw new InvalidOperationException(
        "AnimationBlueprint is not bound to a mesh component.");
    public AnimationPlayback Animation => Mesh.Animation;

    protected sealed override void OnCreate()
    {
        mesh = ResolveMesh();
        OnAnimationCreate();
    }

    protected sealed override void BeginPlay() => OnAnimationBeginPlay();
    protected sealed override void Tick(float deltaTime) => OnAnimationTick(deltaTime);
    protected sealed override void LateTick(float deltaTime) => OnAnimationLateTick(deltaTime);
    protected sealed override void EndPlay(EndPlayReason reason) => OnAnimationEndPlay(reason);
    protected sealed override void OnReload() => OnAnimationReload();

    // 子类只实现动画相关生命周期；Owner 的 Tick 先于组件 Tick，LateTick 也一样，
    // 因此角色可在 LateTick 写入最终速度/接地状态，再由动画蓝图的 LateTick 消费。
    protected virtual void OnAnimationCreate() { }
    protected virtual void OnAnimationBeginPlay() { }
    protected virtual void OnAnimationTick(float deltaTime) { }
    protected virtual void OnAnimationLateTick(float deltaTime) { }
    protected virtual void OnAnimationEndPlay(EndPlayReason reason) { }
    protected virtual void OnAnimationReload() { }

    private MeshComponent ResolveMesh()
    {
        if (!string.IsNullOrEmpty(MeshComponentId))
            return Owner.GetComponent<MeshComponent>(MeshComponentId);
        var meshes = Owner.Components.OfType<MeshComponent>().ToArray();
        if (meshes.Length == 1) return meshes[0];
        throw new InvalidOperationException(meshes.Length == 0
            ? "AnimationBlueprint requires a mesh component on its owner."
            : "AnimationBlueprint requires MeshComponentId when the owner has multiple meshes.");
    }
}
