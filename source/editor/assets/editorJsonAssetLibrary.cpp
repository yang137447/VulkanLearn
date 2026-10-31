#include "editorJsonAssetLibrary.h"
#include "editorRetargetPreview.h"

#include <algorithm>
#include <condition_variable>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <stb_image.h>
#include "resource/image/textureIO.h"

namespace VL::Editor
{
namespace
{
using Json = nlohmann::json;
using Kind = EditorUi::EditorAssetKind;
using ValueKind = EditorUi::EditorJsonValueKind;
using Document = EditorUi::EditorJsonDocumentSnapshot;
using Catalog = EditorUi::EditorJsonAssetCatalogSnapshot;
using Limits = EditorJsonAssetLimits;

std::string LowerAscii(std::string text)
{
    for (char& ch : text)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    return text;
}

std::string NormalizeSeparators(std::string_view text)
{
    std::string result(text);
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

bool SkipDirectory(std::string_view name)
{
    const std::string lower = LowerAscii(std::string(name));
    return lower == ".git" || lower == "generated" || lower == "data";
}

std::string BoundedText(std::string_view text)
{
    if (text.size() <= Limits::MaxTextBytes) return std::string(text);
    size_t end = Limits::MaxTextBytes;
    // 不截断 UTF-8 连续字节，中文标签/说明在 UI 解码时仍保持有效。
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
    return std::string(text.substr(0, end)) + "...";
}

void AddDiagnostic(std::string& diagnostic, std::string_view text)
{
    if (diagnostic.size() >= Limits::MaxTextBytes) return;
    if (!diagnostic.empty()) diagnostic += "; ";
    diagnostic += BoundedText(text);
    diagnostic = BoundedText(diagnostic);
}

std::optional<Kind> FilenameKind(std::string_view assetPath)
{
    const auto path = std::filesystem::u8path(NormalizeSeparators(assetPath));
    if (LowerAscii(path.extension().u8string()) != ".json") return std::nullopt;
    const std::string name = LowerAscii(path.stem().u8string());
    if (name.compare(0, 3, "mi_") == 0) return Kind::MaterialInstance;
    if (name.compare(0, 3, "sm_") == 0) return Kind::Mesh;
    if (name.compare(0, 3, "tf_") == 0) return Kind::Tressfx;
    if (name.compare(0, 3, "sc_") == 0) return Kind::Scene;
    if (name.compare(0, 2, "t_") == 0) return Kind::Texture;
    if (name.compare(0, 3, "tr_") == 0) return Kind::Terrain;
    if (name.compare(0, 3, "bp_") == 0) return Kind::Blueprint;
    if (name.compare(0, 3, "rt_") == 0) return Kind::Retarget;
    // 兼容旧场景文件名；只看 basename，不能把 SC_ 目录下的其他 JSON 当场景。
    if (name.compare(0, 5, "scene") == 0) return Kind::Scene;
    return std::nullopt;
}

std::optional<Kind> TypeKind(const std::string& text)
{
    const std::string lower = LowerAscii(text);
    if (lower == "materialinstance") return Kind::MaterialInstance;
    // 真实 SM_speedtree 资产以 speedtree 为顶层 type；浏览时归入模型，不读取其 SDK data。
    if (lower == "mesh" || lower == "speedtree") return Kind::Mesh;
    if (lower == "tressfx") return Kind::Tressfx;
    if (lower == "scene") return Kind::Scene;
    if (lower == "texture") return Kind::Texture;
    if (lower == "terrain") return Kind::Terrain;
    if (lower == "blueprint") return Kind::Blueprint;
    return std::nullopt;
}

std::string PointerToken(const std::string& text)
{
    std::string result;
    for (char ch : text)
    {
        if (ch == '~') result += "~0";
        else if (ch == '/') result += "~1";
        else result += ch;
    }
    return result;
}

// SAX 只保留展示行与顶层分类信息；data 子树仅通过语法校验，不构造 DOM 或保存内容。
class AssetSax final : public nlohmann::json_sax<Json>
{
public:
    AssetSax(Document& output, bool fields) : document(output), includeFields(fields) {}

    bool null() override { return Scalar(ValueKind::Null, "null"); }
    bool boolean(bool value) override { return Scalar(ValueKind::Boolean, value ? "true" : "false"); }
    bool number_integer(number_integer_t value) override
    {
        return Scalar(ValueKind::Number, std::to_string(value));
    }
    bool number_unsigned(number_unsigned_t value) override
    {
        return Scalar(ValueKind::Number, std::to_string(value));
    }
    bool number_float(number_float_t, const string_t& token) override
    {
        return Scalar(ValueKind::Number, token);
    }
    bool string(string_t& value) override { return Scalar(ValueKind::String, value); }
    bool binary(binary_t&) override { return false; }
    bool start_object(size_t) override { return Start(ValueKind::Object); }
    bool start_array(size_t) override { return Start(ValueKind::Array); }
    bool end_object() override { stack.pop_back(); return true; }
    bool end_array() override { stack.pop_back(); return true; }
    bool key(string_t& value) override
    {
        if (stack.empty()) return true;
        stack.back().key = value;
        // `/simulation/skinning` 是 tressfx 绑定声明；只在 root 下的 simulation
        // 对象里认这一层，不累计完整键路径。type 由 Metadata 单独记录。
        if (!simulationSkinning && stack.size() == 2u &&
            stack.back().kind == ValueKind::Object &&
            stack.back().label == "simulation" && value == "skinning")
            simulationSkinning = true;
        return true;
    }
    bool parse_error(size_t position, const std::string&, const nlohmann::detail::exception&) override
    {
        // 不回显 lexer token，损坏的 data 或超长输入不能通过异常文本泄漏到 UI。
        AddDiagnostic(document.diagnostic, "Invalid JSON at byte " + std::to_string(position));
        return false;
    }

    bool typeSeen = false;
    bool typeIsString = false;
    std::string type;
    std::string modelDataPath;
    // root.simulation.skinning 是否存在；与 type == "tressfx" 一起决定毛发视口守卫。
    bool simulationSkinning = false;

private:
    struct Frame
    {
        ValueKind kind;
        int row;
        bool omitted;
        std::string path;
        std::string key;
        // 引入该对象的键（数组元素为空）；只用于识别 /simulation/skinning，
        // 不累计完整路径。
        std::string label;
        size_t nextIndex = 0;
    };
    struct Location
    {
        int parent = -1;
        uint32_t depth = 0;
        bool omitted = false;
        std::string path;
        std::string label = "$";
    };

    Location NextLocation()
    {
        Location location;
        location.depth = static_cast<uint32_t>(stack.size());
        if (stack.empty()) return location;
        Frame& parent = stack.back();
        location.parent = parent.row;
        location.omitted = parent.omitted;
        if (parent.kind == ValueKind::Object)
        {
            location.label = parent.key;
            location.omitted = location.omitted || LowerAscii(parent.key) == "data";
        }
        else location.label = std::to_string(parent.nextIndex++);
        // 略过子树不累计路径/值；长键保留完整 JSON Pointer（总量受输入字节数限制）。
        if (!location.omitted) location.path = parent.path + "/" + PointerToken(location.label);
        return location;
    }

    void Metadata(const Location& location, ValueKind kind, const std::string& value)
    {
        if (stack.size() != 1 || stack.front().kind != ValueKind::Object) return;
        if (location.label == "type")
        {
            typeSeen = true;
            typeIsString = kind == ValueKind::String;
            type = typeIsString ? value : "";
        }
        else if (location.label == "name" && kind == ValueKind::String)
            document.name = BoundedText(value);
        else if (location.label == "description" && kind == ValueKind::String)
            document.description = BoundedText(value);
        // tressfx 的 .tfx 相对路径；worker 用它读取 guideCount，扫描目录时不打开模型。
        else if (location.label == "modelDataPath" && kind == ValueKind::String)
            modelDataPath = value;
        else if (location.label == "source" && includeFields && kind == ValueKind::String)
            document.textureSource = value;
        else if (location.label == "colorSpace" && includeFields && kind == ValueKind::String)
            document.textureColorSpace = value;
    }

    int AddField(const Location& location, ValueKind kind, const std::string& value)
    {
        if (!includeFields || location.omitted) return -1;
        if (document.fields.size() == Limits::MaxFields)
        {
            if (!fieldsTruncated)
                AddDiagnostic(document.diagnostic, "Field row limit (4096) reached; remaining fields omitted");
            fieldsTruncated = true;
            return -1;
        }
        const int row = static_cast<int>(document.fields.size());
        document.fields.push_back({location.parent, location.depth, location.path,
            BoundedText(location.label), BoundedText(value), kind});
        return row;
    }

    bool Start(ValueKind kind)
    {
        if (stack.size() >= Limits::MaxDepth)
        {
            AddDiagnostic(document.diagnostic, "JSON depth limit (64) exceeded");
            return false;
        }
        Location location = NextLocation();
        if (!CheckLocation(location)) return false;
        if (stack.empty() && kind != ValueKind::Object)
            AddDiagnostic(document.diagnostic, "Asset JSON root must be an object");
        Metadata(location, kind, "");
        const int row = AddField(location, kind, kind == ValueKind::Object ? "{...}" : "[...]");
        stack.push_back({kind, row, location.omitted, std::move(location.path), {},
            kind == ValueKind::Object ? location.label : std::string(), 0});
        return true;
    }

    bool Scalar(ValueKind kind, const std::string& value)
    {
        Location location = NextLocation();
        if (!CheckLocation(location)) return false;
        if (stack.empty()) AddDiagnostic(document.diagnostic, "Asset JSON root must be an object");
        Metadata(location, kind, value);
        AddField(location, kind, value);
        return true;
    }

    bool CheckLocation(const Location& location)
    {
        // 长键与宽数组组合会把同一巨型 Pointer 复制到每行，必须在分配展示行前拒绝。
        if (location.path.size() <= Limits::MaxFieldPathBytes) return true;
        AddDiagnostic(document.diagnostic, "JSON field path byte limit (4096) exceeded");
        return false;
    }

    Document& document;
    bool includeFields;
    bool fieldsTruncated = false;
    std::vector<Frame> stack;
};

bool PhysicalWithinRoot(const std::filesystem::path& root, const std::filesystem::path& path)
{
    auto filePart = path.begin();
    for (auto rootPart = root.begin(); rootPart != root.end(); ++rootPart, ++filePart)
    {
        if (filePart == path.end()) return false;
#ifdef _WIN32
        if (LowerAscii(rootPart->u8string()) != LowerAscii(filePart->u8string())) return false;
#else
        if (*rootPart != *filePart) return false;
#endif
    }
    for (; filePart != path.end(); ++filePart)
        if (SkipDirectory(filePart->u8string())) return false;
    return true;
}

bool ResolveJson(const std::filesystem::path& root, const std::string& assetPath,
    std::filesystem::path& physicalPath, std::string& error)
{
    if (!ValidateEditorJsonAssetPath(assetPath, error)) return false;
    std::error_code ec;
    physicalPath = std::filesystem::canonical(root / std::filesystem::u8path(assetPath), ec);
    if (ec)
    {
        error = "JSON file cannot be resolved";
        return false;
    }
    if (!PhysicalWithinRoot(root, physicalPath))
    {
        error = "JSON path escapes resourceRoot or resolves into an excluded directory";
        return false;
    }
    if (LowerAscii(physicalPath.extension().u8string()) != ".json")
    {
        error = "JSON alias resolves to a non-JSON file";
        return false;
    }
    if (!std::filesystem::is_regular_file(physicalPath, ec) || ec)
    {
        error = "JSON path is not a regular file";
        return false;
    }
    return true;
}

bool ReadJson(const std::filesystem::path& path, std::string& text, std::string& error)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) { error = "Cannot query JSON file size"; return false; }
    if (size > Limits::MaxFileBytes)
    {
        error = "JSON file byte limit (4 MiB) exceeded";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) { error = "Cannot read JSON file"; return false; }
    // 多读一个字节即可发现扫描后增长的文件，不依赖不可信的 file_size 分配无限缓冲。
    text.resize(static_cast<size_t>(size) + 1);
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    const auto read = static_cast<size_t>(input.gcount());
    if (input.bad()) { error = "JSON read failed"; return false; }
    if (read > size)
    {
        error = "JSON file changed while reading; refresh again";
        return false;
    }
    text.resize(read);
    return true;
}

bool EntryOrder(const EditorUi::EditorJsonAssetEntry& first, const EditorUi::EditorJsonAssetEntry& second)
{
    return first.assetPath < second.assetPath;
}

bool SourceLineLimitExceeded(std::string_view text)
{
    size_t lines = 1;
    for (size_t index = 0; index < text.size(); ++index)
    {
        const bool lineEnd = text[index] == '\n' ||
            (text[index] == '\r' && (index + 1 == text.size() || text[index + 1] != '\n'));
        if (lineEnd && ++lines > Limits::MaxSourceLines) return true;
    }
    return false;
}

} // namespace

