# v1.12.0 板载 Flash 固化与自动启动交付

**已完成：PL 与 PS 启动镜像写入板载 Flash，完整原内容已备份；原生写入校验、独立全片双读 SHA-256、实际断电后的 QSPI 自启动均通过。用户确认自动进入第三页，画面与采集编号持续更新。当前保持 LIVE。**

## 用户选择与范围

用户选择将当前成功 PL/PS 固化到已确认装有 Flash 的 F20／CM02-2CG，
上电自动持续采集并进入第三页“平面回波监测”。最终介质为 **QSPI**。
不制作 ZIP，不改页面风格、采集波形、算法或 KEY3 逻辑，不验证 KEY3。

原 v1.11.5 ELF 初始化后停在手动 SAFE 控制台；原电脑脚本负责资格检查、
DP 初始化和 `scope live`。因此本轮生成 v1.12.0 自动启动版，不能将它
描述为“不改原 ELF 直接固化”。

## 已保存的成功基线

- [v1.11.5 基线说明](/software/verification/20261103_autoboot_01/baseline.json)：
  PS 源码、平台、ELF 及相关成功证据共 2175 个文件，复制前后哈希核对。
  [基线清单](/archive/20261103_autoboot_01/v1.11.5/BASELINE_MANIFEST.json)。
- FPGA 工程、RTL、IP、约束、脚本和既有构建产物共 274 个文件保存在
  `/archive/20261103_autoboot_01/fpga_before/`。
  [FPGA 基线清单](/archive/20261103_autoboot_01/fpga_before/BASELINE_MANIFEST.json)。
- 自动启动集成的最终 113 个源文件已冻结，平台更新后仅按匹配产物生成
  硬件身份头；原生平台脚本检查其余源保持不变。
  [冻结清单](/software/verification/20261103_autoboot_01/checks/ps_integration/source_manifest.json)。

原 Flash 两份全片备份保存的是写入前介质内容，**不能自动视为 v1.11.5**；
此前成功演示由 JTAG 加载，两类恢复材料分别保留。

## 原生 QSPI 硬件与受保护配置

原基线没有启用 QSPI/SD，旧 FSBL 不具备该板介质启动所需的驱动配置。
本轮经 Vivado 2025.1 原生配置启用 QSPI：MIO0～5、Single、x4、Bank0
保持 1.8 V，不启用反馈时钟。QSPI 参考时钟目标 **200 MHz**，工具传播值
**199.998001 MHz**；该值是控制器参考时钟，不是实测 Flash 传输频率。

| 受保护部分 | 本轮处理 |
|---|---|
| DDR、PL0、DP/视频、UART 等既有非 QSPI 时钟 | 保持；未为 QSPI 重调既有工作时钟 |
| 雷达接收 RTL、LVDS 时钟/复位、现有管脚约束 | 保持业务逻辑和已定连接 |
| 采集 DMA 的 SG/S2MM、异步 FIFO、GPIO、PL 计算链 | 保持既有参数与拓扑 |
| 已分配地址 | DMA `0xA0000000`、GPIO `0xA0010000`、DSP `0xA0020000`，每项 64 KiB；既有 DDR 窗口保持 |
| PS 的 QSPI 相关配置 | 仅新增 QSPI MIO、使能、数据模式及对应原生保护描述；新增原生未分配 QSPI 从窗口已记录 |
| PS 普通与保留内存 | 普通区域仍低于 2 MiB；DMA、RAW、DP、16 MiB DSP 保留区保持 |

证据：[原生参数差异](/software/verification/20261103_autoboot_01/checks/qspi_config_04/ps_changes.tcl)、
[受保护拓扑](/software/verification/20261103_autoboot_01/checks/qspi_config_04/protected_topology.tcl)、
[配置结果](/software/verification/20261103_autoboot_01/checks/qspi_config_04/status.txt)。

原生配置、BD Validate/生成、综合、布局布线和导出已通过。本轮 routed
WNS **0.627 ns**、WHS **0.011 ns**，报告中的时序约束满足；这些是本轮
实现证据，不额外证明外部雷达信号、类别或定位精度。
[流水线结果](/software/verification/20261103_autoboot_01/checks/qspi_native_stack.json)、
[布线结果](/software/verification/20261103_autoboot_01/checks/native_build/route_qspi_01/result.json)、
[时序报告](/software/verification/20261103_autoboot_01/checks/native_build/route_qspi_01/route_timing.rpt)。

Vitis 2025.1 已原生更新匹配 QSPI 平台、FSBL、BSP 和应用，并核对原生
PS 初始化来源、XSA 内嵌 bit、应用硬件身份和内存布局。
[匹配原生结果](/software/verification/20261103_autoboot_01/checks/platform_native/matched_qspi_01/result.json)。

## PS 自动启动与已完成检查

新状态机在原 SW0/SW1、冷 DMA 和内存检查后，依次选择 PL_BASELINE、
解耦显示、连续 STREAM、固定配置 `ba6ddefbcaf3defb`，使用用户此前确认的
安装合同，执行原生 DMA qualify、connect、check10、reset3、配置确认及
preflight。每一步回读实际状态，不把命令“已处理”当作成功。

HPD 稳定后仅尝试一次 DP 初始化，获得真实描述符确认并稳定后才启动
RF；首个完整帧通过校验、PL 处理及显示确认后才记录自动启动完成。
保留历史启动 DP 告警，新增观察或严重错误按定义拒绝/停止；保留串口
诊断、普通停止及原生紧急 SAFE 语义。不自动重试，也不伪造资格计数。
启动后的显式手动控制会接回监视权，避免旧启动状态干预后续手动会话。

