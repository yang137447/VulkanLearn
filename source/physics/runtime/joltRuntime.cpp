#include "physics/runtime/joltRuntime.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>

namespace VL::Physics
{
namespace
{

void JoltTrace(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}

#ifdef JPH_ENABLE_ASSERTS
bool JoltAssert(const char* expression, const char* message, const char* file, JPH::uint line)
{
    std::fprintf(stderr, "Jolt assertion: %s (%s) at %s:%u\n",
        expression, message ? message : "", file, line);
    return false;
}
#endif

struct Registration
{
    Registration()
    {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = JoltTrace;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = JoltAssert;)
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }

    ~Registration()
    {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
};

} // namespace

void EnsureJoltRegistered()
{
    static Registration registration;
}

JoltRegistrationToken::JoltRegistrationToken()
{
    EnsureJoltRegistered();
}

} // namespace VL::Physics