bool ValidateEditorJsonAssetPath(std::string_view assetPath, std::string& error)
{
    error.clear();
    const std::string text = NormalizeSeparators(assetPath);
    if (text.empty() || text.front() == '/' || text.find(':') != std::string::npos ||
        text.find('\0') != std::string::npos)
    {
        error = "Asset path must be resourceRoot-relative (no absolute path, drive, ADS or NUL)";
        return false;
    }
    try
    {
        const auto path = std::filesystem::u8path(text);
        if (path.has_root_path() || LowerAscii(path.extension().u8string()) != ".json")
        {
            error = "Only relative .json paths are supported";
            return false;
        }
        size_t depth = 0;
        for (auto part = path.begin(); part != path.end(); ++part)
        {
            if (*part == "..")
            {
                error = "Parent traversal is not allowed";
                return false;
            }
            if (++depth > Limits::MaxDepth ||
                (std::next(part) != path.end() && SkipDirectory(part->u8string())))
            {
                error = "Asset directory is excluded or path depth limit (64) exceeded";
                return false;
            }
        }
    }
    catch (const std::exception&)
    {
        // UI 路径是 UTF-8；平台路径转换失败也必须在请求边界变成诊断，不能逃入 GT。
        error = "Asset path cannot be converted to a filesystem path";
        return false;
    }
    return true;
}

