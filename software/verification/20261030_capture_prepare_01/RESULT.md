# 真实首包采集结果 — 2026-10-30

**128 KiB 首包及同启动的一次复现均通过，2/2 PASS。** 已完成真实雷达 → F20 → SG DMA/DDR → 电脑的数据传输、导出 CRC 和逐字节复核。最终雷达停止，控制状态 SAFE、输出关闭，串口已释放。

| 数据 | 大小 | CRC32 |
|---|---:|---|
| [首包 BIN](board_sessions/first_capture_20261030_01/hardware_20261030_130257/first_packet.bin) | 131072 B | `cd9b1297` |
| [复现 BIN](board_sessions/first_capture_20261030_01/repeat_20261030_130536/repeat_packet.bin) | 131072 B | `3ec7a6fe` |

[本轮完整结果与证据](board_sessions/first_capture_20261030_01/CAPTURE_RESULT.md) 记录启动、代码变更、输入身份、计数、正常收尾、主机检查与待验收边界；[独立审计](board_sessions/first_capture_20261030_01/audit_first_packet.json) 验证两包来源和数据一致性。

本次原生 FSBL 启动通过，`radar_runtime` 已构建为 `M2-SW4.1-v1.2 / 20261030_first_capture_01`，使用 profile `7d81ef52b1e64b78`。仅 PS 应用及相关工具/测试改变，当前 FPGA bit/XSA 沿用，无新的综合实现或 ZIP。

资格为 **FIRST_PACKET_TEST**，不是正式完整时序或 ADC 语义签核。用户确认 S1.5=ON、HDR1=1V8/1–2；实际传输、停止与 DMA 收尾通过。输入时序裕量以及 lane/IQ/chirp/frame 解释仍待核实，下一步优先分析已保存的两包数据。

此前准备工作已完成：[官方固件烧录结果](flash/20261030_program_01/PROGRAM_RESULT.md)、[实际 cfg](inputs/iwr6843isk_sdk35_raw16_candidate.cfg)、[格式源码审查](reports/sdk_raw16_review.md)、[启动前导审查](reports/sdk_clock_startup_review.md)。试验 profile 依据位于 [capture_review.first_packet.json](board_sessions/first_capture_20261030_01/capture_review.first_packet.json)；原 `reports/capture_review.pending.json` 是正式资格的准备记录，不能冒充本次实测结果。

准备阶段的“尚未采集/生产未改动”描述已由当前结果取代，其原文保存于 [准备阶段历史记录](board_sessions/first_capture_20261030_01/baseline/evidence_before_capture_close/PREPARATION_RESULT.md)。此前 `production_after.json` 同样只表示准备阶段的检查时点。
