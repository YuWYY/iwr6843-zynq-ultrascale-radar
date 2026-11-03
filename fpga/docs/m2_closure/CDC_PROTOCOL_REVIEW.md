# M2 持稳数据 CDC 协议审查

本文件记录六组自定义跨域数据通道的协议、原生诊断和共享复位修复。最终构建 **`20261013_c1_build05` 的 `internal_status=PASS`、`CDC_SIGNOFF=PASS`**：实际 Routed CDC 为 CDC-3 Info 35、CDC-9 Info 8、CDC-15 Warning 649，Critical 为 0，原先 12 条 CDC-11 已由结构修复消除，没有写接受 waiver。完整 sim06 为 17/17 PASS，T6 第 10 次门禁回归为 88/88 PASS。R07 仍 PENDING，整体保持 `NOT_DOWNLOAD_READY`；旧 build02/build03/build04 仅为诊断历史，不替代 build05。

最终直接依据为 `outputs/m2_closure_20261013_c1_build05/reports_route/cdc.rpt`，2026-10-13 19:44:20、Routed，SHA-256 `ddfd648a3ca3c585ea22e78dc099531dc9d32a954d3f0dbe7400f957875955e1`；发布记录为同 build 目录下 `release_status.json`，SHA-256 `fb0df8a4f0b641ccc52397dfcd5825a34f6108b3fbdb967f9ef30effdcd0a30d`。以下旧报告用于解释发现和修复过程。

诊断证据为 `outputs/m2_closure_20261013_c1_build02/reports_synth/cdc.rpt`，Vivado 2025.1，报告时间 2026-10-13 18:27:23，设计状态 Synthesized；SHA-256 为 `9a038c23f6779467302ccfc5eb64b05c4c89a9a92a757201c61d1f481d8f7718`。该报告有 CDC-3 Info 36、CDC-9 Info 14、CDC-10 Critical 1、CDC-15 Warning 649。build02 是结构诊断，发生在后述 PHY 复位所有者修复之前，不代表当前源或最终 route。

后继 `outputs/m2_closure_20261013_c1_build03/reports_route/cdc.rpt` 为 2026-10-13 18:58:36 的 Routed 报告：CDC-3 Info 35、CDC-9 Info 4、CDC-11 Critical 12、CDC-15 Warning 649，没有 CDC-10。它说明旧复位入口合并问题已消除，同时暴露同源同域多同步器扇出；不能用较早综合中的 INFO 覆盖布线 Critical。12 条均保留为阻断历史，未被接受或降级。

源码依据为 [radar_mailbox.v](../../rtl/common/radar_mailbox.v)、[radar_snapshot.v](../../rtl/common/radar_snapshot.v)、[radar_reset_release.v](../../rtl/common/radar_reset_release.v)、[radar_capture_bd.v](../../rtl/top/radar_capture_bd.v)、[radar_lvds_phy.v](../../rtl/phy/radar_lvds_phy.v)、[radar_phy_delay.v](../../rtl/phy/radar_phy_delay.v) 和 [radar_cdc.xdc](../../constraints/radar_cdc.xdc)。协议解释与 [CONTROL_ABI](../m2_real/CONTROL_ABI.md) 一致。最终源码身份应使用构建 manifest，不能把本文件的行文日期当成输入哈希。

## 649 个目的位的实际集合

以下实例前缀均为 `u_system/radar_system_i/radar_capture_bd_0/inst/`。表中数字来自逐条 CDC-15 目的端 D 的解析，不是 RTL 总线宽度推算；build05 routed `constraint_metrics.json` 再次确认相同的 649 个目的位，全部有实际 8 ns max-delay 路径。

| 实例 / 方向 | 数据源 → 目的寄存器 | build05 routed 目的位 | 两端共同取消输入 | 本域消费含义 |
|---|---|---|---|---|
| `u_cmd_mb` / SYS→RX | `src_hold_reg` → `dst_data_reg` | `[0:132]`、`[136:143]`，141 位 | `command_resetn` | ARM/STOP opcode、四个 32 位配置及 5 位格式 |
| `u_snapshot` / RX→SYS | `rx_hold_reg` → `data_reg` | `[0:383]`，384 位 | `common_resetn` | 12 个状态/计数快照字 |
| `u_tap_snapshot` / RX→SYS | `rx_hold_reg` → `data_reg` | `[0:63]`，64 位 | `common_resetn` | `{实际测量代次[31:0],5'b0,三 lane 实际 tap[26:0]}` |
| `u_phy/u_delay_mb` / SYS→RX | `src_hold_reg` → `dst_data_reg` | `[0:26]`，27 位 | `rx_epoch_resetn` | 三 lane 绝对目标，各 9 位 |
| `u_phy/u_delay_report_mb` / RX→SYS | `src_hold_reg` → `dst_data_reg` | `[27:58]`，32 位 | `rx_epoch_resetn` | baseline `[53:27]`、result `[57:54]`、kind `[58]` |
| `u_phy/u_cal_report_mb` / REF→SYS | `src_hold_reg` → `dst_data_reg` | `[0]`，1 位 | `cal_epoch_resetn` | 校准成功/失败；自动启动失败也报告 0 |

