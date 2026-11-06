# ProcessTopMonitor

[TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor) 的插件：以悬浮窗形式显示进程资源占用 Top 10 排行——CPU、内存、磁盘 IO、GPU、显存五维度。与 [ProcessNetMonitor](https://github.com/deYangar/ProcessNetMonitor)（进程网速）同形式并存。

**当前状态（2026-11-06）：M1~M6 全部落地。M6 悬浮窗 hover 形态对齐 PNM（hover 弹出/pin/常驻/多窗），双架构产物已构建，待 green 位部署实测。**

## 功能

- 五维排行：CPU / 内存（专用提交或工作集）/ 磁盘 IO（ETW FileIo 精确口径，A/B/C 档如实标注）/ GPU / 显存（专用或专用+共享）
- 出现方式（默认 hover）：鼠标悬停 TM 主窗/任务栏窗 400ms 弹出，离开 300ms 隐藏；可切换常驻显示（设置或任务栏 item pin）
- 锚跟随：hover 态弹窗跟随 TM 主窗移动（事件级同拍，亚克力面板与内容零分离）；pin 后可拖到任意位置
- 单窗+切换 / 多窗并排：单窗头部点指标名或滚轮循环切换；多窗模式每窗固定一种指标（最多 6 窗，右键菜单增删）
- 头部条传感器摘要：可选叠加宿主温度与 CPU 频率（频率为 PDH 自测，0.1GHz 精度）
- 选项对话框：全量配置项（出现方式/默认指标/多窗口并排/刷新率/口径/GPU 引擎多选/效果透明度等）
- 任务栏 item：CPU Top1 进程与占用，点击 pin/unpin 弹窗组

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

插件配置：`%APPDATA%\TrafficMonitor\plugins\ProcessTopMonitor\settings.json`（schema 3；v1/v2 老配置自动迁移）。
语言文件：DLL 旁 `ProcessTopMonitor\lang\`。

## 文档

- 技术方案（v2 拍板版）：[docs/技术方案.md](docs/技术方案.md)
- 落地方案（M3~M6）：[docs/plans/](docs/plans/)
- 进度与验收档案：[PROGRESS.md](PROGRESS.md)
- x86 宿主验证报告：[tools/vm_x86_report.md](tools/vm_x86_report.md)

## 已知限制

- M6 hover 形态的交互级实测待 green 位部署验证（代码路径已全部落地）
- Win7（G3）与 ETW B 档真机验证未做（缺 Win7 环境）
- Win8.1/1607 真实 G2 分支未验证（发布前可选）
- GPU 口径为「每进程最忙引擎」，与任务管理器一致；不再提供引擎求和/截断开关
