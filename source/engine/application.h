#pragma once

#include <functional>
#include <memory>

namespace VL
{

class IEditorHost;

// Shared process entry used by the editor and game executables. The factory
// runs after RuntimeConfig is loaded so the host sees a valid configuration.
int RunApplication(
    int argc,
    char** argv,
    std::function<std::unique_ptr<IEditorHost>()> createEditorHost);

} // namespace VL
