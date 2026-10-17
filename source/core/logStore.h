#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace VL
{

enum class LogSeverity { Debug, Info, Warning, Error };

inline const char* LogSeverityName(LogSeverity severity)
{
    switch (severity)
    {
    case LogSeverity::Debug: return "Debug";
    case LogSeverity::Info: return "Info";
    case LogSeverity::Warning: return "Warning";
    case LogSeverity::Error: return "Error";
    }
    return "Info";
}

struct LogLocation
{
    std::string file;
    uint32_t line = 0;
    uint32_t column = 0;
};

struct LogEntry
{
    uint64_t sequence = 0;
    int64_t timestampMilliseconds = 0;
    LogSeverity severity = LogSeverity::Info;
    std::string source;
    std::string message;
    LogLocation location;
};

struct LogSnapshot
{
    uint64_t revision = 0;
    uint64_t lastSequence = 0;
    uint64_t discardedEntries = 0;
    std::vector<LogEntry> entries;
};

// 所有 producer 可并发追加；UI 只消费冻结副本。静止帧复用同一快照，清空不回退 sequence。
class LogStore
{
public:
    static constexpr size_t MaxEntries = 4096;
    static constexpr size_t MaxBytes = 4 * 1024 * 1024;
    static constexpr size_t MaxMessageBytes = 8192;
    static constexpr size_t MaxSourceBytes = 128;
    static constexpr size_t MaxFileBytes = 1024;

    uint64_t Append(LogSeverity severity, std::string source, std::string message,
        LogLocation location = {})
    {
        Truncate(source, MaxSourceBytes);
        Truncate(message, MaxMessageBytes);
        Truncate(location.file, MaxFileBytes);
        LogEntry entry;
        entry.timestampMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        entry.severity = severity;
        entry.source = std::move(source);
        entry.message = std::move(message);
        entry.location = std::move(location);
        const size_t bytes = TextBytes(entry);
        std::lock_guard<std::mutex> lock(mutex);
        entry.sequence = ++sequence;
        entries.push_back(std::move(entry));
        retainedBytes += bytes;
        while (entries.size() > MaxEntries || retainedBytes > MaxBytes)
        {
            retainedBytes -= TextBytes(entries.front());
            entries.pop_front();
            ++discarded;
        }
        ++revision;
        return sequence;
    }

    std::shared_ptr<const LogSnapshot> Freeze() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!frozen || frozen->revision != revision)
        {
            auto candidate = std::make_shared<LogSnapshot>();
            candidate->revision = revision;
            candidate->lastSequence = sequence;
            candidate->discardedEntries = discarded;
            candidate->entries.assign(entries.begin(), entries.end());
            frozen = std::move(candidate);
        }
        return frozen;
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lock(mutex);
        entries.clear();
        retainedBytes = 0;
        discarded = 0;
        ++revision;
    }

private:
    static size_t TextBytes(const LogEntry& entry)
    {
        return entry.message.size() + entry.source.size() + entry.location.file.size();
    }
    static void Truncate(std::string& text, size_t limit)
    {
        if (text.size() <= limit) return;
        size_t end = limit - 3;
        // UTF-8 continuation 不能被截成新的首字节；ASCII 与合法 UTF-8 均保持可展示。
        while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
        text.resize(end);
        text += "...";
    }
    mutable std::mutex mutex;
    std::deque<LogEntry> entries;
    size_t retainedBytes = 0;
    uint64_t revision = 0;
    uint64_t sequence = 0;
    uint64_t discarded = 0;
    mutable std::shared_ptr<const LogSnapshot> frozen;
};

inline LogStore& ProcessLog()
{
    static LogStore log;
    return log;
}

} // namespace VL