总数为 `141+384+64+27+32+1=649`。这些全部是自定义 RTL 的协议通道，不能归入官方 FIFO 的通用豁免。

CMD 的 RTL 宽度为 144，综合保留源 144 位、目的 141 位；布线网表进一步将无消费者源 `[135:133]` 裁去，源和目的实际均为 141 位。`radar_receive` 仅使用格式 `[132:128]`。DELAY report 的 RTL 宽度为 59，综合源 59 位、目的 32 位；布线源和目的均为 `[58:27]` 的 32 位，低 `[26:0]` report readback 没有 SYS 目的消费，软件实际 tap 来自独立 tap snapshot。最终原生导出按明确允许集合核对这些优化，未凭总数降低要求，也未为已裁剪位伪造路径。

`u_phy/u_recal_mb` 是额外的纯事件通道，数据恒为 1 且目的数据未使用，因此不属于上述六组 649 个目的位。它的 req/ack 三级链和共同取消仍须检查，不能因为没有 payload 路径就漏掉握手结构。

## Mailbox 的源持稳与交付窗口

`radar_mailbox` 的直接源码依据如下：

1. 只有 `src_send && !src_busy` 才同拍写 `src_hold` 并翻转 `req_toggle`；`src_busy = !src_resetn || (req_toggle != ack_ff[2])`。请求发出后，在 ACK 返回前不会覆盖源数据。
2. 请求依次经过 `req_ff[0:2]`，且三个寄存器带 `ASYNC_REG=TRUE`、`SHREG_EXTRACT=NO`。目的逻辑用旧拍的 `req_ff[2]` 与 `ack_toggle` 比较，发现新请求才一次写入 `dst_data`、产生单拍 `dst_valid` 并更新 ACK。
3. ACK 经过 `ack_ff[0:2]` 返回源。ACK 只代表目的已捕获数据，不代表 RX 延时操作或 REF 校准已经完成；执行结果使用另一条 report mailbox。
4. 三同步级后再捕获使理想最早数据采样距源发射约为 `3*Tdst`。这给持稳总线留下窗口，但不是亚稳态概率或布局时序证明，仍必须检查每个实际目的位的物理路径。

在六组中，CMD/DELAY target 的目的为 RX75，名义最早窗口约 40 ns；DELAY report/calibration report 的目的为 SYS 约 100 MHz，名义约 30 ns。数据源保持到 ACK 返回，覆盖该采样窗口。停止目的时钟时请求保持未交付；停止源时钟时已锁存数据不会改变。共同取消是上述持稳规则的另一合法结束方式，取消后不得用旧 ACK 或旧结果完成下一 epoch。

| 通道 | 源持稳与消费的具体核对 |
|---|---|
| CMD | control 在 busy/send 互斥下发送；`src_hold` 冻结已受理 ARM/STOP 内容。RX 只在 `cmd_valid` 消费。`command_resetn` 同时清 mailbox 两端与 receive 控制资格，防止停钟期间取消的 ARM 在恢复后迟到。 |
| DELAY target | SYS 在受理时保存 `target_hold`；mailbox 交付后由 RX 执行器按本地时钟处理。`cfg_busy` 不因 delivery ACK 结束，完成须等独立结果。 |
| DELAY report | RX 先注册 `{kind,result,baseline,readback}`，`report_pending` 等 mailbox 空闲后发 send；mailbox 再持稳到 SYS ACK。SYS 用同一次捕获的 kind/result/baseline 判断基准或事务结果，未从异步多位总线逐位拼接。 |
| Calibration report | 成功/失败由 REF 注册后发送。显式 RECAL 用报告结束软件事务；自动启动失败只锁存独立 PHY fault=9，不制造软件事务 generation。 |

