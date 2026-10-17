using System.Numerics;
using VulkanLearn;

namespace ReloadFixtures;

public sealed class ContactMigrationBlueprint : Blueprint
{
    [Migrate] public int Counter { get; set; }
    protected override void OnCollisionEnter(BlueprintHandle other) => ++Counter;
    protected override void OnTriggerEnter(BlueprintHandle other) => Counter += 10;
    protected override void OnCollisionExit(BlueprintHandle other) => Counter += 100;
    protected override void OnTriggerExit(BlueprintHandle other) => Counter += 1000;
}

public sealed class PendingMigrationBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)] public bool PublishEvents { get; set; }
    protected override void BeginPlay()
    {
        if (PublishEvents)
        {
            Events.Subscribe("pending", _ => Events.Publish("pending"));
            Events.Publish("pending");
        }
        else World.Spawn("Common/Blueprints/BP_pending_migration.json", Vector3.Zero);
    }
}
