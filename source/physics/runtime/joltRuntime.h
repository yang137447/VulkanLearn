#pragma once

namespace VL::Physics
{

// 进程级 Jolt 注册；C# Blueprint 与原生软体冲击共享同一个 Factory/Types。
void EnsureJoltRegistered();

// 作为 owner 的第一个成员构造，保证后面的 Jolt 容器/分配器在注册之后初始化。
struct JoltRegistrationToken
{
    JoltRegistrationToken();
};

} // namespace VL::Physics