## Snapshot 的有效性与代次

`radar_snapshot` 的请求也经过三级 `req_ff`。RX 仅在 `req_ff[2] != ack_toggle && sample_valid` 时同拍冻结 `rx_hold` 并翻转 ACK；SYS 的三级 `ack_ff` 返回后才整体写 `data`、置 `valid` 并把复制 `generation` 加一。`busy` 用 `ack_seen` 判断，直到 SYS 实际完成复制才撤销，因此下一请求不能覆盖尚未复制的数据。

状态快照的 `sample_valid=1`，采样的是一个 RX 边沿上的 384 位状态组合；先整体进入 RX `rx_hold`，再跨域传输。tap 快照的 `sample_valid=tap_valid_rx`，等于 PHY 的 RX ready 源寄存资格；校准失效或 DELAY 执行中不能把旧 tap ACK 为一次新样本。

tap 的 64 位内容把实际值和 `tap_generation_rx` 一起采样。实际测量代次只在合法稳定观察流程的 RESTORE 完成时增加，跨本地取消/RECAL 保留；快照的复制 generation 则每次成功回传都增加。重复复制同一次测量可以增加 page13，不能增加 page0C。baseline 的 page12 仅在 SYS 成功收到真实 kind0 基准报告时增加；无 RX 的 RECAL 不能制造新 baseline 或 tap 代次。

`valid` 表示已有一次历史有效结果；发出下一请求不会立即清掉旧 data/valid。软件必须结合 busy、page09/page13 的复制代次以及当前 PHY 资格判断新结果，不能只读 `valid=1`。接受后 RX 停钟或 sample_valid 变低时，保持 busy/旧数据，不产生新 ACK 或新 generation；恢复有效采样或共同 RECOVER 才结束该等待。SNAPSHOT 与 DELAY/RECAL 的互斥包含已注册 send/request 拍。

两组 snapshot 各自原子，**不保证 384 位状态与 64 位 tap 在同一个 RX 边沿采样**。总 busy 等两组返回，不能据此假定跨组同时性。

## 共同取消与共享域复位

当前生产实例使用每个 epoch、每个目的域唯一的 release，同步完成后再扇出。`radar_mailbox` 增加 `EXTERNAL_SRC_RESET`、`EXTERNAL_DST_RESET` 和 `src_resetn_in`、`dst_resetn_in`；`radar_snapshot` 增加对应的 `EXTERNAL_SYS_RESET`、`EXTERNAL_RX_RESET` 和本域输入。生产实例两端 EXTERNAL 均为 1，不再在消费者内部建立独立 XPM。参数默认 0 时仍使用内部释放，供既有独立测试；默认路径不能被误认为当前生产连接。

每个原始域资格使用 `radar_reset_release` 的唯一三级 XPM，原生层级为 `g_async/u_reset_sync`。新 `PREQUALIFIED=1` 只允许输入本域已同步资格，直接作为 `permit_n`，不产生另一个 XPM；计数及 released 仍保持本域 HOLD 和异步安全断言。

| epoch | SYS 共享 owner / 消费者 | RX 或 REF 共享 owner / 消费者 |
|---|---|---|
| command | `u_command_sys_release` → CMD mailbox 源端 | `u_command_release` → CMD mailbox 目的端、receive.common_resetn |
| common | `u_common_sys_release` → 两组 snapshot SYS 端 | `u_rx_control_release` → heartbeat、两组 snapshot RX 端、FIFO 本域 HOLD 输入 |
| RX epoch | `u_phy/u_rx_epoch_sys_release` → DELAY 请求源端、结果目的端 | `u_phy/u_rx_release` → RX 执行器、DELAY 请求目的端、结果源端 |
| REF epoch | `u_phy/u_cal_epoch_sys_release` → RECAL 请求源端、校准结果目的端 | `u_phy/u_cal_epoch_ref_release` → cal_enable_ref、RECAL 请求目的端、校准结果源端 |

FIFO 的 `u_fifo_release` 改为 PREQUALIFIED HOLD64：先等共同 RX release，再等 64 个本域边沿，比旧树多一拍资格等待，ARM 仍须等官方 FIFO 的真实 TREADY。应用 `u_phy/u_deser_release` 使用 PREQUALIFIED HOLD64，输入为同域 `rx_resetn && rx_ready`，不将已同步资格再次跨域处理。

