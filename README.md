# ProcessTopMonitor

[TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor) 的插件：以悬浮窗形式显示进程资源占用 Top 10 排行——CPU、内存、磁盘 IO、GPU、显存五维度。与 [ProcessNetMonitor](https://github.com/deYangar/ProcessNetMonitor)（进程网速）同形式并存。

**当前状态（2026-10-26 11:00）：M1~M5 全部落地，双架构产物已构建，x64 已部署实机；x86 已在 32 位宿主实测。项目按指示暂停，待 Win7 环境补最后一档验证。**

## 功能

- 五维排行：CPU / 内存（专用提交或工作集）/ 磁盘 IO（ETW FileIo 精确口径，A/B/C 档如实标注）/ GPU / 显存（专用或专用+共享）
- 多窗：最多 6 个悬浮窗，各自绑定指标、IO 视图、行数，位置/指标跨重启恢复
- 停靠跟随：菜单显式停靠到 PNM 详情窗或 TM 主窗，WinEventHook 三段式跟随，拖离 >48px 解除
- 标题栏传感器摘要：可选叠加宿主温度与 CPU 频率（频率为 PDH 自测，0.1GHz 精度）
- 选项对话框：方案 §9 全量配置项、GPU 引擎多选、IO 档位状态与重试
- 任务栏 item：CPU Top1 进程与占用，点击显隐全部悬浮窗

## 构建

```bat
plugin\build_release.bat
```

- 产出 `plugin\ProcessTopMonitor.dll`（x64）与 `plugin\ProcessTopMonitor_x86.dll`（x86）
- `/MT` 静态 CRT、零第三方依赖、链接显式 `/OPT:REF`（项目铁律）

## 部署

1. 关闭 TrafficMonitor（**TM 只能单实例**）
2. 按宿主位数把 DLL 拷到 `<TM>\plugins\ProcessTopMonitor.dll`
3. 提权启动 TM

插件配置：`%APPDATA%\TrafficMonitor\plugins\ProcessTopMonitor\settings.json`（schema 2，windows[] 行级配置）。
语言文件：DLL 旁 `ProcessTopMonitor\lang\`。

## 文档

- 技术方案（v2 拍板版）：[docs/技术方案.md](docs/技术方案.md)
- 落地方案（M3-1/M3-2/M3-3/M4/M5）：[docs/plans/](docs/plans/)
- 进度与验收档案：[PROGRESS.md](PROGRESS.md)
- x86 宿主验证报告：[tools/vm_x86_report.md](tools/vm_x86_report.md)

## 已知限制

- Win7（G3）与 ETW B 档真机验证未做（缺 Win7 环境）
- Win8.1/1607 真实 G2 分支未验证（发布前可选）
- GPU 口径为「每进程最忙引擎」，与任务管理器一致；不再提供引擎求和/截断开关
