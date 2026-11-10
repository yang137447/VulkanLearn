#include "editor/editorHost.h"
#include "engine/application.h"

#include <memory>

int main(int argc, char **argv)
{
    return VL::RunApplication(argc, argv, []
    {
        return std::make_unique<VL::EditorHost>();
    });
}
