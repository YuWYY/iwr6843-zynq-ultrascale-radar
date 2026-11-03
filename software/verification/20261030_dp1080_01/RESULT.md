# 1080p60 升级记录

状态：`1080P60_VISUAL_AND_HSI_COEXIST_PASS`。1080p60彩条文字已获用户确认，两页切换及保持显示时的一份HSI完整帧采集、导出和解析通过。当前应用为`M2-SW4.1-v1.4 / 20261030_dp1080_01`。旧SVGA成功基线继续保留。

## 实板结果

- [冷启动和显示记录](board_sessions/dp1080_native_20261030_154610/result.json)：新FSBL到达XFsbl_Exit且错误状态为0；应用启动通过内存和DMA检查。实际回读VPLL=0x00015900、FRAC=0x800019D4，标称148500000Hz，2200×1125总时序，60000mHz刷新率，两条HBR链路。两页均收到硬件描述符完成确认。
- [用户目视确认](checks/user_visual_confirmation.json)：画面稳定，文字和彩条正常，显示PAGE 2 1920 X 1080及1080P 60HZ。
- [采集共存记录](board_sessions/coexist_final_20261030_154701/result.json)：266240字节，CRC32=`7002e47a`，SHA-256=`f3bcc7c09949e8d4e8d72fbce6da8df29b6705c2aee7596f4247dde2ae740a98`。严格检查64个HSI头，得到262144字节I/Q，形状`[1,64,4,256,2]`。
- 采集及导出前后DP保持PAGE2，DPDMA错误均为0，实时TX FIFO溢出寄存器为0，接收端两lane状态0x77且对齐。启动阶段锁存值`DP_irq=0x0000C002 / TX_fifo=1`前后相同；锁存位不累计次数，因此不据此宣称整段运行从未发生瞬态事件，详见[诊断比较](checks/display_latch_comparison.json)。
- 最终SW2为SAFE、IO输出全关，PL系统/PHY故障为0，DP继续扫描，串口已关闭。未追加冷启动循环或无关回归。

## 本轮修改

- 标准 1920×1080p60，148.5 MHz，水平总计2200、垂直总计1125；两条HBR链路、RGB 8bit输出。
- 两页RGBA显示内存各8MiB，地址0x01400000、0x01C00000；控制区为0x02400000～0x02600000。原雷达DMA、接收池和结果地址不变。
- Vivado原生求解VPLL：FBDIV=89、FRAC=6612/65536、DIV2=1；视频再/10，STC /55。按配置参考频率计算像素为148500000.174 Hz，应用读回按整Hz核验。
- TOPSW_MAIN从VPLL约525MHz迁至DPLL约400MHz；DP_AUDIO迁至DPLL约25MHz，音频仍关闭。DPLL/DDR、APLL/CPU、RPLL/PL0、IOPLL/UART未改变。
- 应用只核验FSBL已设置的时钟，不在线重置PLL。原TU分数修正、行对齐、描述符确认、缓存管理和串口导出排队继续保留。

## 已完成的证据

- [基线SHA-256](checks/baseline_sha256.json)：保存完整平台、用户软件、原ELF、原bit及FPGA配置源；未创建压缩包。
- [硬件变化审核](checks/hardware_update.json)：18个PS参数的实际前后读回，14个RTL哈希未变；[BD差异](checks/bd.diff)未改变连接和其余IP。
- [原生保存、重开与生成日志](logs/clock_apply.log)：Validate完成，原生生成XSA，保存后重开所有PS参数与审核值一致。基线已有SmartConnect参数传播/USER位宽提示仍存在，没有本轮新增AXI配置变更。
- [内存及EDID验证](checks/memory_edid_checks.json)：布局测试4项、EDID断言56项通过。

本轮XSA含原已验证PL bit，SHA-256仍为`dcb3d84a625f6ae5b112f89f9c85adfa1f989aa378540362b943b411afc1b204`。这是PS初始化变更下对未变PL的明确复用，**本轮未重新综合、布局布线或生成新bit**。Vivado的旧实现被标为Out-of-date，不能把旧实现报告当作本轮新报告。

## 已修复的工具交接问题

Vitis原生update_hw/build更新了SDT/BSP，但保留旧FSBL目录中的psu_init.c。本轮额外来源核验发现并阻止了这份旧初始化FSBL下载。首次失败记录保留在checks/platform_update_20261030_153940_714833.json。

按本机厂商create_app.py的来源机制，将新BSP/hw_artifacts中与SDT/XSA逐字节一致的psu_init.c/.h复制至FSBL，再原生构建；未手改任何寄存器初始化值。C文件身份完全一致，H文件仅有厂商CMake添加的包含前缀。修复后的[平台记录](checks/platform_update_20261030_154518_418155.json)及[应用ELF布局核验](checks/native_elf_layout.json)通过。

当前身份：XSA=`d33553b3a170916392745a05877838e46699a1d2125f8983719681009d464ad6`；FSBL=`4172d1c71cae86985f06825ca06b334b5c6c579e57f2a8b47681d90289e6d604`；应用ELF=`721e46c233e09ac4eb960c7a7857a9a41694a889c8d8f6f41c4834d37f2a31bd`。完整启动身份见[startup/launch_identity.json](startup/launch_identity.json)。

原生构建保留厂商头文件宏重定义提示；应用仅有两个保留的旧模式EDID辅助函数未使用提示，无编译错误。未为消除无行为影响的提示追加板卡下载。

## 验收与边界

本轮验收已完成，1080p60作为新的演示显示基线。当前仍为静态测试图；下一阶段可接入已解析I/Q的距离谱，再做距离随时间变化的图。物理输入时序裕量、模拟信号质量和长期运行不由本次单帧共存测试证明。

恢复资料：本目录baseline保存修改前平台/源码/ELF/bit及FPGA配置；success_baseline保存本次通过的用户源码及关键启动产物。当前Vivado旧实现标为过期，日后若重新综合/导出，应重新核对新产物身份，不能直接沿用本次旧bit的实测结论。没有创建压缩包。

时钟方案参考AMD [PG201分数时钟说明](https://docs.amd.com/r/3.4-English/pg201-zynq-ultrascale-plus-processing-system/Fractional-Clocking)，具体参数与产物以本机2025.1原生回读为准。