采集 `command_resetn`、流/快照 `common_resetn` 仍由 SYS 寄存所有者产生；ABORT 取消 mailbox/receive 控制，但不清 FIFO 或撤销已有 AXIS 有效拍。DMA reset/RECOVER 才清流 epoch。PHY 失败/超时先持久隔离，再结束 busy，显式 RECAL 建立下一 epoch；8 SYS 周期隔离仍保留，但取消是否被目的域记住不再依赖这 8 个周期内恰好存在 REF 边沿。

最短 command 取消或被 SYS 采到的 DMA reset 可以仅持续一个 SYS 周期，名义约 10 ns，短于一个 RX 周期；RECOVER 保持 128 SYS 周期。异步断言必须满足器件原生最小脉宽，并由 XPM 记忆，不能把“没有 RX 边沿”当作忽略取消的理由。REF epoch 在新 RECAL 时至少隔离 8 SYS 周期，目的 REF 停止时共享资格仍保持低，直到恢复后的真实同步/HOLD 边沿。build05 的脉宽时序 WPWS=0、TPWS=0，原生复位物理检查 PASS；这不是任意外部毛刺宽度均有效的承诺。

build05 native 检查已确认 13 个预期 XPM 父实例、8 组共享复位所有权和 2 个 PREQUALIFIED 使用点。每组原始源到目的域的唯一同步入口及共享消费者驱动均通过硬断言，未在消费者内部重建 XPM。共享释放不表示物理叶端零延迟；普通本域路径与 released 下游恢复/移除保持计时。

共享消费者计数是**综合 1851、routed 1793**，各包括一个 supervisor 的 `cal_enable_ref` 资格边界，其余为实际 CLR/PRE 叶端。逐对象比较为删除 58、增加 0：command SYS 148→145，仅 CMD `src_hold[133:135]` 的 3 位；delay RX 323→269，仅 report mailbox `src_hold[0:26]` 和上游 `report_data[0:26]` 各 27 位；cal SYS 13→12，仅纯事件 RECAL `src_hold[0]`。其余五组 524/465/297/70/11 不变。这些是无消费者数据寄存器优化，不是共享释放关系丢失；649 个活动目的位始终完整。最终逐对象路径及报告哈希由本轮 Warning 审阅证据保存。

## 12 条 CDC-11 与停 REF 漏取消反例

build03 的精确分组如下。表内消费者为旧结构被 CDC-11 标记的同步器入口，其完整前缀与前文一致。

| 源 / 目的域 | 旧入口（逐对象） | 重汇合判断与改动 |
|---|---|---|
| `command_resetn_reg` / RX，2 条 | `u_cmd_mb/u_dst_release`、`u_command_release` | 请求要经过三级同步后才产生 cmd_valid，因此仅一拍释放差有启动余量；但协议不应依赖未明确界定的同步器差值，现共用 command RX release。 |
| `common_resetn_reg` / RX，4 条 | `u_fifo_release`、`u_rx_control_release`、`u_snapshot/u_rx_release`、`u_tap_snapshot/u_rx_release` | heartbeat/快照可早于 FIFO 就绪，ARM 另受 TREADY 保护；两组快照独立原子。现先共享 RX release，再给 FIFO 单独本域 HOLD。 |
| `u_phy/rx_epoch_resetn_reg` / RX，3 条 | `u_phy/u_delay_mb/u_dst_release`、`u_phy/u_delay_report_mb/u_src_release`、`u_phy/u_rx_release` | DELAY 需 RX ready/基准，结果发送检查 busy；一拍差不等于立即丢包，但已统一到 RX owner，避免复位释放关系依赖多链延迟。 |
| `u_phy/cal_epoch_resetn_reg` / REF，3 条 | `u_phy/u_cal_enable_ref`、`u_phy/u_cal_report_mb/u_src_release`、`u_phy/u_recal_mb/u_dst_release` | 存在真实漏取消反例：普通 D 电平链不能记住 REF 停止期间已经低后又高的 epoch，而异步 mailbox 已清零。现用同一个 REF 异步记忆资格供三者。 |

反例时序是：旧显式 RECAL 已到 SETTLE 最后计数；停止 REF，SYS 取消旧事务，再在 REF 仍停止时受理并派送下一 RECAL。旧 `u_cal_enable_ref` 从未采到低电平，supervisor 的旧 state/explicit_request/report_pending 可残留。恢复 REF 后，旧成功报告可能先于新请求完成完整校准就被 SYS 视为新事务 SUCCESS。固定延长隔离脉冲不能覆盖任意长停钟，因此必须修结构。

