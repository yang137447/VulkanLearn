#pragma once

namespace VL
{
namespace Physics
{

// Jolt 的类型注册表属于进程；地形碰撞与 C# Blueprint 物理共用同一份注册，
// 任何创建 PhysicsSystem/Shape 的入口都必须先调用这里，不能各自 RegisterTypes()。
// 首次调用完成 RegisterDefaultAllocator/Factory/RegisterTypes，进程结束时统一注销。
void EnsureJoltRegistered();

} // namespace Physics
} // namespace VL
