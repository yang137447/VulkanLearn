#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include "editor/command/assetJsonCommandHost.h"

int main(int argc, char** argv)
{
    try
    {
        std::filesystem::path root, inputPath;
        for (int index = 1; index < argc; ++index)
        {
            const std::string option = argv[index];
            if (option == "--help")
            {
                std::cout << "vl_asset_edit [--resource-root PATH] [--input REQUESTS.jsonl]\n"
                    "One editor protocol JSON request per line; results are JSON Lines on stdout.\n"
                    "Read scene/mesh/tressfx/texture/blueprint.get_document, then modify with expectedDocumentRevision,\n"
                    "validate, and explicitly save. No window or GPU is initialized.\n";
                return 0;
            }
            if ((option != "--resource-root" && option != "--input") || index + 1 == argc)
                throw std::invalid_argument("Unknown/incomplete option: " + option);
            if (option == "--resource-root") root = argv[++index];
            else inputPath = argv[++index];
        }
        if (root.empty())
        {
            std::ifstream config("config/config.json");
            root = nlohmann::json::parse(config).at("resourcePath").get<std::string>();
        }
        if (!std::filesystem::is_directory(root)) throw std::invalid_argument("resource root does not exist");
        std::ifstream file;
        if (!inputPath.empty())
        {
            file.open(inputPath, std::ios::binary);
            if (!file) throw std::runtime_error("Cannot open input request file");
        }
        std::istream& input = inputPath.empty() ? std::cin : file;
        VL::Editor::AssetJsonCommandHost host(root);
        bool failed = false;
        std::string line;
        while (input.peek() != std::char_traits<char>::eof())
        {
            line.clear();
            bool oversized = false;
            char character;
            while (input.get(character) && character != '\n')
            {
                if (line.size() < 1024 * 1024 + 1) line.push_back(character);
                else oversized = true;
            }
            if (input.bad()) throw std::runtime_error("Failed to read requests");
            // 超长行读完再恢复协议边界，内存不随攻击者的整行长度增长。
            if (oversized) line.resize(1024 * 1024 + 1);
            if (line.empty()) continue;
            const auto result = host.Process(line);
            std::cout << result.dump() << std::endl;
            failed = failed || result.at("status") != "succeeded";
        }
        return failed ? 2 : 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
