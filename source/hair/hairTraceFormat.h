#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <Eigen/Dense>

// 一次性 GPU hair trace 的文件格式与纯 CPU 读写器。
//
// 这个头文件刻意不依赖 Vulkan：`HairSimulationPass` 只负责把 device-local 的
// current/previous/prevPrev 拷进 host-visible buffer，真正的存储与解析都在这里，
// 因此 `tool/hair-tests` 可以在不创建 Vulkan 设备的情况下做往返/截断测试。
//
// 文件全部使用小端字节序（Windows x64 与 MinGW 目标平台均为小端；Python 侧用
// "<" 格式显式声明，不依赖宿主字节序）。
//
// 布局：
//   Header
//     char[4]  magic = "VLHT"
//     u32      version
//     u32      totalGuideCount      对象的总 guide 数（不是选中数）
//     u32      pointsPerGuide
//     u32      stride               guide 选择步长
//     u32      selectedCount        <= 256
//     u64      startFrame           第一帧的 RenderScene::frameIndex
//     u32[selectedCount]            selectedGuides
//     f32[selectedCount * pointsPerGuide] restLengths（末点为 0）
//   Frame（重复 frames 次）
//     u64      frameIndex
//     u64      worldGeneration
//     u32      substeps
//     f32      timeStep
//     u32      resetReason          VL::Hair::GuideSimulationResetReason
//     u32      dispatchCount        该对象本帧实际录制的 dispatch 数
//     f32[16]  hairModel            该帧 hair 的 model（列主序，世界 = model * local）
//     f32      sdfPenetrationMaxCm  碰撞前 SDF 内部最大穿透深度（0=无）
//     f32      capsulePenetrationMaxCm 碰撞前胶囊内部最大穿透深度（0=无）
//     sample[selectedCount * pointsPerGuide]
//       每个 sample 固定 3 个 float4：current、previous、prevPrevious。
//       顺序是 guide 外层、点中层、buffer 内层（见 SampleOrder 注释）。
namespace VL::Hair
{
inline constexpr std::array<uint8_t, 4> HairTraceMagic{'V', 'L', 'H', 'T'};
inline constexpr uint32_t HairTraceVersion = 3;
// 与 Console 命令的 guide 上限一致；超过后 trace 只记录前 256 个选中 guide。
inline constexpr uint32_t HairTraceMaxSelectedGuides = 256;
// 防御性上限：只拒绝明显损坏的头，不限制正式资产的点数（当前资产为 32）。
inline constexpr uint32_t HairTraceMaxPointsPerGuide = 4096;

// Console `hair trace <path> <frames> <guide-stride>` 的值语义请求。
// RenderSystem 在 GT 侧接收，渲染线程在 Record 中消费；这里只放纯数据。
struct HairTraceRequest
{
    std::string path;
    uint32_t frames = 0;
    uint32_t guideStride = 0;
};

struct HairTraceHeader
{
    uint32_t version = HairTraceVersion;
    uint32_t totalGuideCount = 0;
    uint32_t pointsPerGuide = 0;
    uint32_t stride = 0;
    uint32_t selectedCount = 0;
    uint64_t startFrame = 0;
    std::vector<uint32_t> selectedGuides;
    // 选中 guide 的静止段长，布局 [selectedIndex][point]；末项为 0。
    std::vector<float> restLengths;

    // 返回 [selectedIndex][point] 的样本下标。
    uint64_t SampleCount() const
    {
        return static_cast<uint64_t>(selectedCount) * pointsPerGuide;
    }
};

struct HairTraceFrameInfo
{
    uint64_t frameIndex = 0;
    uint64_t worldGeneration = 0;
    uint32_t substeps = 0;
    float timeStep = 0.0f;
    uint32_t resetReason = 0;
    uint32_t dispatchCount = 0;
    // 该帧 hair 的 model；local positions 乘它得到世界坐标，供 placement
    // 惯性/根点轨迹离线分析。默认单位矩阵（旧调用者/纯格式测试）。
    Eigen::Matrix4f hairModel = Eigen::Matrix4f::Identity();
    // 碰撞 pass 在投影前记录的最大内部穿透深度（厘米）；0 表示没有点进入
    // SDF/胶囊内部。它们不是 margin 缓冲层厚度。
    float sdfPenetrationMaxCm = 0.0f;
    float capsulePenetrationMaxCm = 0.0f;
};

// 一个样本由三个 float4 组成；文件里按 guide -> point -> (current, previous,
// prevPrevious) 交错写入。readback buffer 本身按 state -> guide -> point 连续存放
// （vkCmdCopyBuffer 只能拷贝连续区间），所以 Writer 会做一次 CPU 侧的 48-byte
// 交错整理，避免为每个点单独发起一次 GPU copy。
struct HairTraceSample
{
    Eigen::Vector4f current = Eigen::Vector4f::Zero();
    Eigen::Vector4f previous = Eigen::Vector4f::Zero();
    Eigen::Vector4f previousPrevious = Eigen::Vector4f::Zero();
};

enum class HairTraceReadResult
{
    Frame,
    EndOfFile,
    Error,
};

class HairTraceWriter
{
public:
    HairTraceWriter() = default;
    ~HairTraceWriter();
    HairTraceWriter(const HairTraceWriter&) = delete;
    HairTraceWriter& operator=(const HairTraceWriter&) = delete;

    // 创建/截断文件并写 header。失败时 error 非空且返回 false。
    bool Open(std::string path, const HairTraceHeader& header, std::string& error);
    // current/previous/previousPrevious 各指向 state -> guide -> point 的连续
    // float 数据，元素数为 SampleCount()*4。Writer 负责交错成文件布局。
    bool WriteFrame(const HairTraceFrameInfo& info,
        const float* current,
        const float* previous,
        const float* previousPrevious,
        std::string& error);
    bool Close(std::string& error);
    bool IsOpen() const { return stream.is_open(); }
    const std::string& Path() const { return path; }
    uint64_t BytesWritten() const { return bytesWritten; }

private:
    bool WriteRaw(const void* data, size_t bytes, std::string& error);

    std::ofstream stream;
    std::string path;
    HairTraceHeader header;
    std::vector<float> frameScratch;
    uint64_t bytesWritten = 0;
    bool failed = false;
};

class HairTraceReader
{
public:
    HairTraceReader() = default;
    ~HairTraceReader();
    HairTraceReader(const HairTraceReader&) = delete;
    HairTraceReader& operator=(const HairTraceReader&) = delete;

    // 读取并校验 header；失败时返回 Error 并填 error。
    HairTraceReadResult Open(std::string path, HairTraceHeader& outHeader, std::string& error);
    // 读取下一帧；EOF 返回 EndOfFile，截断/坏数据返回 Error。
    HairTraceReadResult ReadFrame(HairTraceFrameInfo& info,
        std::vector<HairTraceSample>& samples,
        std::string& error);

private:
    HairTraceReadResult ReadExact(void* destination, size_t bytes, std::string& error);

    std::ifstream stream;
    HairTraceHeader header;
    uint64_t remainingFrames = 0;
    bool opened = false;
};

// 供 UI/日志复用的原因名；与 GuideSimulationResetReasonName 保持一致的取值集合。
const char* HairTraceResetReasonName(uint32_t reason);

} // namespace VL::Hair
