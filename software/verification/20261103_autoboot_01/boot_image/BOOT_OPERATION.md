# F20 上电自动运行

2026-11-03：v1.12.0 已写入板载 Flash，原生校验、两次全片 SHA256 及一次实际独立冷启动通过。用户确认第三页持续更新。详见[交付记录](../RESULT.md)。

## 日常使用

1. 保持 **F20 底板 SW1 第1位ON、第2位OFF、第3位ON**。这是F20启动开关，不是雷达S1。需要改动时先断电。
2. 保持原雷达接线、拨码和电压设置；DP显示器连接并开启。
3. 给F20上电即可。无需电脑、JTAG或Vitis Run；本轮已在拔掉JTAG的情况下验证。J2串口仅用于可选诊断，自动运行不依赖PC命令。
4. 程序完成原准入检查后，自动进入第三页“平面回波监测”，运行PL_BASELINE / STREAM。应用初始化流程约12.7秒，不含BootROM/FSBL启动时间。

显示器未就绪或检查失败会保留阶段/原因，不无限重试。此部署对应当前已确认接线和雷达固件，不能直接套用不同安装。

## 串口与恢复

J2 PS串口115200、8N1。`autoboot status`、`scope status`、`display status`可读状态；`scope stop`正常停采，`safe`保留紧急取消语义。开机后自动运行，无需发送启动命令。

若需JTAG调试，先断电，再把F20 SW1改为ON/ON/ON并接回JTAG。不要带电改拨码；失败时先保存日志，不反复烧录。

此前成功v1.11.5：`/archive/20261103_autoboot_01/v1.11.5/`；FPGA旧基线：`/archive/20261103_autoboot_01/fpga_before/`。这些配套产物可恢复此前手动SAFE演示，勿混用新旧XSA/bit/FSBL/ELF。

写前Flash两份完整备份：`/archive/20261103_autoboot_01/flash_before/export_resume_20261103T060920_478439Z/flash_read_a.bin` 和 `flash_read_b.bin`。它们是原介质内容，不等于此前通过JTAG运行的v1.11.5。恢复原Flash属于独立操作，应先核对身份。

本次启动镜像：[production_02/BOOT.BIN](production_02/BOOT.BIN)，SHA256 `cc05646c6d6dd860aa2f778fde47b8a06e75a91fec9eecc36448b0b4e4ac86e9`。生成、编程与校验入口和证据位于本目录及上级scripts/、flash_verify/；编程器默认只预检，不能把旧payload用于未知Flash状态。

当前保留中文四页、原算法与KEY3逻辑。未测试KEY3；一次冷启动和短测不代表反复开关机可靠性、长测或测量精度已经全部验收。
