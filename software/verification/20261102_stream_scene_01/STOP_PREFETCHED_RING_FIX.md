# 连续预排 DMA 环停止修复

状态：**v1.10.1 停止修复已在 91 帧短测和新版长测 6213 帧后实际正常停止中验证，本次连续预排环 HALT_TIMEOUT 故障关闭。全量遥测审核及再次启动已完成，数值性能目标达到，具体结论与 CPU/DP 限定见 RESULT.md。** 原故障、原源码、旧 ELF 与身份保存在 `candidates/v110_01_stop_failure/`，没有把旧长测改成 PASS。

## 实际故障与原因边界

`board_checks/stream620_01/result.json` 保存了连续采集的首次停止失败：6213 帧已完成验证和处理，残帧计数 0；`scope stop` 后 HALT 超时，原生状态 `CR=00010002 / SR=00011008`，环有 126 个硬件拥有的预排 BD，2 个 FREE BD，`partial_BD=0`、queued=0。故障后进入隔离状态，未自动复位或启动新采集。

旧实现对连续预排环与有限尾链耗尽都执行 RS=0→等待 Halted。前者在本次实物上未完成。不能把原因写成“规范要求必须等下一个 TLAST”：PG021 对 RS 清除描述为 graceful 完成/清空，而不是该普遍承诺；目前能证明的是这条预排环路径发生了实测 HALT 超时。

`checks/stop_prefetched_repro/stream_capture.run.log` 用生产 STREAM 和 AMD SG 驱动、注入“RS 停止不完成、原生软件复位可完成”的主机模型复现了旧流程：64 个完整帧之后 `error=-210/-15`、DMA=HALT_TIMEOUT。该注入只重现观测到的结果，不模拟或证明 IP 内部的根因。

## 修改范围和完成屏障

生产只修改 `capture/radar_stream.c/.h`。没有改 DMA HAL、原始采集 RTL、时钟、配置或 DSP 算法。应用版本由主任务更新为 v1.10.1。

正常停止首先要求：

1. PL 报告 SOURCE_COMPLETE，并且完整快照的 session、字节/拍数、完整包数及错误状态匹配。
2. 已收到 radar `sensorStop` 的成功应答；当前来源状态为 STOPPED。再次读取健康状态和 SOURCE_COMPLETE / SESSION_OPEN / DMA_RESETN，拒绝待 ARM、待命令及 RX fault。
3. `completed_packets == released_packets == frames_verified == metadata_received == stop_target`，字节和 BD 数严格匹配；没有 partial BD、DMA queued packet、活动复制、DMA consumer lease、写入中 RAW 或未配对元数据。
4. DMA 仍属于该 admission epoch，原 SW3 资格包含复位控制；BD Pre/Post 均为空，硬件/空闲数及提交/完成差值一致。

若还有未消费的预排 BD，正常路径记录 drain proof 和 `unused_bds`，调用一次已有的 `radar_dma_reset_authorized()`。这是原生 `XAxiDma_Reset()` 软件复位，不是外部 `aresetn`；AMD 本机驱动注释说明在途事务先完成、其他已提交事务被丢弃。`partial_BD=0` 本身不作为总线静止证明。

若有限尾链已耗尽，仍沿用 HALT→软件复位路径。新路径不先制造 HALT 超时，不在故障后自动转复位，不修改 TAIL/CUR，不伪造 TLAST。

两条路径都必须等现有 HAL 证实 ResetIsDone、RS=0、Halted=1、无 DMA 错误并完成环重建；STREAM 再检查 reset_result=PASS、reset_confirmed、epoch 仅增加 1、128 个 FREE BD 和实际 quiescent。**此后才结束元数据 epoch、执行 PL RECOVER 和释放源所有权。** CPU 的独立 RAW 槽不属于接收 BD 内存，已验证且正在处理的 RAW 租约仍有效。

复位超时仍为 LOCKED；元数据 epoch、描述符所有权和失败证据保持，不二次复位、不回收未确认安全的内存。

## 回归结果

最终证据：`checks/stop_prefetched_fix_02/summary.json`，build_exit=0、run_exit=0、UBSan 启用、源码前后稳定，**1,302,565 个断言**。运行日志：`checks/stop_prefetched_fix_02/run.log`。

- 16/64 有限自然结束保持 HALT_RESET。
- 连续 64 帧在 RS→Halted 永不完成的模型下，用恰好一次软件复位正常停止，记录 126 个未消费 BD，不发起 HALT。
- 复位本身超时保持 LOCKED、epoch/所有权/未消费 BD，不释放元数据、不重试。
- sensorStop 失败、真实描述符部分完成而无 EOF、残留元数据、释放计数不足、SOURCE_COMPLETE 丢失、元数据总数错误均不能授权正常复位。
- 已验证 RAW 处理租约跨 DMA 复位保持有效；有限采集提前停止也正确处理剩余预排 BD。
- 原过载停采、HSI/CRC、序号和 RAW 租约检查仍通过。

