# 真实距离谱与距离—时间图：实板验证完成

2026-10-30，本轮应用 `M2-SW4.1-v1.5 / 20261030_range_dp_01` 已在 F20 上运行。IWR6843ISK 实采 16 帧，DP 保持 1920×1080p60；用户确认“两张图正常，帧数为 16”。最终雷达 SAFE、最后原始帧保留、DP 保持图像、串口已关闭。

## 已实现

- 固定 HSI/IQ 解析后，每个 RX、每个 chirp 去均值并加 Hann 窗，执行 256 点复数 FFT；对 64 chirp × 4 RX 的线性功率取平均，再转换为 dBFS。
- 上图显示当前帧 0～9 m 距离谱。下图按实际 PL 时间戳绘制历史采样列，采集间隔留白；最多保留 128 帧。
- 每批显式启动 1～32 帧，默认 16 帧。每帧通过完整 HSI 检查、保留区 CRC 检查和实际显示翻页确认后，才开始下一帧；异常停止，不自动重试。
- DP 仅绘制空闲的后台页，等待描述符确认和两个 VSYNC 后再复用旧页。采集、结果存储、显示页和显示控制区仍各自独立。

只修改 `radar_runtime` 应用、相关工具和测试。RTL、PL bit、XSA、平台/BSP、FSBL、雷达固件和采集 cfg 均未修改。此前 1080p60 成功基线保留。

## 本次实测

| 项目 | 结果 |
|---|---|
| 完整有限帧 | 16/16；每帧 266240 B，64 个 HSI 记录 |
| 时间跨度 | 21.519906 s |
| 相邻帧间隔 | 1.430267～1.435007 s，平均约 0.697 帧/s |
| 单帧 CRC + FFT 处理 | 37.220～38.286 ms，平均 37.707 ms |
| 屏幕刷新 | 1920×1080p60；与采集帧率分别记录 |
| 图像检查 | 用户实屏确认；另保存实际板端数据的同代码复绘 |
| 显示错误 | DPDMA ISR/EISR 为 0；TX FIFO 观察计数测试前后均为 1，保留启动期事件 |
| ADC 削顶 | 16 帧日志均为 0 |
| 最后一帧独立核验 | 全部 256 点与 NumPy 计算一致，最大差 0.001002 dB 以下；串口频谱导出量化为 0.001 dB |
| 软件检查 | 原生应用构建、ELF 内存分区检查及 9 组主机测试通过 |

两项旧 EDID 辅助函数未使用的编译警告保留；本轮新增模块无编译警告。主机调度测试使用服务替身，实板 16 帧另有独立日志。

## 使用方式

当前屏幕已经保留 16 帧结果，无需重新下载或点击 Run。终端波特率为 115200。

只查看：

```text
scope status
scope data 15
display status
```

`scope data 15` 导出第 16 个历史条目的 256 点频谱；索引从 0 开始。`scope show` 可恢复已经绘制的保留帧画面，不重复追加历史。`scope clear` 只清图和时间原点，不清原始帧。

同一次启动中再采一批，先按需导出最后原始帧；本次最后帧已保存到电脑，可以执行：

```text
capture clear
dma qualify
connect
query
capture preflight
scope start 16
```

逐条等待完成；预检必须为 READY。`scope stop` 结束当前有限帧后停止后续采集；`safe` 是立即取消。不要在采集中导出或切换显示测试图。任何失败先查看原生日志，不连续盲目重试。

冷启动仍需完整初始化和本轮入口门槛。`scripts/run_scope.py` 是本次已执行的单次控制流程，包含按身份核对后的完整启动、初始检查、16 帧采集和证据导出；它不构建应用、不自动重试。应用构建对象是 `radar_runtime`，不是重新编译 Platform `radar_system_m2`。

## 数据含义与下一步

幅度为相对 ADC 满量程的 dBFS，未标定为 dBm 或反射截面积。标称距离栅格约 44.1 mm，不能当作实际测距精度。右侧 0.26 m 是 0.25～9 m 搜索区间内最强谱点，近端泄漏和静态杂波可能主导它，不能直接当作已识别目标。

当前完成的是“真实采集→可信解析→板上 FFT→双图显示”。尚未验证已知距离目标的测距误差、运动轨迹、连续无间断采集或长期运行。下一步优先用已知距离的静态/移动反射物核对谱峰，再处理静态背景、提高实际采样率；保留当前成功基线。

## 证据和恢复

- [实板原始会话及命令结果](board_sessions/range_20261030_162009/result.json)
- [全量串口记录](board_sessions/range_20261030_162009/transcript.txt)
- [实采历史 JSON](board_sessions/range_20261030_162009/range_history.json) / [CSV](board_sessions/range_20261030_162009/range_history.csv)
- [最后一帧 RAW](board_sessions/range_20261030_162009/capture.bin) / [固定 I/Q 重排](board_sessions/range_20261030_162009/decoded.iq_i16le.bin)
- [独立数据审计](board_sessions/range_20261030_162009/range_audit.json)
- [实际数据同代码复绘](board_sessions/range_20261030_162009/renderer_replay/scope_history_replay.png)：不是物理显示器截图
- [算法和来源说明](checks/range_algorithm_notes.md)
- 修改前文件在 `baseline/`；成功应用和源文件在 `success_baseline/`；源差异、哈希在 `checks/`。

未生成任何压缩包。唯一生产源码仍为 `/software/radar_runtime/src`。