bool ParseEditorJsonAsset(std::string_view assetPath, std::string_view jsonText,
    Document& document, bool includeFields, std::string* tressfxModelDataPath)
{
    document = {};
    if (!ValidateEditorJsonAssetPath(assetPath, document.diagnostic)) return false;
    document.assetPath = std::filesystem::u8path(NormalizeSeparators(assetPath)).lexically_normal().generic_u8string();
    document.name = BoundedText(std::filesystem::u8path(document.assetPath).stem().u8string());
    const auto inferred = FilenameKind(document.assetPath);
    if (inferred) document.kind = *inferred;
    if (jsonText.size() > Limits::MaxFileBytes)
    {
        document.diagnostic = "JSON file byte limit (4 MiB) exceeded";
        return inferred.has_value();
    }
    if (SourceLineLimitExceeded(jsonText))
    {
        document.diagnostic = "JSON source line limit (100000) exceeded";
        return inferred.has_value();
    }
    AssetSax sax(document, includeFields);
    const bool valid = Json::sax_parse(jsonText.begin(), jsonText.end(), &sax);
    // type / simulation.skinning 是毛发视口与目录共用的作者摘要；损坏 JSON 也保留
    // 已解析到的 type，让错误候选仍能按资产类型呈现。
    if (sax.typeSeen && sax.typeIsString) document.type = sax.type;
    document.tressfxSkinning =
        sax.typeIsString && sax.type == "tressfx" && sax.simulationSkinning;
    if (tressfxModelDataPath != nullptr) *tressfxModelDataPath = sax.modelDataPath;
    if (!valid)
    {
        // 不展示损坏输入的半棵树，但保留已经识别的顶层类型供目录呈现错误候选。
        document.fields.clear();
        if (document.diagnostic.empty()) document.diagnostic = "Invalid JSON";
    }
    if (sax.typeSeen && sax.typeIsString)
    {
        const auto explicitKind = TypeKind(sax.type);
        if (!explicitKind)
        {
            AddDiagnostic(document.diagnostic, "Unsupported top-level asset type");
            document.fields.clear();
            return false;
        }
        document.kind = *explicitKind;
        if (inferred && *inferred != *explicitKind)
            AddDiagnostic(document.diagnostic, "Filename prefix differs from type; top-level type takes precedence");
        return true;
    }
    if (sax.typeSeen) AddDiagnostic(document.diagnostic, "Top-level type must be a string");
    if (inferred) return true;
    AddDiagnostic(document.diagnostic, "Unsupported asset JSON (no supported type or filename prefix)");
    document.fields.clear();
    return false;
}

