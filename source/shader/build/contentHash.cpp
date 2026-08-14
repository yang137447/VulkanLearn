#include "shader/build/contentHash.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <utility>

#include <blake3.h>

namespace VL
{
namespace
{

uint8_t DecodeHexDigit(char digit)
{
    if (digit >= '0' && digit <= '9')
    {
        return static_cast<uint8_t>(digit - '0');
    }
    if (digit >= 'a' && digit <= 'f')
    {
        return static_cast<uint8_t>(digit - 'a' + 10);
    }
    if (digit >= 'A' && digit <= 'F')
    {
        return static_cast<uint8_t>(digit - 'A' + 10);
    }
    throw std::runtime_error("Invalid hexadecimal digest");
}

} // namespace

struct ContentHasher::Impl
{
    blake3_hasher state;
    bool finalized = false;
};

std::string ContentDigest::ToHex() const
{
    static constexpr char HexDigits[] = "0123456789abcdef";
    std::string result;
    result.resize(Size * 2);
    for (size_t index = 0; index < bytes.size(); ++index)
    {
        result[index * 2] = HexDigits[bytes[index] >> 4];
        result[index * 2 + 1] = HexDigits[bytes[index] & 0x0f];
    }
    return result;
}

ContentDigest ContentDigest::FromHex(std::string_view hex)
{
    if (hex.size() != Size * 2)
    {
        throw std::runtime_error("BLAKE3-256 digest must contain 64 hexadecimal characters");
    }

    ContentDigest digest;
    for (size_t index = 0; index < digest.bytes.size(); ++index)
    {
        digest.bytes[index] = static_cast<uint8_t>(
            (DecodeHexDigit(hex[index * 2]) << 4) |
            DecodeHexDigit(hex[index * 2 + 1]));
    }
    return digest;
}

ContentHasher::ContentHasher()
    : impl(std::make_unique<Impl>())
{
    blake3_hasher_init(&impl->state);
}

ContentHasher::~ContentHasher() = default;
ContentHasher::ContentHasher(ContentHasher&&) noexcept = default;
ContentHasher& ContentHasher::operator=(ContentHasher&&) noexcept = default;

void ContentHasher::Update(const void* data, size_t size)
{
    if (impl->finalized)
    {
        throw std::runtime_error("Cannot update a finalized BLAKE3 hasher");
    }
    if (size == 0)
    {
        return;
    }
    if (data == nullptr)
    {
        throw std::runtime_error("Cannot hash a null byte range");
    }
    blake3_hasher_update(&impl->state, data, size);
}

void ContentHasher::Update(std::string_view text)
{
    Update(text.data(), text.size());
}

ContentDigest ContentHasher::Finalize()
{
    if (impl->finalized)
    {
        throw std::runtime_error("Cannot finalize a BLAKE3 hasher more than once");
    }

    ContentDigest digest;
    blake3_hasher_finalize(&impl->state, digest.bytes.data(), digest.bytes.size());
    impl->finalized = true;
    return digest;
}

ContentDigest ContentHasher::HashBytes(const void* data, size_t size)
{
    ContentHasher hasher;
    hasher.Update(data, size);
    return hasher.Finalize();
}

ContentDigest ContentHasher::HashString(std::string_view text)
{
    return HashBytes(text.data(), text.size());
}

ContentDigest ContentHasher::HashFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
    {
        throw std::runtime_error("Failed to open file for BLAKE3 hashing: " + path.string());
    }

    ContentHasher hasher;
    std::array<char, 64 * 1024> buffer{};
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize bytesRead = input.gcount();
        if (bytesRead > 0)
        {
            hasher.Update(buffer.data(), static_cast<size_t>(bytesRead));
        }
    }
    if (!input.eof())
    {
        throw std::runtime_error("Failed while hashing file: " + path.string());
    }
    return hasher.Finalize();
}

CanonicalFieldHasher::CanonicalFieldHasher(std::string_view domain)
{
    static constexpr std::array<uint8_t, 4> Magic{{'V', 'L', 'C', 'F'}};
    hasher.Update(Magic.data(), Magic.size());
    AddString("__domain", domain);
}

void CanonicalFieldHasher::AddBytes(
    std::string_view name,
    const void* data,
    size_t size)
{
    AddFieldHeader(FieldType::Bytes, name, size);
    hasher.Update(data, size);
}

void CanonicalFieldHasher::AddString(
    std::string_view name,
    std::string_view value)
{
    AddFieldHeader(FieldType::String, name, value.size());
    hasher.Update(value);
}

void CanonicalFieldHasher::AddDigest(
    std::string_view name,
    const ContentDigest& value)
{
    AddFieldHeader(FieldType::Digest, name, value.bytes.size());
    hasher.Update(value.bytes.data(), value.bytes.size());
}

void CanonicalFieldHasher::AddUInt32(std::string_view name, uint32_t value)
{
    AddFieldHeader(FieldType::UInt32, name, sizeof(value));
    AddLittleEndian(value, sizeof(value));
}

void CanonicalFieldHasher::AddUInt64(std::string_view name, uint64_t value)
{
    AddFieldHeader(FieldType::UInt64, name, sizeof(value));
    AddLittleEndian(value, sizeof(value));
}

void CanonicalFieldHasher::AddBool(std::string_view name, bool value)
{
    const uint8_t encodedValue = value ? 1 : 0;
    AddFieldHeader(FieldType::Bool, name, sizeof(encodedValue));
    hasher.Update(&encodedValue, sizeof(encodedValue));
}

ContentDigest CanonicalFieldHasher::Finalize()
{
    return hasher.Finalize();
}

void CanonicalFieldHasher::AddFieldHeader(
    FieldType type,
    std::string_view name,
    uint64_t valueSize)
{
    const uint8_t typeTag = static_cast<uint8_t>(type);
    hasher.Update(&typeTag, sizeof(typeTag));
    AddLittleEndian(name.size(), sizeof(uint32_t));
    hasher.Update(name);
    AddLittleEndian(valueSize, sizeof(uint64_t));
}

void CanonicalFieldHasher::AddLittleEndian(uint64_t value, size_t byteCount)
{
    std::array<uint8_t, sizeof(uint64_t)> encoded{};
    for (size_t index = 0; index < byteCount; ++index)
    {
        encoded[index] = static_cast<uint8_t>((value >> (index * 8)) & 0xff);
    }
    hasher.Update(encoded.data(), byteCount);
}

} // namespace VL
