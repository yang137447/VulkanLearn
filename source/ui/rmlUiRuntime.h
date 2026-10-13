#pragma once

#include <string>

namespace VL::Ui
{

// RmlUi 的**进程级**生命周期。
//
// 为什么需要单独一个东西：`Rml::Initialise()` 与 `Rml::Shutdown()` 是 RmlUi 的全局状态，
// 而它们此前住在 `UiSubsystem` 里 —— 一个"每个窗口一份"的对象。单窗口时看不出问题，
// 一旦有两个窗口，第二个 `UiSubsystem` 的 Shutdown 会把 RmlUi 从第一个底下抽走。
// 同时 `Rml::CreateContext` 要求 context 名唯一，硬编码的 `"vulkanlearn"` 也会直接撞名。
//
// 因此把"进程级一次"与"每窗口一份"分开：
//   - 本类：全局初始化/关闭，进程内引用计数（测试里可以多次建立与拆除）。
//   - `UiSubsystem`：自己的 context、文档与输入状态，名字按窗口区分。
//
// 用引用计数而不是"第一次初始化、最后不管"：仓库的测试会反复创建与销毁 UI 子系统，
// 只在最后一次关闭才能让 `Rml::Shutdown` 与 `Rml::Initialise` 配对，否则下一次初始化
// 会撞上未清空的全局状态。
class RmlUiRuntime
{
public:
    // 建立进程级 RmlUi 状态；已建立时只增加引用计数。
    static void Acquire();
    // 释放一次引用；计数归零时才真正 `Rml::Shutdown()`。
    static void Release();
    static bool IsInitialized();

    // 每窗口的 context 名。名字必须唯一，而窗口身份是唯一稳定的区分依据。
    static std::string ContextNameFor(size_t windowIndex);
};

} // namespace VL::Ui
