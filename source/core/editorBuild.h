#pragma once

// UE 式编辑器/运行时边界：编辑器可执行文件显式定义 VL_WITH_EDITOR=1，
// 纯运行时目标不定义该宏。运行时代码不得依赖 source/editor/**。
// 新代码只有在“字段必须随对象生命周期存在、且确实只服务编辑器”时才使用
// VL_EDITOR_ONLY；优先把编辑器数据放到 source/editor 下的独立 DTO。
#ifndef VL_WITH_EDITOR
#define VL_WITH_EDITOR 0
#endif

#if VL_WITH_EDITOR
#define VL_EDITOR_ONLY(...) __VA_ARGS__
#else
#define VL_EDITOR_ONLY(...)
#endif
