# 证据来源与官方接口说明

访问日期：2026-10-17。工程事实优先来自本次上传文件和冻结硬件；公开文档仅用于解释IP/工具语义，不替代用户板卡资料。

1. 最新生产基线：Radar_Vitis_M2(1).zip，SHA-256 `46f049edc8e5f5bc1b7c618583bf372f3e12b2cd871653b18f5f267ac1ee87d0`。实际代码、Platform/BSP和下载副本均由此提取。
2. 冻结硬件：Radar_XCZU2CG_M2(5).zip，SHA-256 `61453da9dfaa34637bc7e97d9d5dec36424456bf0a46f002d91525a25f3cbadb`。CONTROL_ABI、RAW16合同的原文副本在 frozen_m2_reference；HWH关键配置/身份在 validation/HARDWARE_IDENTITY.json。
3. 上一交付的tests/tools作为测试起点，经本版补充。生产源文件均来自最新上传而非旧交付替换；AMD测试驱动副本与本次上传BSP匹配记录在 VENDOR_BSP_MATCH.json。
4. 用户串口证据：2026-10-17 12:12–12:28（用户日志时区+08:00），旧SW4-v1.0的无输入HALT超时、一次显式RESET和一次独立RUN_RESET通过。本包不修改该历史为新软件的板测。
5. AMD PG021 S2MM_DMACR：https://docs.amd.com/r/en-US/pg021_axi_dma/S2MM_DMACR-S2MM-DMA-Control-Register-Offset-30h 。Stop请求与实际Halted不是同一事件；Soft reset作用于整个DMA；完成前不能推断所有权已回收。文档不构成本板无输入HALT必然失败的证明。
6. AMD PG021 S2MM_DMASR：https://docs.amd.com/r/en-US/pg021_axi_dma/S2MM_DMASR-S2MM-DMA-Status-Register-Offset-34h 。区分Halted、Idle及错误状态。
7. AMD PG021 S2MM_STATUS：https://docs.amd.com/r/en-US/pg021_axi_dma/S2MM_STATUS-S2MM-Status 。描述符实际长度和SOF/EOF/完成状态。
8. AMD embeddedsw AXI DMA API：https://xilinx.github.io/embeddedsw.github.io/axidma/doc/html/api/group___a_x_i_d_m_a.html 。实际本地驱动版本由上传BSP决定。
9. AMD UG1400 2025.1 Manage Source Code：https://docs.amd.com/r/2025.1-English/ug1400-vitis-embedded/Manage-Source-Code 。通过UserConfig.cmake管理源列表与编译设置。
