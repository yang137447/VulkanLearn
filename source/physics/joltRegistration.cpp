#include "physics/joltRegistration.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>

namespace VL
{
namespace Physics
{
namespace
{

void PhysicsTrace(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}

#ifdef JPH_ENABLE_ASSERTS
bool PhysicsAssert(const char* expression, const char* message, const char* file, JPH::uint line)
{
    std::fprintf(stderr, "Jolt assertion: %s (%s) at %s:%u\n",
        expression, message ? message : "", file, line);
    std::abort();
}
#endif

// 进程级注册；候选/活动 World 可同时存活，不能在某个 Stop 中注销其它 World 的类型。
struct JoltRegistration
{
    JoltRegistration()
    {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = PhysicsTrace;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = PhysicsAssert;)
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
    ~JoltRegistration()
    {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
};

} // namespace

void EnsureJoltRegistered()
{
    static JoltRegistration registration;
    (void)registration;
}

} // namespace Physics
} // namespace VL