struct EditorJsonAssetLibrary::Impl
{
    std::filesystem::path root;
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    bool stopping = true;
    bool refreshPending = false;
    bool openPending = false;
    bool catalogLoading = false;
    bool documentLoading = false;
    uint64_t catalogGeneration = 0;
    uint64_t documentGeneration = 0;
    std::string openPath;
    std::shared_ptr<const Catalog> catalog;
    std::shared_ptr<const Document> document;
    std::shared_ptr<const Catalog> readyCatalog;
    std::shared_ptr<const Document> readyDocument;

    bool CurrentCatalog(uint64_t generation)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return !stopping && generation == catalogGeneration;
    }

    bool CurrentDocument(uint64_t generation)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return !stopping && generation == documentGeneration;
    }

    // Groups 编辑器在保存前用 guideCount 拦截 end > guideCount；这里只读 .tfx 头 12 字节。
    // 路径非法、文件缺失或头太短都返回 0（未知），绝不能影响 JSON 本身打开。
    uint32_t ReadTfxGuideCount(const std::string& modelDataPath) const
    {
        const std::string normalized = NormalizeSeparators(modelDataPath);
        if (normalized.empty() || normalized.front() == '/' ||
            normalized.find(':') != std::string::npos ||
            normalized.find('\0') != std::string::npos)
            return 0;
        // u8path 对非法 UTF-8 可能抛异常；读取失败只允许降级为“未知”，不能逃出 worker。
        try
        {
            const auto relative = std::filesystem::u8path(normalized);
            if (relative.is_absolute() || relative.has_root_name() ||
                LowerAscii(relative.extension().u8string()) != ".tfx")
                return 0;
            for (const auto& part : relative)
                if (part == "..") return 0;
            std::error_code ec;
            const auto physical = std::filesystem::canonical(root / relative, ec);
            if (ec || !PhysicalWithinRoot(root, physical) ||
                !std::filesystem::is_regular_file(physical, ec))
                return 0;
            std::ifstream input(physical, std::ios::binary);
            if (!input) return 0;
            // .tfx 头：offset 0 = layout version(f32)，4 = guideCount(u32)，8 = pointsPerStrand(u32)。
            // 只读 12 字节并按 tfxLoader 的规则做最小合理性检查；Git LFS 指针等非二进制内容
            // 会让 version 不满足 >=4，从而返回 0 而不是把 ASCII 字节误当成 guide 数。
            uint8_t header[12] = {};
            input.read(reinterpret_cast<char*>(header), sizeof(header));
            if (input.gcount() != static_cast<std::streamsize>(sizeof(header)))
                return 0;
            float version = 0.0f;
            std::memcpy(&version, header, sizeof(version));
            const uint32_t guideCount = static_cast<uint32_t>(header[4]) |
                (static_cast<uint32_t>(header[5]) << 8) |
                (static_cast<uint32_t>(header[6]) << 16) |
                (static_cast<uint32_t>(header[7]) << 24);
            const uint32_t pointsPerStrand = static_cast<uint32_t>(header[8]) |
                (static_cast<uint32_t>(header[9]) << 8) |
                (static_cast<uint32_t>(header[10]) << 16) |
                (static_cast<uint32_t>(header[11]) << 24);
            constexpr uint32_t MaxTfxVertexCount = 4194304u;
            if (!std::isfinite(version) || version < 4.0f || guideCount == 0 ||
                pointsPerStrand < 4 || pointsPerStrand > 64 ||
                (pointsPerStrand & (pointsPerStrand - 1u)) != 0 ||
                guideCount > MaxTfxVertexCount / pointsPerStrand)
                return 0;
            return guideCount;
        }
        catch (const std::exception&)
        {
            return 0;
        }
    }

    bool Load(const std::string& path, Document& output, bool fields, std::string* raw = nullptr)
    {
        std::filesystem::path physical;
        std::string error;
        std::string text;
        if (ResolveJson(root, path, physical, error) && ReadJson(physical, text, error))
        {
            std::string modelDataPath;
            const bool recognized = ParseEditorJsonAsset(
                path, text, output, fields, &modelDataPath);
            if (raw) *raw = std::move(text);
            if (recognized && output.type == "tressfx" && !modelDataPath.empty())
                output.tressfxGuideCount = ReadTfxGuideCount(modelDataPath);
            return recognized;
        }
        // 读取失败时仍保留有命名前缀的候选；不读取任何被 JSON 引用的文件。
        const bool candidate = ParseEditorJsonAsset(path, {}, output, false);
        output.diagnostic = error;
        return candidate;
    }

    std::shared_ptr<Catalog> Scan(uint64_t generation)
    {
        auto result = std::make_shared<Catalog>();
        result->revision = generation;
        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(root,
            std::filesystem::directory_options::skip_permission_denied, ec);
        const std::filesystem::recursive_directory_iterator end;
        if (ec) AddDiagnostic(result->diagnostic, "Cannot enumerate resourceRoot");
        size_t visited = 0;
        while (!ec && it != end)
        {
            if (!CurrentCatalog(generation)) return {};
            ServicePendingOpen();
            if (++visited > Limits::MaxScanEntries)
            {
                AddDiagnostic(result->diagnostic, "Scan entry limit (100000) exceeded");
                break;
            }
            const auto path = it->path();
            const bool directory = it->is_directory(ec);
            if (ec) break;
            if (directory)
            {
                std::error_code resolveError;
                const auto physical = std::filesystem::canonical(path, resolveError);
                const bool invalidPhysical = resolveError || !PhysicalWithinRoot(root, physical);
                const bool tooDeep = it.depth() >= static_cast<int>(Limits::MaxDepth - 1);
                // 不跟随目录符号链接，避免目录环和重复扫描；junction 的实际路径也必须在根内。
                if (SkipDirectory(path.filename().u8string()) ||
                    tooDeep ||
                    it->is_symlink(resolveError) || resolveError ||
                    invalidPhysical)
                    it.disable_recursion_pending();
                if (tooDeep) AddDiagnostic(result->diagnostic, "Scan directory depth limit (64) reached");
            }
            else if (LowerAscii(path.extension().u8string()) == ".json")
            {
                const std::string relative = path.lexically_relative(root).generic_u8string();
                Document parsed;
                if (Load(relative, parsed, false))
                {
                    result->assets.push_back({parsed.assetPath, parsed.kind, std::move(parsed.name),
                        std::move(parsed.description), std::move(parsed.diagnostic),
                        std::move(parsed.type), parsed.tressfxSkinning,
                        parsed.tressfxGuideCount});
                    if (result->assets.size() == Limits::MaxCatalogEntries)
                    {
                        AddDiagnostic(result->diagnostic, "Catalog entry limit (20000) reached");
                        break;
                    }
                }
            }
            it.increment(ec);
        }
        if (ec) AddDiagnostic(result->diagnostic, "Resource scan stopped after filesystem error");
        std::sort(result->assets.begin(), result->assets.end(), EntryOrder);
        return result;
    }

    void LoadTexturePreview(Document& result)
    {
        // 只在显式打开贴图时读 source；目录扫描和普通 JSON inspector 不解码图片。
        const auto relative = std::filesystem::u8path(NormalizeSeparators(result.textureSource));
        if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
            result.textureSource.find(':') != std::string::npos || result.textureSource.find('\0') != std::string::npos)
        {
            result.previewDiagnostic = "Invalid texture source path";
            return;
        }
        for (const auto& part : relative)
            if (part == "..")
            {
                result.previewDiagnostic = "Texture source escapes resource root";
                return;
            }
        std::error_code ec;
        const auto physical = std::filesystem::canonical(root / relative, ec);
        if (ec || !PhysicalWithinRoot(root, physical) || !std::filesystem::is_regular_file(physical, ec))
        {
            result.previewDiagnostic = "Texture source is missing or outside the resource root";
            return;
        }
        const auto extension = LowerAscii(physical.extension().u8string());
        if (extension != ".png" && extension != ".jpg" && extension != ".jpeg" &&
            extension != ".tga" && extension != ".bmp")
        {
            result.previewDiagnostic = "Preview supports PNG, JPEG, TGA and BMP; unsupported format: " + extension;
            return;
        }
        if (result.textureColorSpace != "srgb" && result.textureColorSpace != "linear")
        {
            result.previewDiagnostic = "Texture colorSpace must be srgb or linear";
            return;
        }
        constexpr size_t MaxBytes = 64 * 1024 * 1024;
        const auto size = std::filesystem::file_size(physical, ec);
        if (ec || size > MaxBytes)
        {
            result.previewDiagnostic = "Texture source exceeds preview input limit (64 MiB)";
            return;
        }
        std::ifstream file(physical, std::ios::binary);
        std::vector<uint8_t> bytes(static_cast<size_t>(size) + 1);
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        const auto count = static_cast<size_t>(file.gcount());
        if (!file.eof() || count != size)
        {
            result.previewDiagnostic = "Texture source changed or cannot be read";
            return;
        }
        int width = 0, height = 0, channels = 0;
        // 在解码分配前检查尺寸；内存解码使用同一份字节，避免检查和加载之间文件被替换。
        if (!stbi_info_from_memory(bytes.data(), static_cast<int>(count), &width, &height, &channels) ||
            width <= 0 || height <= 0 || uint64_t(width) * uint64_t(height) > 32 * 1024 * 1024 ||
            stbi_is_hdr_from_memory(bytes.data(), static_cast<int>(count)))
        {
            result.previewDiagnostic = "Invalid image or preview pixel limit exceeded (32 MP)";
            return;
        }
        TextureIO::LoadOptions options;
        options.forceChannels = 4;
        auto decoded = TextureIO::LoadFromMemory(bytes.data(), count, options);
        if (!decoded || decoded->format != HostImage::PixelFormat::RGBA8_UNORM)
        {
            result.previewDiagnostic = "Texture image decoding failed";
            return;
        }
        auto preview = std::make_shared<EditorUi::EditorTexturePreview>();
        preview->sourceWidth = decoded->width;
        preview->sourceHeight = decoded->height;
        const double scale = std::min(1.0, 1024.0 / std::max(width, height));
        preview->width = std::max(1u, uint32_t(width * scale));
        preview->height = std::max(1u, uint32_t(height * scale));
        preview->pixels.resize(size_t(preview->width) * preview->height * 4);
        float linearValues[256];
        for (size_t index = 0; index < 256; ++index)
        {
            const float v = index / 255.0f;
            linearValues[index] = result.textureColorSpace == "linear" ? v :
                v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
        }
        // 面积平均避免缩略图跳采样产生摩尔纹；先在线性域平均，再编码为 UI 使用的 sRGB。
        for (uint32_t y = 0; y < preview->height; ++y)
            for (uint32_t x = 0; x < preview->width; ++x)
            {
                const uint32_t x0 = uint64_t(x) * width / preview->width;
                const uint32_t x1 = uint64_t(x + 1) * width / preview->width;
                const uint32_t y0 = uint64_t(y) * height / preview->height;
                const uint32_t y1 = uint64_t(y + 1) * height / preview->height;
                float sum[3] = {};
                for (uint32_t sy = y0; sy < y1; ++sy)
                    for (uint32_t sx = x0; sx < x1; ++sx)
                        for (size_t c = 0; c < 3; ++c)
                        {
                            sum[c] += linearValues[decoded->data[(size_t(sy) * width + sx) * 4 + c]];
                        }
                const size_t offset = (size_t(y) * preview->width + x) * 4;
                for (size_t c = 0; c < 3; ++c)
                {
                    const float v = sum[c] / float((x1 - x0) * (y1 - y0));
                    const float encoded = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
                    preview->pixels[offset + c] = uint8_t(std::clamp(std::lround(encoded * 255), 0l, 255l));
                }
                preview->pixels[offset + 3] = 255;
            }
        result.texturePreview = std::move(preview);
    }

    void Open(uint64_t generation, const std::string& path)
    {
        auto result = std::make_shared<Document>();
        std::string raw;
        try
        {
            Load(path, *result, true, &raw);
        }
        catch (const std::exception&)
        {
            result->assetPath = path;
            result->fields.clear();
            result->diagnostic = "JSON document read/parse failed";
        }
        if (result->kind == Kind::Texture && result->diagnostic.empty() && CurrentDocument(generation))
        {
            // 图片失败不能清掉仍然有效的 JSON Details。
            try { LoadTexturePreview(*result); }
            catch (const std::exception&) { result->previewDiagnostic = "Texture preview decoding failed"; }
        }
        if (result->kind == Kind::Retarget && result->diagnostic.empty() && CurrentDocument(generation))
        {
            try { LoadRetargetPreview(raw, *result); }
            catch (const std::exception& error)
            {
                result->retargetPreview.reset();
                result->previewDiagnostic = BoundedText(error.what());
            }
        }
        result->revision = generation;
        std::shared_ptr<const Document> discarded;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!stopping && generation == documentGeneration)
            {
                discarded = std::move(readyDocument);
                readyDocument = std::move(result);
            }
        }
    }

    void LoadRetargetPreview(const std::string& raw, Document& result)
    {
        // worker 只验证引用路径/参考骨架；实际网格及 sourceModel 仍走 GT 的正式加载事务。
        // 复用目录 worker 的路径/字节边界；扫描目录时不跟随这些引用。
        std::string text, error;
        std::filesystem::path physical;
        const Json profile = Json::parse(raw);
        if (!profile.contains("preview") || !profile.at("preview").is_object())
            throw std::runtime_error("Reference preview requires preview.sourceSkeletonPath and targetSkeletonPath");
        const auto& preview = profile.at("preview");
        for (const auto& item : preview.items())
            if (item.key() != "sourceSkeletonPath" && item.key() != "targetSkeletonPath" &&
                item.key() != "targetModelPath")
                throw std::runtime_error("Unknown retarget preview field: " + item.key());
        Json skeletons[2];
        size_t index = 0;
        for (const char* key : {"sourceSkeletonPath", "targetSkeletonPath"})
        {
            if (!preview.contains(key) || !preview.at(key).is_string())
                throw std::runtime_error(std::string("Missing preview.") + key);
            if (!ResolveJson(root, preview.at(key).get<std::string>(), physical, error) ||
                !ReadJson(physical, text, error))
                throw std::runtime_error(std::string(key) + ": " + error);
            Document checked;
            // SK 不属于目录分类，但同样执行 SAX 深度/行数预算后才构造 JSON DOM。
            if (SourceLineLimitExceeded(text))
                throw std::runtime_error("JSON source line limit (100000) exceeded");
            AssetSax sax(checked, false);
            const bool valid = Json::sax_parse(text.begin(), text.end(), &sax);
            if (!valid || !checked.diagnostic.empty())
                throw std::runtime_error(checked.diagnostic.empty() ? "Invalid preview skeleton JSON" : checked.diagnostic);
            skeletons[index++] = Json::parse(text);
        }
        auto values = BuildEditorRetargetPreview(profile, skeletons[0], skeletons[1]);
        if (preview.contains("targetModelPath"))
        {
            if (!preview.at("targetModelPath").is_string() ||
                !ResolveJson(root, preview.at("targetModelPath").get<std::string>(), physical, error))
                throw std::runtime_error("Invalid preview.targetModelPath: " + error);
            values.targetModelPath = preview.at("targetModelPath").get<std::string>();
        }
        result.retargetPreview = std::make_shared<const EditorUi::EditorRetargetPreview>(std::move(values));
    }

    void ServicePendingOpen()
    {
        uint64_t generation = 0;
        std::string path;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping || !openPending) return;
            generation = documentGeneration;
            path = openPath;
            openPending = false;
        }
        // 扫描在文件之间服务最新打开请求，不必等整个资产库遍历完成。
        if (CurrentDocument(generation)) Open(generation, path);
    }

    void Refresh(uint64_t generation)
    {
        std::shared_ptr<Catalog> result;
        try { result = Scan(generation); }
        catch (const std::exception&)
        {
            result = std::make_shared<Catalog>();
            result->revision = generation;
            result->diagnostic = "JSON catalog scan failed";
        }
        std::shared_ptr<const Catalog> discarded;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!stopping && result && generation == catalogGeneration)
            {
                discarded = std::move(readyCatalog);
                readyCatalog = std::move(result);
            }
        }
    }

    void WorkerMain()
    {
        for (;;)
        {
            uint64_t generation = 0;
            bool open = false;
            std::string path;
            {
                std::unique_lock<std::mutex> lock(mutex);
                while (!stopping && !refreshPending && !openPending) wake.wait(lock);
                if (stopping) return;
                // 打开请求优先；各类只保留一个待办，新路径替换旧路径，不积压点击队列。
                open = openPending;
                if (open)
                {
                    generation = documentGeneration;
                    path = openPath;
                    openPending = false;
                }
                else
                {
                    generation = catalogGeneration;
                    refreshPending = false;
                }
            }
            if (open)
            {
                if (CurrentDocument(generation)) Open(generation, path);
            }
            else Refresh(generation);
        }
    }
};

