# 项目总计划：冻结 M2 硬件，推进软件与首包板测

## 1. 当前证据边界

用户串口（2026-10-17，旧 SW4-v1.0）已证明 SW0/SW1、GPIO ABI和无输入DMA RUN→RESET控制路径。无输入HALT超时仍保留。当前没有真实UART批次、正式采集profile、RAW16有效包、ADC语义、FFT或DP板测结果。本版改动的本地验证见 validation；不能把主机模型PASS作为新板测。

软件基线 `Radar_Vitis_M2(1).zip`；硬件基线 `Radar_XCZU2CG_M2(5).zip`。本次实际核对了11个XSA/bit文件及XSA内bit身份项，全部符合冻结哈希；这不是板上加载配置的实时哈希。

## 2. 与实际冻结硬件的对应

```text
IWR6843ISK DATA0/DATA1 + BITCLK + FRCLK
 -> IBUFDS / IDELAYE3 / ISERDESE3
 -> 字对齐 / RAW16 / 有限运输分包
 -> 64bit AXIS async FIFO (8192拍 = 64KiB)
 -> AXI DMA S2MM SG
 -> SmartConnect -> PS HP0 -> DDR

PS UART1 EMIO -> PL IO Guard -> 雷达UART/控制
PS -> AXI GPIO双32位邮箱 -> ABI/ARM/PHY/SNAPSHOT/RECOVER
PS UART0 -> 调试台；后续PS显示路径另行验证
```

实际HWH：DMA 0xA0000000，SG=1、S2MM=1、MM2S=0、64bit、32bit地址、23bit长度、DRE=0；GPIO 0xA0010000、dual32；ABI 0x52430101。DMA/SG/AXI-Lite/HP0系统域沿用约100MHz，不用雷达BITCLK作为DMA控制时钟。本版不修改任何上述接口。

RAW16合同：两Lane，每Lane600Mb/s，16位字；FRCLK用于解串字边界，不把它当真正雷达帧边界。一个32位pair为 `{lane1,lane0}`；64位AXIS低32位是更早pair。TLAST按运输包边界生成。首包可沿用128KiB/2个64KiBBD/1包，但它不是已确认的雷达frame。

SOURCE_COMPLETE仅说明最后一拍已入FIFO，不说明写DDR完成。接收资格要结合RX时钟、PHY终态和fresh snapshot；不能把固定sleep、GPIO ACK或CLI Done当作波形合同已验证。

内存不变：BD 0x00200000–0x00400000（2MiB独立NC属性区域，实际128BD只需8KiB）；RX 0x00400000–0x00C00000（8MiB）；CPU结果0x00C00000–0x01400000（8MiB）。最大有限采集受128BD和8MiB两个约束共同限制。堆64KiB，不用于大图像/FFT动态缓冲。

## 3. 接下来按依赖推进

| 里程碑 | 本轮状态 | 下一项验收 |
|---|---|---|
| SW3.2 独立复位资格 | 本地代码/回归完成 | 原生构建；3次冷启动×qualify10；epoch许可与时间统计 |
| SW2 雷达控制 | 代码保留，实际验证待完成 | 供电/电平/PG/nERR，query、check10、reset3；当前固件身份 |
| SW4.1 单次有限采集 | 会话准入/生命周期修订完成 | 正式bin/cfg/review，首个有效RAW16及元数据、完整停机与恢复 |
| ADC数据合同核对 | 未执行 | 已知场景下I/Q、RX、chirp/frame排列、HSI/CQ/填充解析对齐 |
| 重复会话/异常验证 | 主机模型覆盖 | 断流、错误、取消、跨会话实际板测，不静默覆盖旧结果 |
| 连续流/吞吐 | 暂不扩展 | 有限模式稳定后再改补BD、队列、缓存和消费者预算 |
| 算法与DP | 后续 | 先可信Range/Doppler/angle参考，再硬件加速、PS framebuffer/DP |
| 论文创新 | 尚未冻结 | 依托稳定数据链聚焦一个新增算法或硬件功能，另作查新/对照 |

推荐先让 SW3.2 在不连接雷达的SAFE条件下验证通过，再补SW2实际硬件证据；真实profile补全后在同一套会话框架进入首包。不要把FFT、DP、RTOS、DMA中断迁移同时塞进当前故障闭环。

## 4. 需要继续保留的风险清单

R07外部DATA/FRCLK双边沿输入时序和真实电气/端接证据仍须关闭；不能因为已有bit就标为PASS。PHY校准窗口/板上信号完整性不由C单元测试证明。无输入HALT的具体硬件内部原因没有被本次软件修订证明。

以两路600Mb/s全时输入计算，理论原始流量150MB/s；64KiB空FIFO在下游完全停顿时仅约0.437ms，8MiBRX池约55.9ms。这只是参数预算，不是板上可持续吞吐结论。115200 UART导出只作首包调试，后续连续算法要单独规划消费者和导出通道。

显示阶段优先复用既有PS路径；`PL live video` 未启用不等同于无法通过DDR framebuffer输出。实际PS DP初始化、PMUFW/启动配置与显示器链路将在显示阶段单独验证，不因本次DMA控制修订提前变更冻结硬件。
