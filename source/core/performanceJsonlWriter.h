#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace VL
{

// --perf-jsonl 的写侧。每行独立 flush，进程崩溃或外部 agent 轮询时不会读到
// 半行；二进制模式保证 Windows 上也是单个 '\n'。父目录不存在时创建，
// 创建/打开失败返回人类可读的原因，绝不静默丢弃时间序列。
class PerformanceJsonlWriter
{
public:
    bool Open(const std::filesystem::path& path, std::string& error)
    {
        Close();
        if (path.empty())
        {
            error = "--perf-jsonl requires a file path.";
            return false;
        }
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty())
        {
            std::error_code directoryError;
            std::filesystem::create_directories(parent, directoryError);
            if (directoryError)
            {
                error = "Cannot create --perf-jsonl directory '" +
                    parent.string() + "': " + directoryError.message();
                return false;
            }
        }
        stream.open(path, std::ios::app | std::ios::binary);
        if (!stream.is_open())
        {
            error = "Cannot open --perf-jsonl file '" + path.string() + "' for append.";
            return false;
        }
        error.clear();
        return true;
    }

    void Append(const std::string& line)
    {
        if (!stream.is_open())
        {
            return;
        }
        stream << line << '\n';
        stream.flush();
    }

    void Close()
    {
        if (stream.is_open())
        {
            stream.flush();
            stream.close();
        }
    }

    bool IsOpen() const { return stream.is_open(); }

private:
    std::ofstream stream;
};

} // namespace VL