EditorJsonAssetLibrary::EditorJsonAssetLibrary() : impl(std::make_unique<Impl>()) {}
EditorJsonAssetLibrary::~EditorJsonAssetLibrary() { Shutdown(); }

bool EditorJsonAssetLibrary::Initialize(std::filesystem::path resourceRoot, std::string& error)
{
    Shutdown();
    error.clear();
    std::error_code ec;
    const auto root = std::filesystem::canonical(resourceRoot, ec);
    if (ec || !std::filesystem::is_directory(root, ec) || ec)
    {
        error = "resourceRoot must be an existing directory";
        return false;
    }
    impl->root = root;
    impl->stopping = false;
    try { impl->worker = std::thread(&Impl::WorkerMain, impl.get()); }
    catch (const std::exception&)
    {
        impl->stopping = true;
        error = "Cannot start JSON asset worker";
        return false;
    }
    RequestRefresh();
    return true;
}

void EditorJsonAssetLibrary::Shutdown() noexcept
{
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->stopping = true;
        impl->refreshPending = false;
        impl->openPending = false;
    }
    impl->wake.notify_one();
    if (impl->worker.joinable()) impl->worker.join();
    // join 后旧根目录的结果不可能再进入新会话；已借出的 const 快照仍有效。
    impl->readyCatalog.reset();
    impl->readyDocument.reset();
    impl->catalog.reset();
    impl->document.reset();
    impl->catalogLoading = false;
    impl->documentLoading = false;
    impl->openPath.clear();
    impl->root.clear();
}

