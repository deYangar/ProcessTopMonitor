#pragma once
// ============================================================
// options_dialog — 插件选项对话框（技术方案 §9，M4）
// 骨架拷贝自 PNM plugin_main.cpp ShowOptionsDialog：动态创建
// （RegisterClass + CreateWindowExW + 自绘布局），无 rc 模板依赖，
// DPI 感知布局。全部逻辑在 TM 主线程（模态消息循环内）。
// ============================================================
#include <windows.h>
#include "PluginInterface.h"

ITMPlugin::OptionReturn PtShowOptionsDialog(HWND hParent);
