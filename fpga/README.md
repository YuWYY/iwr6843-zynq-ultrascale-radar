# Radar_XCZU2CG_M2 主线入口

> **仓库说明**：本文件是硬件工程自带的版本记录，最新一次构建（v1.12.0 /
> `20261103_autoboot_01`，含 QSPI 自启动）以仓库根目录 `README.md` 为准。
> 为控制仓库体积，`radar_system_m2.xsa`、bit、`.runs/.cache/.gen` 等构建产物
> 以及 `ip/pl_dsp/` 下的 IP 输出产物均不随仓库发布，需在本地重新生成。

唯一工程：[Radar_XCZU2CG_M2.xpr](/fpga/Radar_XCZU2CG_M2.xpr)。使用 Vivado 2025.1，器件 `xczu2cg-sfvc784-1-e`，设计顶层 `radar_m2_top`，原采集仿真顶层 `tb_capture_fifo`。请保留整个工程目录一起使用。

当前硬件版本 **20261102_stream_scene_01**，配套软件 **v1.10.1**。在已验证的 PL_BASELINE 算术链上增加运输帧元数据保护和原子 SYS 时间快照/epoch 接口；FFT/CFAR、原采集 DMA、时钟及物理约束保持。本版已完成 XSim、Validate、综合布局布线和匹配 Vitis 构建，WNS +0.627 ns、WHS +0.011 ns，路由错误为零。v1.10.1 已完成 6213 帧、621 秒实物长测及正常停止，再次启动并恢复 TOP VIEW LIVE。新画面约 9.997 Hz，更新间隔 P95 100.135 ms，SYS 接收到实际显示确认延迟 P95（含映射误差）149.797 ms。完整 CPU 计量、物理标定/分类及 DP 观察边界见[本轮结果](/software/verification/20261102_stream_scene_01/RESULT.md)；不标为无条件全项通过。

根目录 `radar_system_m2.xsa` 当前 SHA-256 为 `d7693692fd7f3d5735af0060dcfd94a0124ca6897ab9cf28f872e98364185730`，内嵌 bit 为 `fc101407c97d812f3b01a47077eff6b80a643b9fd3d20dd2eb30598563e06dc1`。Vitis 输入/平台/导出 XSA、BSP、FSBL、应用和启动缓存已核对一致。当前普通应用内存结束于 `0x1ba3c0`。可恢复 v1.9.3 基线在 `/archive/20261102_stream_scene_01/`；未制作压缩包、未测试 KEY3。实际输入时序和空间标定边界继续保留。

以下为 **20261101_pl_dsp_01 历史记录**，其中版本、哈希与离线性能不代表当前产物。

该历史版本完成主要雷达计算链迁入 PL：去直流、加窗、距离／多普勒 FFT、功率、CFAR、峰值筛选和实验性时序检测。两个 FFT 均为批准的 24 位 Radix-2 Burst。该阶段状态为 **离线工程验证通过／算法收益门槛未达标／板上未验证**。

本版原生官方 FFT HDL 完整帧、逐位对照、64 任务回绕、Validate、综合布局布线和匹配 Vitis 构建均通过。正式 WNS +0.641 ns、WHS +0.011 ns；单任务在公开内存仿真条件下为 55.357823 ms，不代表真实板上采集率。实际输入时序与板级并发仍待验证。候选弱目标算法未达到漏检改善 20% 的门槛，保留负结果与修订计划。

根目录 `radar_system_m2.xsa` 为本版原生导出，SHA-256：`32513746ffcdbc80c7d24ece6bb76d035b8fe762d236d07073051de5a0dbae0a`。Vitis 平台、BSP、FSBL、应用和启动缓存已匹配，未下载或启动。本轮未连接串口/JTAG、未验证 KEY3、未制作压缩包。

新增入口为 `scripts/m2_pl_dsp/`、`rtl/dsp/`、`ip/pl_dsp/` 与 [仿真说明](/fpga/sim/pl_dsp/README.md)。接口、证据与待项集中在 [本轮结果](/software/verification/20261101_pl_dsp_01/RESULT.md) 和 [接口合同](/software/verification/20261101_pl_dsp_01/CONTRACT.md)。完整修改前基线在 `/archive/20261101_pl_dsp_01/`。旧 Streaming 尝试已明确标为被替代记录，不作为当前验收证据。

下述 **20261014_r07_b05** 是历史核验基线，不能证明当前 DSP 源码或当前运行目录通过。它的内部时序为 WNS +1.085 ns、WHS +0.010 ns；实际输入时序及板级电气尚未完成签核。原始记录按当时状态保留。

| 位置 | 用途 |
|---|---|
| `rtl/`、`constraints/`、`sim/` | 当前生产RTL、约束和测试台 |
| `scripts/`、`docs/` | 集成/核验入口、接口协议及设计说明 |
| `Radar_XCZU2CG_M2.srcs/` | 当前BD、IP配置和GUI布局 |
| `.gen/`、`.ip_user_files/`、`.runs/`（均带工程名前缀） | 当前原生生成依赖、IP文件及有效综合/实现结果，继续保留 |
| `outputs/m2_closure_20261014_r07_b05/` | 本次有效构建、报告和身份凭据 |
| `outputs/m2_closure_20261013_c1/sim_06/` | 当前仍有效的17项真实XSim证据，虽日期较早仍须保留 |
| `delivery/20261014_r07_01/` | 当前R07核验结果与证据 |
| `delivery/20261013_c1/before/radar_system.bd` | 结构检查脚本仍使用的基线，特意保留这一份 |

工具维护的 `.cache`、`.hw`、`.sim` 以及 `.Xil` 等目录保持原生位置；它们不是另一套主线。当前交付目录内部分文件名含 b02/b04，是仍被当前凭据引用的测试或恢复证据，不能仅凭旧编号移除。

直接查看：[核验结果](/fpga/delivery/20261014_r07_01/RESULT.md) · [警告复核](/fpga/delivery/20261014_r07_01/verification/native_warning_review_b05.md) · [实际时序/电气待项](/fpga/docs/m2_closure/R07_SOURCE_TIMING.md)。

## 历史资料归档

2026-10-14 已将 **781 个旧文件、65.65 MiB** 移至工程外，保留原相对路径，没有永久删除。包括早期构建/失败尝试、旧交付包与专项调试记录，以及不再被工程引用的M1源、旧独立Wizard、旧增量DCP和旧OOC运行日志。

归档入口：[cleanup_20261014_01](/archive/cleanup_20261014_01/README.md)。归档中的 `mainline/<原工程相对路径>` 是旧文件的新位置；`completed_moves.json` 提供完整映射，`verification.json` 记录逐文件SHA和原生重开核验。历史设计文档内的旧报告路径按该映射追溯；更早一次清理的路径映射也保留在归档中。

2026-10-14 的整理未改动当时保留文件的原字节，当时342项构建输入、DCP、仿真和报告身份通过。该次原生重开前后文件引用清单相同，两个run均Complete且NEEDS_REFRESH=0。这些结论不适用于后续修改后的运行目录。