void EditorJsonAssetLibrary::RequestRefresh()
{
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (impl->stopping || impl->refreshPending) return;
        ++impl->catalogGeneration;
        impl->refreshPending = true;
        impl->catalogLoading = true;
    }
    impl->wake.notify_one();
}

void EditorJsonAssetLibrary::RequestOpen(const std::string& path)
{
    std::string error;
    std::string normalized = NormalizeSeparators(path);
    const bool valid = ValidateEditorJsonAssetPath(normalized, error);
    if (valid) normalized = std::filesystem::u8path(normalized).lexically_normal().generic_u8string();
    std::shared_ptr<const Document> discarded;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (impl->stopping || (impl->documentLoading && impl->openPath == normalized)) return;
        ++impl->documentGeneration;
        impl->openPath = normalized;
        impl->documentLoading = true;
        impl->openPending = valid;
        if (!valid)
        {
            auto rejected = std::make_shared<Document>();
            rejected->revision = impl->documentGeneration;
            rejected->assetPath = BoundedText(normalized);
            rejected->diagnostic = std::move(error);
            discarded = std::move(impl->readyDocument);
            impl->readyDocument = std::move(rejected);
        }
    }
    impl->wake.notify_one();
}

void EditorJsonAssetLibrary::Poll()
{
    std::shared_ptr<const Catalog> catalog;
    std::shared_ptr<const Document> document;
    bool acceptCatalog = false;
    bool acceptDocument = false;
    {
        // 锁只保护小型请求/完成邮箱；扫描、解析、排序和旧快照销毁均不持此锁。
        std::unique_lock<std::mutex> lock(impl->mutex, std::try_to_lock);
        if (!lock.owns_lock()) return;
        if (impl->readyCatalog)
        {
            catalog = std::move(impl->readyCatalog);
            acceptCatalog = catalog->revision == impl->catalogGeneration;
            if (acceptCatalog) impl->catalogLoading = false;
        }
        if (impl->readyDocument)
        {
            document = std::move(impl->readyDocument);
            acceptDocument = document->revision == impl->documentGeneration;
            if (acceptDocument) impl->documentLoading = false;
        }
    }
    // worker 只能投递，GT 再次核对代际后发布；迟到结果不能覆盖用户的新选择。
    if (acceptCatalog) impl->catalog = std::move(catalog);
    if (acceptDocument) impl->document = std::move(document);
}

std::shared_ptr<const Catalog> EditorJsonAssetLibrary::GetCatalog() const { return impl->catalog; }
std::shared_ptr<const Document> EditorJsonAssetLibrary::GetDocument() const { return impl->document; }
bool EditorJsonAssetLibrary::IsCatalogLoading() const { return impl->catalogLoading; }
bool EditorJsonAssetLibrary::IsDocumentLoading() const { return impl->documentLoading; }

} // namespace VL::Editor
