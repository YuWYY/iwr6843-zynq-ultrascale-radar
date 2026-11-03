# 单有限帧与 DP 第一阶段

代码在 `radar_runtime` 应用中，平台为 `radar_system_m2`。当前版本`M2-SW4.1-v1.4 / 20261030_dp1080_01`已通过1080p60彩条文字及HSI完整帧共存验证。构建应用产生ELF；仅构建Platform不会更新应用。1080p升级已更新PS时钟/XSA/平台/FSBL，采集RTL、PL bit、雷达固件和接线保持成功基线。

## 采集配置

实际候选保存在 `verification/20261030_adc_dp_01/inputs/`：

| 目录 | 雷达源帧数 | HSI头 | 传输几何 | 用途 |
|---|---:|---:|---:|---|
| hsi_single | 1 | 64字节/chirp | 266240 × 1 | 首个完整帧的严格诊断 |
| adc_single | 1 | 无 | 262144 × 1 | 去掉头后的固定 I/Q 输入 |
| adc_batch16 | 16 | 无 | 262144 × 16 | 后续有限批采集，尚需实测 |

三者均为256个复数样本、4 RX、64 chirps/帧；TX0/TX2交替。每次使用哪一份由编译进应用的 profile 决定，`capture profile` 可读取身份。源配置只限定试验，不把审批范围提升为物理时序已通过。

原有正常启动、`dma qualify`、`confirm io`、`connect`、`check10`、`reset3`、`capture confirm ID`、`capture preflight` 和 `capture once` 流程保持。雷达启动必须发生在 DMA 和采集 ARM 之后。有限源已自动停止时，仅对有效有限 profile 的 STOP 事务接受官方 SDK 指定的 stopped-warning 行；仍要求 Done、提示符与静默收尾。

先用 `tools/decode_capture.py` 核验串口导出的完整数据及 CRC，再调用：

```powershell
python /software/tools/decode_adc.py capture.bin --mode hsi --cfg /software/verification/20261030_adc_dp_01/inputs/hsi_single/radar.cfg --transport capture.json --output decoded
```

HSI从偏移0逐条检查64个头，不搜索魔数补救错位。解析器按官方 lane mapping 恢复内存序，再把 Q/I 变为小端有符号 `[I,Q]`。输出形状为 `[frame,64,4,256,2]`，文件为 `.iq_i16le.bin` 和 `.adc.json`。

HSI session ID不是帧计数器。只有实际有限源、ARM先于启动、零skip、完整传输及严格头检查共同满足，才能建立这一帧的边界证据。串口CRC不验证模拟信号或LVDS误码率；ADC-only长度本身不能证明帧对齐。

## 显示命令

| 命令 | 行为 |
|---|---|
| display init | 在空闲状态检查时钟和显示器能力、训练链路并显示第一页 |
| display test | 请求第二页/第一页，在硬件描述符确认后报告切页 |
| display status | 读取扫描状态、错误和时序信息 |
| display diag | 空闲时读取源寄存器、接收端状态和EDID，保留故障线索 |
| display off | 停止显示DMA并关闭主链路 |

当前画面为彩条和英文文字，明确标注 STATIC TEST IMAGE，不显示雷达测量结果。真实画面仍须目视确认，HPD或链路训练成功不能替代这一确认。采集期间保持静态画面；初始化、切页、停机和重连避开采集/控制事务。串口导出期间暂停显示命令，防止混入数据记录。

当前采用标准1920×1080p60：Htotal2200、Vtotal1125、标称像素148.5MHz、RGBA行长/步长7680字节、每页8294400字节。VPLL由Vivado原生求解并由新FSBL冷启动配置，视频/10、STC/55；主互连迁至DPLL/3约400MHz，DPLL/DDR等采集相关时钟未改。应用读取并核验时钟身份，保护28个时钟寄存器，不在线修改PLL或视频分频。

历史自定义720p尝试未出图；随后SVGA成功并保留基线。当前1080p60已获用户目视确认，并完成一次显示/HSI采集/完整导出共存验证。裸机dppsu1.7将TU小数余数重复乘1000，应用在原生MSA配置后按PS寄存器单位覆盖并回读；1080p下整数52、小数800/1000。未改BSP厂商驱动。显示提示进入控制台队列，防止异步HPD消息插入R4导出数据行。

平台更新时发现Vitis保留旧FSBL初始化源码，已用当前SDT/BSP原生生成的psu_init.c/.h精确刷新并原生重建，源码来源及实际冷启动均通过。该处理不手改寄存器值。以后改变XSA时必须同时核验FSBL实际采用的初始化来源。

| 内存 | 范围 | 用途 |
|---|---|---|
| 原SG描述符 | 0x00200000–0x003fffff | Normal NC |
| 原RX池 | 0x00400000–0x00bfffff | Normal WB |
| 原CPU结果 | 0x00c00000–0x013fffff | Normal WB |
| DP两页 | 0x01400000–0x023fffff | 每页8MiB，WB，提交前flush |
| DP控制 | 0x02400000–0x025fffff | 独立2MiB Normal NC，官方驱动描述符 |

两张测试画面首次提交前预绘，此阶段不覆写正在扫描的画面。未来动态雷达界面须另行实现帧所有权与交换完成管理。

## 证据位置

当前结果、启动身份、源码及记录在[1080p60核验目录](../verification/20261030_dp1080_01/RESULT.md)，旧SVGA资料在`verification/20261030_adc_dp_01/`。当前复用未改变的PL bit，没有重新综合/布局布线；Vivado旧实现标为过期。启动锁存TX FIFO位为1，采集前后相同，实时FIFO寄存器和DPDMA错误为0；不把锁存位无变化视为连续无瞬态错误证明。本轮不交付压缩包。
