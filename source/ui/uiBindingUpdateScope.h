#pragma once

namespace VL
{

// RmlUi 的程序赋值也会同步触发 change；作用域内的回调不能回写引擎快照。
// 支持热重载的嵌套更新，并在异常退出时恢复外层状态。
class UiBindingUpdateScope
{
public:
    explicit UiBindingUpdateScope(bool& updating)
        : updating(updating), previous(updating)
    {
        updating = true;
    }

    ~UiBindingUpdateScope()
    {
        updating = previous;
    }

    UiBindingUpdateScope(const UiBindingUpdateScope&) = delete;
    UiBindingUpdateScope& operator=(const UiBindingUpdateScope&) = delete;

private:
    bool& updating;
    bool previous;
};

} // namespace VL