首次修复测试 `stop_prefetched_fix_01` 保留失败记录：主机注入的 `partial_prefix` 被真实 harvest 重新计算覆盖，随后改为在实际 BD 状态注入 COMPLETE+SOF、无 EOF；另外测试期间版本头发生主任务授权变更，source_stable=false。最终 fix_02 同时修正测试注入方式并确认源码稳定，未通过放宽生产门槛获得 PASS。

复跑命令（仅主机，不操作板卡）：

```powershell
wsl.exe -d Ubuntu -- python3 /mnt/d/Radar_Vitis_M2/verification/20261102_stream_scene_01/scripts/check_stream_stop.py --output /mnt/d/Radar_Vitis_M2/verification/20261102_stream_scene_01/checks/stop_prefetched_final
```

## 实物短测关闭

`checks/application_native/stop_fix_02/result.json` 确认新版原生 PS 构建 PASS，ELF SHA-256 为 `9f6f371977ccd7316d6e98b2611135c64bd53dc9fc3cf3288f256689ee2f7b46`；硬件维持原已匹配 bit/XSA。启动身份保存在 `startup/launch_identity_v1101.json`。

`board_checks/stream_stop_fixed_01/scope_stop.txt` 保存了实际正常停止：91 帧、36341760 字节、637 个已释放 BD、119 个未消费预排 BD；实际选择 `RUN_TO_RESET`，日志确认 `reset/Halted confirmed`。停止完成时已显示 90 帧，随后最后一帧真实 ACK 到达；`scope_status.txt` 最终确认 verified/processed/presented=91/91/91，ready=owners=pending_display=0，error=0，CRC/HSI/DMA/metadata 错误均为 0，没有丢弃尾帧或把停止状态版本冒充帧 ACK。

短测完整时序导出为 91 条，覆盖约 9 秒观测：采集/处理/新画面率 9.917 Hz，更新间隔 P95=100.122 ms、最大 133.484 ms，含映射不确定度的 SYS 首接受→首次 DP ACK 延迟 P95=145.289 ms。以上是短测数字，不是十分钟结果。

**DP 限定仍保留：** 原严格 `timing_audit.json` 因累计 DP mask=0x8 保持 FAIL。独立 `dp_scope_observed_audit_01.json` 结论为 `NO_NEW_DP_ERROR_OBSERVED_IN_SCOPE`，不是物理零事件证明，也没有覆盖完整 CPU 80 ms 门槛。停止路径关闭不等于整体无条件验收。

## 新版长测停止关闭，性能分析待项

同一新版在短测正常停止后显式重新启动 `board_checks/stream620_fixed_01`，随后持续采集并软件切换页面。该次计划 620 秒的试验现已正常停止；这里只关闭停止故障，不用配置周期或帧数推算实际观测跨度来替代全量时序分析。

实际停止证据：

| 项目 | 已保存证据 |
| --- | --- |
| 来源与边界 | `scope_stop.txt`：source ACK、boundary/HSI/CRC/metadata matched，epoch=2 |
| 完整数据 | frames=6213，bytes=2481223680，恰为 6213×399360；released_bds=43491，恰为 6213×7 |
| 预排环处理 | unused_bds=126，path=RUN_TO_RESET；原生 reset/Halted confirmed，reason=0 |
| 停止时处理状态 | `[STREAM STOPPED]` verified=processed=6213，presented=6212，remaining=residual=error=0 |
| 尾帧实际显示确认 | 随后的 `scope_status.txt`：presented=6213，skipped_display=pending_display=0；尾帧 ACK 没有被停止状态遮蔽 |
| 最终所有权 | running=0、ready=0、owners=0；records=6213、overwritten=0；高水位 high=1 |
| 采集错误 | 最终 CRC/HSI/DMA/metadata 错误计数均为 0；已保存 `live.txt` 未出现 STREAM FAIL/ERROR、HALT_TIMEOUT 或 RESET_TIMEOUT |
| DP 有限观测 | 前后 `display_diag` 文件逐字节相同：HPD=1、ISR/EISR=0/0、TX FIFO sticky=1、observations=1、寄存器 0x0110=0；最终累计 DP mask 仍为 0x8 |

这次独立复核只读取现成文件，没有执行硬件恢复、采集或寄存器操作。`scope_stop.txt` SHA-256 为 `ae45c4318cb788f45a6f2106d6521b5c8662fb60332902088e23e7a09209e2ea`；`scope_status.txt` 为 `adc743e89953ecfcd22d4e9228fb16919c6fecb5b1fcf0337ff58cc3dd48756f`；两份 DP 诊断均为 `e9935ca38bb15bf31e985a48b10bdc016d3601bf3814a683b1175d52ee214878`。

**尚未关闭的全局门槛：** 14:09 审计时完整 RAM 遥测仍在导出，不能提前宣称 6213 条记录全部通过数值、唯一 ACK、间隔分布、延迟上界或 ≥600 秒真实观测跨度审核。`result.json` 的 RUNNING 包含导出阶段，不代表源仍在采集。DP 仍只支持当前观测范围内未见新增错误，不证明零物理事件；完整 CPU≤80 ms/帧仍为 NOT_PROVEN。保留原长测的停止失败与原严格 DP FAIL，由主任务最终全量分析单独给出性能结论，不需要为本停止故障再重复板测。
