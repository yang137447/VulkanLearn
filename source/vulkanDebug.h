#pragma once
#include <vulkan/vulkan.hpp>
#include <string>
#include <array>

class VulkanDebug
{
public:
    /**
     * @brief 初始化调试工具，加载 Vulkan 扩展函数指针
     * @param instance Vulkan 实例
     */
    static void Init(vk::Instance instance);

    /**
     * @brief 设置 Vulkan 对象名称（在 RenderDoc 等调试工具中显示）
     * @param device Vulkan 逻辑设备
     * @param object 对象句柄（需转换为 uint64_t）
     * @param objectType 对象类型
     * @param name 对象名称
     */
    static void SetObjectName(vk::Device device, uint64_t object, vk::ObjectType objectType, const std::string& name);

    /**
     * @brief 模板辅助函数，自动处理 handle 类型转换
     */
    template <typename T>
    static void SetObjectName(vk::Device device, T object, vk::ObjectType objectType, const std::string& name)
    {
        SetObjectName(device, (uint64_t)(typename T::CType)object, objectType, name);
    }

    enum class DebugCategory
    {
        ePass,        // RenderPass 级别，建议用显眼的颜色（如绿色）
        eObject,      // 绘制物体级别，建议用蓝色
        ePipeline,    // Pipeline 绑定、Barrier 等，建议用黄色
        eResource,    // 资源加载、拷贝等，建议用青色
        eWait,        // 同步等待，建议用红色
        eDefault      // 默认白色
    };

    /**
     * @brief RAII 风格的调试区域标记
     * 
     * 使用示例：
     * {
     *     VulkanDebug::ScopedRegion region(cmd, "Render Pass", VulkanDebug::DebugCategory::ePass);
     *     // ... 记录命令 ...
     * } // region 析构时自动调用 EndRegion
     */
    struct ScopedRegion
    {
        /**
         * @brief 构造函数：自动开始调试区域
         * @param cmd 命令缓冲区
         * @param labelName 区域名称
         * @param category 区域类型（决定颜色）
         */
        ScopedRegion(vk::CommandBuffer cmd, const std::string& labelName, DebugCategory category = DebugCategory::eDefault)
            : cmd(cmd)
        {
            BeginRegion(cmd, labelName, category);
        }

        /**
         * @brief 析构函数：自动结束调试区域
         */
        ~ScopedRegion()
        {
            EndRegion(cmd);
        }

        // 禁用拷贝和赋值，防止多次调用 EndRegion
        ScopedRegion(const ScopedRegion&) = delete;
        ScopedRegion& operator=(const ScopedRegion&) = delete;

    private:
        vk::CommandBuffer cmd;
    };

private:
    // 私有化 Begin/End Region，强制使用 RAII 模式的 ScopedRegion
    static void BeginRegion(vk::CommandBuffer cmd, const std::string& labelName, DebugCategory category);
    static void EndRegion(vk::CommandBuffer cmd);

    static std::array<float, 4> GetColor(DebugCategory category);

    static PFN_vkCmdBeginDebugUtilsLabelEXT pfnCmdBeginDebugUtilsLabelEXT;
    static PFN_vkCmdEndDebugUtilsLabelEXT pfnCmdEndDebugUtilsLabelEXT;
    static PFN_vkSetDebugUtilsObjectNameEXT pfnSetDebugUtilsObjectNameEXT;
};
