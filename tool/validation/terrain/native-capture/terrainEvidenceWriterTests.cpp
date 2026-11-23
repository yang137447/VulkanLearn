#include "terrainEvidenceWriter.h"
#include <fstream>
#include <iostream>
#include <string>

namespace
{
BOOL WINAPI FailWrite(HANDLE, LPCVOID, DWORD, LPDWORD written, LPOVERLAPPED)
{
    *written = 0;
    SetLastError(ERROR_DISK_FULL);
    return FALSE;
}
BOOL WINAPI FailFlush(HANDLE) { SetLastError(ERROR_WRITE_FAULT); return FALSE; }
BOOL WINAPI FailClose(HANDLE) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
}
int main(int argc, char** argv)
{
    try
    {
        Require(argc == 2, "Expected fresh output directory");
        const std::filesystem::path root(argv[1]);
        Require(std::filesystem::create_directory(root), "Test directory must be fresh");
        int passed = 0;
        const auto attempt = [&](const std::string& name, VL::TerrainEvidenceIo operations,
            bool race, bool expectedSuccess)
        {
            const auto binary = root/(name+".bin");
            const auto metadata = root/(name+".json");
            bool complete = false;
            bool threw = false;
            Require(!std::filesystem::exists(metadata), "Precheck should pass");
            try
            {
                VL::WriteTerrainEvidenceFile(binary, [](std::ostream& output) { output << "payload"; });
                if (race) std::filesystem::create_directory(metadata);
                VL::WriteTerrainEvidenceFile(metadata,
                    [](std::ostream& output) { output << "{\"complete\":true}"; }, operations);
                complete = true;
            }
            catch (const std::exception& error)
            {
                threw = true;
                std::cout << name << ": retained raw error: " << error.what() << '\n';
            }
            Require(complete == expectedSuccess && threw != expectedSuccess,
                "Failure reached complete or success failed");
            if (expectedSuccess)
            {
                std::ifstream input(metadata);
                const std::string content((std::istreambuf_iterator<char>(input)), {});
                Require(content == "{\"complete\":true}", "Success bytes mismatch");
            }
            ++passed;
        };
        attempt("success", {}, false, true);
        attempt("post-precheck-directory-race", {}, true, false);
        VL::TerrainEvidenceIo operations;
        operations.write = FailWrite;
        attempt("write-disk-full", operations, false, false);
        operations = {};
        operations.flush = FailFlush;
        attempt("flush-fault", operations, false, false);
        operations = {};
        operations.close = FailClose;
        attempt("close-fault", operations, false, false);
        const auto existing = root/"success.json";
        bool rejected = false;
        try { VL::WriteTerrainEvidenceFile(existing, [](std::ostream& output) { output << "clobber"; }); }
        catch (const std::exception&) { rejected = true; }
        Require(rejected, "Existing proof was overwritten");
        ++passed;
        std::cout << "PASS " << passed << " native CPU evidence IO cases\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