旧源诊断日志为 `outputs/epoch_old_02/sim_result/run_tb_phy_closure.log` 第 143 行，138880 ns：`REGRESSION_ASSERT stale REF success completed new RECAL requests=1 old_sequence=41 current_sequence=41 refready=0`，日志 SHA-256 `65a58513414f4170c804fdb6b6294f5c7b2608d6d277205fdef0b417f69cfd68`。该反例在限定数字竞态窗口受控保持 `idelay_rdy=1`，在新请求实际到达且原语 RST 断言后解除，目的是隔离 epoch 协议；它不是自然 UNISIM RDY 行为或板上故障的证明。

修复后的 REF 共享 release 在停钟时异步变低，恢复后的首批 REF 边沿让 supervisor 观察到 cal_enable=0，清理旧请求/结果并进入 QUIESCE，然后才开放新 epoch。supervisor 的全局 reset 仍是 `ref_resetn`，不会因 epoch 复位而停止有序关钟流程。三组 RX 关系也同时采用共享释放，**没有对这些 CDC-11 写接受 waiver**。

## 8 ns 约束及最终物理检查

当前 `radar_cdc.xdc` 对六组持稳数据均有精确层级源/目的集合的 `set_max_delay -datapath_only 8.000`；五组多位总线另有 `set_bus_skew 8.000`。校准报告只有一位，没有跨位 skew，仍须核验该位 max-delay。8 ns 是这些持稳传输的内部约束，不是外部源同步输入预算，也不能用于证明原语 reset/CLR 的恢复/移除。

只有显式 req/ack/status 第一级 D 被用户 false-path；没有宽泛异步 clock group，也没有对 payload 整体 false-path。XPM 原始复位入口的例外来自其官方作用域，不能扩散到任意 CLR/PRE 或下游释放链。两 DIV CLR 与六原语 RST 的 max/min/恢复/移除另行审查。

最终每组必须同时具有以下证据：

- 源与目的时钟身份正确；源保持寄存器、每个目的 D 和存活位集合非空且精确一致，任何复制、合并或裁剪都有逐位解释。
- req/ack 两个方向实际均为三级 ASYNC_REG，同一取消事件到达两端；数据采样条件对应已审查协议。
- 每组共享复位只有一个本域 XPM 入口；外接复位和 PREQUALIFIED 的实际驱动、目的时钟与普通本域时序均符合上表，不能仅靠参数名判定。
- 每个存活目的位有来自正确保持源的真实路径，max-delay 约束实际生效、slack 非负；五组多位 bus-skew 也真实生效且非负。不得用集合总数、缺路径或异常值代替每位结果。
- 核对 ignored/overridden exceptions，payload 未被更宽的 false-path 覆盖；后续同步级及本地复位释放路径没有被批量切断。
- 实际 routed DCP、报告集合、最终源/XDC/recipe、完整 SIM_PASS 与构建身份一致。综合的 estimated delay 或本报告中的 `Max Delay Datapath Only` 标签不能替代布线测量。

## 旧原生报告的功能状态与复位入口修复

build02 原生报告已把 `u_receive/complete_reg`、`fault_reg`、`idle_observed_reg`、`arm_pending_reg`、packetizer `first_data_seen_reg` 及 `u_control/abort_level_reg` 识别为源寄存器到三级同步器的 CDC-3 Info。PHY ready/executing、heartbeat 和 GPIO 输入链也按实际对象识别；完整统计是 36 项 CDC-3，其中包含官方 FIFO 两条内部同步记录，不能把所有 Info 都笼统当作自定义状态。

build02 的 14 项 CDC-9 Info 均为三级 XPM reset 结构，包括 common/command、PHY epoch、activity/ref release 和 BITCLK 资格链。REF `cal_ready` 被综合合并为 `u_cal_ctrl/clock_enable_ref_reg`，`u_cal_ready_rx` 与 `u_ref_ready_sync` 的报告源均为该寄存器；最终 checker 须证明精确别名，而非为了匹配旧名字放宽任意源。

唯一 Critical 是 build02 `cdc.rpt` 第 765 行：

```text
CDC-10, depth 3, False Path
rst_phy_ref_300m/U0/PR_OUT_DFF[0].FDRE_PER/C
  -> radar_capture_bd_0/inst/u_phy/u_phy_sys_release/
     u_reset_sync/arststages_ff_reg[0]/CLR
```

