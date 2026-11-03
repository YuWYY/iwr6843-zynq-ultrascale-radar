# SW3.2 准入资格与 SW4.1 会话规则

## 三种证据不再混用

| 证据 | 来源 | 能证明/允许什么 | 不能证明什么 |
|---|---|---|---|
| 累计历史 | direct_reset_passes / checks_passed / checks_failed | 故障和成功历史追溯 | 不能作为当前准入开关 |
| 一次性资格 | 整个 qualify 批次通过，绑定 DMA epoch/reset serial | 在其余条件均满足时，尝试一次有限真实采集 | 不是数据有效证明；不是无输入 HALT PASS |
| 会话结果 | 真实数据、快照、停机、复位、恢复、交接全部成功 | 保留并导出本次 RAW16 运输数据 | 不是 ADC/chirp/frame/测角正确证明 |

目前板测只确认旧版的 RUN→RESET，正常完整包后 HALT 仍须板测。新代码在主机模型中验证了独立准入和正常收尾/异常路径，不能由此宣布硬件已通过。

## 资格数据结构与生命周期

`RadarSw3` 保留历史计数，新增 kind、reason、qualification_epoch、qualification_reset_count、reset_qualified。`control_qualified` 只保留旧完整无输入 RUN→HALT→RESET 的诊断含义，不再作为 SW4 唯一准入。

发放条件：整批运行/复位阶段按期通过；最后处于 READY；确认 Reset 清零、RS=0、Halted=1、无 DMA/SG 错误；没有 CPU lease、队列/半包；128 个 slot 全 FREE；Free/Pre/Hw/Post=128/0/0/0；环结构与四条采样 canary 符合预期；控制面保持无生产者的 SAFE。

```text
NOT_TESTED
  -> 开始 qualify：撤销旧资格，NEW_TEST
  -> 一轮或整批未完成：无资格
  -> 整批通过：VALID，记录当前 epoch 与 reset serial
  -> capture once 所有条件合格并取得 SW2 owner
       -> 在任何雷达命令之前消费，CAPTURE_CONSUMED
  -> 成功/失败收尾均不自动续签
```

失败、新测试、测试取消、显式 reset、epoch/reset serial 改变、非 READY/不全空闲、真实寄存器错误都会使资格失效。统计计数即使大于零也不能绕过这些检查。对 READY 软件枚举还要核对真实寄存器；不确认静止就不写 canary、payload 或描述符。

本版刻意采用单次许可，避免以第一次测试永久授权未来所有会话。调试代价是每次新会话前重跑 `dma qualify`；连续采集的复用策略留到后续有吞吐和生命周期证据时再设计。

## 命令关系

| 命令 | 行为 |
|---|---|
| dma qualify | 1 次无输入 RUN→RESET；通过后一次性资格 |
| dma qualify10 | 10 次无输入 RUN→RESET；仅第10次通过后资格 |
| dma check reset | qualify 的兼容别名 |
| dma check10 reset | qualify10 的兼容别名 |
| dma check / dma check10 | 保留原无输入 HALT 诊断；HALT失败绝不改写PASS |
| dma reset | 一次显式安全恢复；撤销资格，不发放新资格 |
| dma status/trace/progress | 状态与证据读取；发现失效状态时撤销过期资格 |
| capture preflight | 九项只读预检，不消费、不发送雷达命令 |

## 会话准入的九个门槛

GENERATED_PROFILE；USER_CONFIRM；DMA_EPOCH_PERMISSION；SW3_IDLE；SW2_CHECK10_RESET3；CURRENT_VERSION；SW2_ONLINE_POWER_IO；IDLE_EMPTY_RESULT；GPIO_ABI_OWNER。

预检读取寄存器可能发现并撤销过期的软件许可，但不改变硬件控制状态。拥有中的会话不会发起竞争性的 SW2 GPIO 事务。预检失败不提前消费原本有效的 DMA 许可；真正 claim 成功后，许可在首次 sensorStop 前被消费。

## 单 owner 与正常成功路径

始终只有一个 GPIO toggle/ACK owner、一个 UART driver/事务 owner、一个 SG ring owner。SW4 拥有会话时，其 poll 负责唯一 DMA/UART poll；SW2 不再把正常 ARM/PHY/SNAPSHOT 状态当故障。电源、nERR、SYS/PHY/RX 错误与每阶段时限仍然检查。

正常完整包 HALT 的验收不能因新的准入策略而放宽：全部预期 BD 完成、RXSOF/RXEOF/实际长度正确、没有半包、无仍由硬件持有的 BD、没有在途中断停机，随后 HALT 按期确认。HALT之后才能把完整数据复制至独立 CPU result store。RESET和重新建环也要按期完成，并核对 epoch 增量、128 FREE、空队列、零租约与实际寄存器静止。

`normal_halt_passes` 只计真实数据完成后的正常 HALT 阶段。结果结构分别保留入口类型、payload halt result/us、teardown reset result/us。HALT通过但后续RESET失败时，可保留阶段证据，整个 capture 仍FAIL。

最终 PL RECOVER 和 SW2 交接读回失败也不能 `result.valid=1`。故障恢复 reset 成功只恢复控制能力；不能补造完整包、TLAST或采集成功。

## 时间字段修复

新 run 开始时清当前 halt/reset 耗时与结果，NOT_RUN 对应0。stop/reset 阶段进入时重设 last_poll_us；新的独立运行/恢复间隔重新统计 max_poll_gap，不把人工等待几分钟计入活动轮询。历史事件继续留在 trace，不通过清空 trace 隐藏失败。

max_poll_gap 是当前活动统计区间的观测值，不是全启动以来最坏中断延迟证明；串口输出/调试器停顿仍可能使实际轮询变晚。到期时优先识别物理完成，迟到记录为 LATE，内存静止性与时延验收分别判断；LATE 不被当作按期成功。
