#pragma once

// File responsibility: Provides stable BLAKE3-256 content digests and a
// typed, length-delimited canonical field stream for persistent shader IDs.
// The rest of the engine does not depend on the upstream BLAKE3 C API.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace VL
{

struct ContentDigest
{
    static constexpr size_t Size = 32;

    std::array<uint8_t, Size> bytes{};

    bool operator==(const ContentDigest& other) const { return bytes == other.bytes; }
    bool operator!=(const ContentDigest& other) const { return !(*this == other); }

    std::string ToHex() const;
    static ContentDigest FromHex(std::string_view hex);
};

class ContentHasher
{
public:
    ContentHasher();
    ~ContentHasher();
    ContentHasher(ContentHasher&&) noexcept;
    ContentHasher& operator=(ContentHasher&&) noexcept;

    ContentHasher(const ContentHasher&) = delete;
    ContentHasher& operator=(const ContentHasher&) = delete;

    void Update(const void* data, size_t size);
    void Update(std::string_view text);
    ContentDigest Finalize();

    static ContentDigest HashBytes(const void* data, size_t size);
    static ContentDigest HashString(std::string_view text);
    static ContentDigest HashFile(const std::filesystem::path& path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Every field is encoded as:
//   type-tag | name-length | name-bytes | value-length | value-bytes
// Integer lengths and values use fixed-width little-endian encoding.
class CanonicalFieldHasher
{
public:
    explicit CanonicalFieldHasher(std::string_view domain);

    void AddBytes(std::string_view name, const void* data, size_t size);
    void AddString(std::string_view name, std::string_view value);
    void AddDigest(std::string_view name, const ContentDigest& value);
    void AddUInt32(std::string_view name, uint32_t value);
    void AddUInt64(std::string_view name, uint64_t value);
    void AddBool(std::string_view name, bool value);
    ContentDigest Finalize();

private:
    enum class FieldType : uint8_t
    {
        Bytes = 1,
        String = 2,
        Digest = 3,
        UInt32 = 4,
        UInt64 = 5,
        Bool = 6
    };

    void AddFieldHeader(FieldType type, std::string_view name, uint64_t valueSize);
    void AddLittleEndian(uint64_t value, size_t byteCount);

    ContentHasher hasher;
};

} // namespace VL
