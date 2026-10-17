#include <filesystem>
#include <iostream>
#include "script/scriptSourceFiles.h"
#include "shader/build/atomicFile.h"

// 初始构建与编辑器使用同一源码收集器；此工具只写生成清单，不加载 CLR 或修改作者文件。
#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
    try
    {
        if (argc != 3) throw std::invalid_argument("Usage: vl_script_sources <resource-root> <output.props>");
        const auto text = VL::ScriptSourceItems(std::filesystem::path(argv[1]));
        VL::WriteTextFileIfChangedAtomically(std::filesystem::path(argv[2]), text);
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "C# source collection failed: " << exception.what() << '\n';
        return 1;
    }
}
