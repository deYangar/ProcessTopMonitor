# ProcessTopMonitor

[TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor) 的进程排行插件：把「现在谁在吃资源」做成悬浮窗里的 Top 10 —— CPU、内存、磁盘 IO、GPU、显存五种视图，一窗一指标。与 [ProcessNetMonitor](https://github.com/deYangar/ProcessNetMonitor)（进程网速插件）同形式并存。

磁盘 IO 视图，数据来自 ETW 文件事件，每个进程的真实读写量：

![磁盘 IO 视图](docs/screenshots/screenshot-diskio.png)

其余四种视图（CPU / 内存 / GPU / 显存）：

<p align="center">
  <img src="docs/screenshots/screenshot-cpu.png" width="24%" alt="CPU 视图">
  <img src="docs/screenshots/screenshot-mem.png" width="24%" alt="内存视图">
  <img src="docs/screenshots/screenshot-gpu.png" width="24%" alt="GPU 视图">
  <img src="docs/screenshots/screenshot-vram.png" width="24%" alt="显存视图">
</p>

## 它做什么

- **磁盘 IO 到进程**：ETW FileIo 事件统计每个进程的文件读/写速率，不是把全盘速率按进程分摊。权限不够、ETW 会话开不起来时退回全盘口径，并在窗口底部写明当前口径和原因，不装精确。
- **GPU 与任务管理器同口径**：每个进程取最忙的引擎（3D / Copy / VideoDecode 等），可选只统计部分引擎类型；显存支持「专用」与「专用+共享」两种口径。
- **内存两种口径**：专用提交或工作集，切换即时生效。
- **出现方式随人**：默认悬停 TrafficMonitor 主窗或任务栏 400 ms 弹出、离开 300 ms 收起；也可以 pin 住常驻、拖到任意位置；或开多窗并排（最多 6 窗，每窗固定一种指标，右键增删）。
- **任务栏预览**：TrafficMonitor 的任务栏 item 显示 CPU 第一名进程名和占用，点击 pin / unpin 整组悬浮窗。
- 头部可选叠加宿主温度与 CPU 频率（0.1 GHz 精度）。
- 界面中英双语；崩溃转储与调试日志可在设置里开关，日志与 dump 同住一个目录。

## 安装

1. 先构建出 `ProcessTopMonitor.dll`（64 位）与 `ProcessTopMonitor_x86.dll`（32 位），按 TrafficMonitor 位数选一个
2. 放进 TrafficMonitor 的 `plugins\` 目录
3. 重启 TrafficMonitor，在「插件管理」里能看到它，点「选项」调整设置

配置文件在 `plugins\ProcessTopMonitor\settings.json`，旧版 schema 1/2 配置自动迁移。语言文件在 DLL 旁的 `ProcessTopMonitor\lang\`。

## 构建

```bat
plugin\build_release.bat
```

一次出 x64 与 x86 两个 DLL。静态链接 CRT（`/MT`）、`/std:c++17`，除 TrafficMonitor 插件接口头外零第三方依赖。

脚本头部硬编码了本机的 MSVC / WinSDK 路径（VS2022 BuildTools + Windows SDK 10.0.26100），换环境先改 `MSVC`、`SDK`、`SDKVER` 三个变量。

## 几个设计决定

- **口径优先，降级明说**。磁盘 IO 首选 ETW（管理员权限下可用）；建不起会话就用性能计数器全盘口径，悬浮窗会标注。不存在「看着像精确值、实际是估算」的状态。
- **GPU 不做引擎求和**。一个进程多个引擎同时忙时，求和会重复计入同一份工作（任务管理器也不这么做），所以只取最忙引擎。
- **纯 Win32 自绘**。悬浮窗是 UpdateLayeredWindow 双缓冲面板，无皮肤引擎、无 D3D 依赖；亚克力/透明多档效果可选。
- **崩溃可查**。内置 WER LocalDumps 开关（默认开），dump 落在插件 debug 目录，关掉即撤销注册表值。

## 已知限制

- 磁盘 IO 的按进程精确口径需要 TrafficMonitor 以管理员身份运行；普通权限下为全盘口径
- GPU / 显存按系统分三档：Win10 1709+ 走 PDH；Win8 ~ Win10 1607 走 D3DKMT（引擎口径降级为全部节点求和，界面如实标注）；Win7 隐藏这两个维度，头部摘要保留整机 GPU。G2 / G3 档只有虚拟机实测，未过真机

## 文档

- 技术方案：[docs/技术方案.md](docs/技术方案.md)
- 进度与验收记录：[PROGRESS.md](PROGRESS.md)
- 落地方案：[docs/plans/](docs/plans/)
- x86 宿主验证：[tools/vm_x86_report.md](tools/vm_x86_report.md)
