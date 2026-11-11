#pragma once

#include "ui/widgets/input/inputWidgets.h"

namespace VL::UiWidgets
{
// 文档标签仅呈现激活/关闭意图，脏文档的保存与关闭决策属于业务宿主。
std::string DocumentTab(const std::string& label, const WidgetAction& activate,
    const WidgetAction& close, bool selected, const std::string& icon = {});
// 停靠壳复用同一个标签内容，外层的稳定 id、停靠地址与搬移由宿主拥有。
std::string DocumentTabContent(const std::string& label, const WidgetAction& activate,
    const WidgetAction& close, bool selected, const std::string& icon = {});

// 菜单项只提交动作名和条目索引：展开状态、勾选含义、面板可见性与命令语义都属于业务宿主。
// 勾选栏始终占位，保证有无勾选的行左边界对齐。
std::string MenuItem(const std::string& id, const WidgetAction& action, const std::string& label,
    bool checked, bool disabled);
} // namespace VL::UiWidgets