这是旧版 `sys_resetn && !ref_reset` 合并后进入 XPM 的自定义复位入口；厂商 XPM 的入口例外没有证明合并逻辑无毛刺，故不能据其 False Path 标签豁免。独立 `u_ref_reset_sys_release` 经 SYS `phy_allow_sys` 所有者再进 `u_phy_sys_release` 的修复，已在 build03 消除该 CDC-10，并由 build05 routed CDC 再次确认。REF 资格撤销依赖持续 SYS 的下一有效边沿接管；全局 SYS 原始 reset 则即使 SYS/REF/BITCLK 停止也能异步断言安全。build05 的 Critical 为 0；build03 的局部修复与 build04 的检查器失败仍保留其各自证据身份。

## 当前完整回归与最终物理结果

本次只读复核以下实际结果及其记录输入哈希，均与当前源一致：

| 证据目录 | 实际结果 | 范围 |
|---|---|---|
| `outputs/m2_closure_20261013_c1/shared_reset_01` | 2 项 PASS，5 个输入哈希匹配 | `tb_reset_cdc` 的 shared SYS/RX release、精确共同消费者、PREQUALIFIED HOLD、短脉冲停高/停低取消；`tb_cdc` 的默认接口兼容、65 次传输和无旧 ACK 回放 |
| `outputs/m2_closure_20261013/phy_shared_epoch_01` | 7 项 PASS，19 个输入哈希匹配 | 真 UNISIM PHY、默认及四种附加相位 DELAY、停 REF 新 RECAL 反例；`tb_phy_closure` 42 次有序复位序列 |

以上两组均为 `required_suite=false` 的专项。正式完整门禁为 `outputs/m2_closure_20261013_c1/sim_06`，17/17 PASS、`required_suite=true`，31 个输入和 42 份日志 SHA-256 与当前一致；`T6_gate_tests_10.log` 为 88/88 PASS。integrate07/08 连续原生刷新均 PASS。旧 sim05、build03 或 build04 不作为本轮最终结果。

| build05 routed 项目 | 实际结果 |
|---|---|
| 内部门禁 / PHY / DRC / 原生日志 | `internal_status=PASS`；`PHY_RULE`、`CDC_SIGNOFF`、`DRC_SIGNOFF`、`NATIVE_LOG_SIGNOFF` 均 PASS；三个 unexplained 集合为空 |
| 时序 | WNS 1.085 ns、WHS 0.010 ns、WPWS 0.000 ns；TNS/THS/TPWS 均 0，setup/hold/pulse 失败数均 0 |
| 路由 | 16932/16932 routable nets 全部完成，错误 0 |
| 时钟 / 内部约束 | no_clock、constant_clock、multiple_clock、unconstrained_internal_endpoints 均 0 |
| CDC 负载 | 649 目的位全通过；六组最差 max-delay slack 依次为 7.153/7.038/7.266/7.395/7.188/7.605 ns |
| 多位 skew | CMD/状态快照/tap快照/DELAY目标/DELAY报告分别为 0.951/1.087/0.830/0.698/0.895 ns，均小于 8 ns；单比特 cal report 不适用跨位 skew |
| PHY 复位物理 | 9 个原语 RST/CLR、12 个 clock leaf、共享 gate/clear arc 检查 PASS；关闭传播余量 17.062 ns，primitive→controller guard 扣除传播上界后 45.328 ns |

六个 IDELAY/ISERDES RST 的原生库没有 minimum recovery/removal 检查，相关 min slack 保持 null，不能改写为 0 或“全都有 R/R”。两 DIV CLR 和 IDELAYCTRL 的原生 recovery/removal 仍正常计时。外部仍有 6 个 no_input_delay、3 个 no_output_delay，属于未完成的 R07/板级合同边界，不能用内部时序 PASS 掩盖。

最终源、SIM、实际 synth/routed DCP 与报告集合由 build05 manifest/receipt 串联。独立 ZIP/解压重开由主任务后续 companion 记录；本文件不提前声明该动作完成。`SOURCE_TIMING=PENDING`、`BOARD_ELECTRICAL=PENDING`、`DOWNLOAD_CANDIDATE=NO`、`BOARD_TEST=NOT_RUN`，保持 `NOT_DOWNLOAD_READY`。

交付包正式证据位于包根 `evidence/sim/`、`evidence/build/`、`evidence/reports/synth/` 与 `evidence/reports/route/`。历史 build02/build03、旧源反例和专项目录名称仅作开发溯源，不混作当前交付或最终合格报告。