自动状态机的定向主机和 UBSan 检查已通过，包括超时、拒绝、历史告警、
真实 ACK 等待、取消及手动接管。它们使用硬件服务模型，不能作为新版本
已在板上自动运行的证明。
[实现说明](/software/verification/20261103_autoboot_01/AUTOBOOT_IMPLEMENTATION.md)、
[主机日志](/software/verification/20261103_autoboot_01/checks/autoboot_host/result.log)。

## 启动镜像身份

[production_02/BOOT.BIN](/software/verification/20261103_autoboot_01/boot_image/production_02/BOOT.BIN)
大小 **6,280,976 字节**。顺序为 FSBL → PL bitstream → A53-0 AArch64
EL3 secure 应用。bit 在应用之前加载，避免 FSBL 临时 DDR 缓冲覆盖应用。

| 产物 | SHA-256 |
|---|---|
| XSA | `b55a70c587112845460c9ef8bf0e646c21df0b67c7a313f8c012bdbb1bf9f01a` |
| bit | `b2493f5edeb8b117c0b285622f5172e9abc30305eac9f09e5a00cf4a8d2622e5` |
| FSBL | `ddbefa4713e7e6a1cc94283f230134e2d6b501fef60c9f49f6b995c7b462fd6e` |
| 应用 ELF | `29bd3b0330f0bc155b59cd0a0f491fecda31d349c1728ad5fd90f3e7a6887419` |
| BOOT.BIN | `cc05646c6d6dd860aa2f778fde47b8a06e75a91fec9eecc36448b0b4e4ac86e9` |

Bootgen 2025.1 build/read、三分区头和属性、FSBL/应用 ELF 的实际 LOAD
字节一致性均通过。应用 54,525,952 字节的独立 NOLOAD 保留区未膨胀进
镜像。`production_01` 曾因检查器未接受 GCC `.constprop.0` 驱动符号而
误拒绝，失败及原脚本保留；修复受限符号匹配后才生成 production_02。
[镜像核验结果](/software/verification/20261103_autoboot_01/boot_image/production_02/result.json)。

## 实物核验结果

- 原 Flash 两份完整 32 MiB 备份：SHA256 均为 `d13bd2c1ab894a9cac2f07d7a6c70c678d7ddb252735797db43c856cb3764458`，CRC32 均为 `b5691917`。实际 JEDEC `9d7019`，原生识别 `is25wp256`。见 [原始备份核验](/archive/20261103_autoboot_01/flash_before/export_resume_20261103T060920_478439Z/backup_verification.json)。
- 本轮只擦写 `[0,0x600000)`，共 6,291,456 字节，含 BOOT 后 10,480 字节的原扇区尾部；未整片擦除。[payload](boot_image/flash_payload_01/result.json)与[原生编程结果](flash_programming/program_01/result.json)已保存。
- 写后重新读取整片两遍、逐字节比较，再分别计算 SHA256。两份实际结果与主机重建的完整期望值均为 `2ca1eb6f0f2eb9294c6576f386d34e6445b07a097a8ade32f266599daf125c05`，包含未写区域。见[写后全片证据](/archive/20261103_autoboot_01/flash_after_sha/attempt_20261103T062721_523036Z/result.json)。只导出 320 字节新结果，无冗余写后 64 MiB 原始文件；[方法及检查](flash_verify/REVIEW.md)。
- 用户实际断电，F20 SW1 改为 ON/OFF/ON，拔掉 JTAG USB 后上电，没有点击 Vitis Run。电脑 hw_server 已关闭。串口被动收到 FSBL、新版应用、实际 PL ABI 和 `AUTOBOOT PASS`，没有 PC 端启动/RF命令或 JTAG 下载。
- 应用自动启动流程耗时 `12.675419` 秒，不包括 BootROM/FSBL 时间。短测末条采集/处理/首次显示计数为 **979/978/977**，队列高水位 1，汇总错误0；单次只读状态 CRC/HSI/DMA/元数据错误均0。运行中计数相差1～2帧属于尚在流水线中的帧，未据此宣称尾帧排空。
- DP 为 1920×1080p60、TOP VIEW。DPDMA ISR/EISR 错误为0；保留启动历史标志8，FIFO观察基线/当前均1，本范围未新增观察。不是无条件全项DP零错误或长测PASS。
- 用户确认“已自动进入第三页，画面和采集编号持续更新”。仅在自动启动成功后发送了 `autoboot status`、`scope status`、`display status` 三个只读查询。随后释放 COMx 和调试工具，板上继续 LIVE。

完整[独立启动记录](board_sessions/qspi_cold_01/result.json)和[串口日志](board_sessions/qspi_cold_01/boot_serial.txt)已保存。本轮不重复长测，不测试 KEY3，不验证类别/精度，也没有额外停止/重启当前成功演示。

## 保留的异常

原 Flash 导出约70秒/2 MiB，首轮触及2400秒时限，尚余4 MiB；首次续导遇到端口关闭。仅重启本任务的PC端 hw_server，核对原A53仍暂停、最终WFE指令及mailbox完全一致后，续导同一DDR的最后两块；没有复位或重新读取Flash。首失败均保留，最终双文件闭环通过，见[分块来源](/archive/20261103_autoboot_01/flash_before/export_resume_20261103T060920_478439Z/chunk_provenance.json)。

原生工具的Xicom50-353提示对比了连接类型`qspi-x4-single`与芯片名`is25wp256`；实际JEDEC、容量和扇区与板卡一致，原生及独立全片校验均通过。FSBL仍打印PMU-FW未运行提示；本应用直接驱动所需外设，本次独立启动和真实采集显示已完成，不据此推广到Linux/电源管理功能。

恢复与日常操作见[BOOT_OPERATION.md](boot_image/BOOT_OPERATION.md)。原成功v1.11.5及旧Flash内容分别保留，二者不是同一恢复对象。不制作压缩包。
