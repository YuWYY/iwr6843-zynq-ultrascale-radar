# R01–R03 PHY 时钟域与事务接口合同（2026-10-13）

本文在实现前固定接口，现按实际 RTL 更新。外部 GPIO 的地址和原始 tap 单位保留；新 ABI 状态字段由 CONTROL_ABI 同步版本化。所有新通道均须纳入原生 CDC / max-delay / bus-skew 核验，不能沿用原 585 端点计数。

当前证据：共享 epoch 资格修正后的 `outputs/m2_closure_20261013_c1/sim_06/SIM_PASS.json` 全部 17 次回归 PASS，31 个输入与 42 份日志哈希逐一匹配，门禁 SHA256 为 `5e7a4b85b24337a6274cad33b2059afe0c4b668e3931fb4836a07eef9fb837d3`。七次 `phy_shared_epoch_01` 专项也全部 PASS，含 42 次有序复位和 REF 停钟漏取消反例。当前 build05 原生综合、布线及内部发布门禁 PASS：WNS=1.085 ns、WHS=0.010 ns、WPWS=0.000 ns，16932 条可布线网络全部完成、路由错误 0；routed DCP SHA256 为 `f2a2cb73ee50b2cf8a39e119a04ac1e6e210c39b4a43a275bb769cfd16ec6603`。R07 的 SOURCE_TIMING/BOARD_ELECTRICAL 仍为 PENDING，DOWNLOAD_CANDIDATE=NO、BOARD_TEST=NOT_RUN。历史 sim_05/build03 不能继承为当前源签核。旧源 FAIL 与新源 PASS 的输入/日志 SHA、固定 RDY 注入边界见 IMPLEMENTATION_REVIEW 的 REF 停钟反例小节。

| 域 | 职责 |
|---|---|
| SYS100 | cfg 受理、所有权、阶段、终态代次、持续运行 watchdog、持久取消隔离 |
| REF300 | 原语/控制器有序复位、64 REF 周期保持、RDY 资格、校准失败锁存；不驱动动态 LOAD 或 CNTVALUEIN |
| RX75 | IDELAYE3.CLK 和 ISERDESE3.CLKDIV 同网；EN_VTC 窗口、真实基准、每 lane 独立 LOAD / 最大 8 tap 步进、真实读回 |
| BITCLK300 | 从独立真实全速缓冲时钟建立两个 BUFGCE_DIV（SERDES /1 与 RX /4）的共同 CLR 释放资格 |

## 实际实例及持稳通道

以下均使用 radar_mailbox 的 `src_hold` → `dst_data` bundled bus、3 级 req/ack 同步和共同取消断言。同一 epoch、同一时钟域的全部消费者共用一个本地 reset owner；四个 PHY mailbox 均设置 EXTERNAL_SRC_RESET=1、EXTERNAL_DST_RESET=1，通过 src_resetn_in/dst_resetn_in 直接接收该资格，不再生成各自释放链。字段捕获后保持至 ACK 返回；源端不得把 delivery ACK 当执行完成。

| epoch | SYS 共享 owner | 远端共享 owner | 共同消费者 |
|---|---|---|---|
| rx_epoch_resetn | u_rx_epoch_sys_release | u_rx_release，RX | 两个 DELAY mailbox 的对应本域端；RX 执行器也使用同一 rx_resetn |
| cal_epoch_resetn | u_cal_epoch_sys_release | u_cal_epoch_ref_release，REF | 两个 RECAL/report mailbox 的对应本域端；REF supervisor 的 cal_enable_ref 也直接使用该资格 |

REF supervisor 的全局 resetn_ref 仍由 u_ref_release 产生，不由 epoch 异步硬复位。共享 cal_enable_ref 在 REF 停止期间也异步撤销并保持至恢复后的本地释放；普通 D 同步器 u_cal_enable_ref 已删除。这样恢复首沿必然先清旧 explicit_request/report_pending，同时继续执行有序 QUIESCE。

| PHY 内实例 | 源 → 目的 | 宽度/含义 | 取消生命周期 |
|---|---|---|---|
| u_delay_mb | SYS → RX | 27：FR/D0/D1 绝对目标，各 9 bit | rx_epoch_resetn；超时/失败后持久拉低，显式 RECAL 才开启新 epoch |
| u_delay_report_mb | RX → SYS | 59：kind[58]、result[57:54]、baseline[53:27]、readback[26:0] | 与 u_delay_mb 和 RX 执行器同一 rx_epoch_resetn |
| u_recal_mb | SYS → REF | 1：显式 RECAL 请求 | cal_epoch_resetn；取消时请求/结果立即隔离，REF 监督仍运行以执行有序复位；每次受理 RECAL 先隔离 8 SYS 周期再投递新请求 |
| u_cal_report_mb | REF → SYS | 1：显式 RECAL 成功/失败，或启动自动校准失败报告 0；启动成功不发送报告 | 同一 cal_epoch_resetn；自动失败只锁存 phy_fault_sys=9，不生成软件命令完成、不改变 cfg_result/cfg_generation |

综合裁剪合同：`u_delay_mb` 的 0..26 bit 均为运行时目标。`u_delay_report_mb` 的逻辑格式仍为 59 bit，但 SYS 实际只消费 kind/result/baseline，即 [58:27]；低 [26:0] readback 未被 SYS 消费，软件 tap 测量走独立快照通道。build02 原生映射及已审核 `scripts/m2_closure/cdc_contract.tcl` 分别登记源 KEEP [58:0] 共 59 位、目的 [58:27] 共 32 位；仅零消费者的源 [26:0] 允许在后续优化中裁剪。bit57 虽在当前 RX result 编码中恒零，build02 仍实际保留，不能凭编码猜测它不存在。`u_recal_mb` 是常量 1 的纯事件，目的端 `dst_data` 未使用，允许数据位完全裁剪；req/ack 三级及两端取消仍须核验，不能为不存在的数据位制造路径。`u_cal_report_mb` 的成功/失败值可变，bit0 必须保留。源、目的数量和常量保留分别登记；build05 已按当前网表逐位追溯，649 个持稳总线目的端覆盖通过，未由旧源对象计数直接签核。

单 bit CDC 的真实源寄存器：`u_delay_ctrl/ready` → `u_ready_sync`，`u_delay_ctrl/executing` → `u_rx_exec_sync`，`u_cal_ctrl/cal_ready` → `u_ref_ready_sync` 和 `u_cal_ready_rx`，`u_cal_ctrl/executing` → `u_cal_exec_sync`，`heartbeat_counter[2]` → `u_activity_sync`；`u_bitclk_release/released`（BITCLK）→ `u_gate_open_ref`（REF）。以上普通状态同步器均为三级，源资格在对应域寄存。cal_epoch 不再属于此类 D 同步状态，而由上表的共享异步断言/同步释放 owner 传递。

上表使用 RTL 名称。cal_ready 因与 clock_enable_ref 完全同值而合并，两个同步器第一级 D 的实际直接源均为 `u_cal_ctrl/clock_enable_ref_reg/Q`。该已审核别名不允许任意组合路径或其他源替代；build05 已重新核实直接 Q、三级链及真实源/目的时钟。当前 routed CDC 仅有 CDC-3=35、CDC-9=8、CDC-15=649，CDC-10/11 和 Critical 为 0；Warning 按实际持稳协议及路径约束审阅，未降级或笼统豁免。

复位实例：原始 REF reset 单独进入 SYS 的 `u_ref_reset_sys_release`（HOLD=1），`ref_reset_sys_n` 由 SYS 寄存器 `phy_allow_sys` 捕获；该寄存器只由原始 SYS reset 异步清零。`u_phy_sys_release`（SYS，HOLD=1）只接 `phy_allow_sys` 的 Q，避免多域组合逻辑先进入 reset 同步器。REF 资格撤销在连续 SYS 的下一拍接管，SYS reset 即使所有时钟停止仍可异步断言；解除资格必须等待真实 SYS 及各目的域时钟。`u_ref_release` 由全局 `phy_sys_resetn` 释放，REF 监督不再被命令 epoch 异步重置；`u_rx_release` 由 SYS 的 `rx_epoch_resetn` 持久断言、在 RX 释放；`u_activity_release` 只受全局 PHY reset，不随命令 epoch 清除。`u_bitclk_release` 只接 REF 源寄存 `u_cal_ctrl/clock_enable_ref`，在 `u_bitclk_buf/O` 的独立真实全速 BITCLK 下同步并等待 4 周期，输出 `bitclk_resetn` 经同一个反相网驱动 `u_serdes_div/CLR` 和 `u_rx_div/CLR`。两个 DIV 同 I、CE=1、除数分别 1/4，ISERDES CLK/CLK_B 接 /1、CLKDIV 接 /4。`u_deser_release` 在 RX 对同域 `rx_resetn && rx_ready` 做 64 周期释放，只表示应用可以使用反串行数据，不再驱动原语 RST。原始异步资格的 owner 使用公共 XPM reset 同步结构；u_deser_release 已配置 PREQUALIFIED=1，仅对共享本域资格执行 HOLD，不重复实例化 XPM。RTL generate 层级为 g_async/u_reset_sync，实际综合网表保留为 g_async.u_reset_sync。

## 原语有序复位与物理时间界限

[UG571 v1.16 p188–189](https://docs.amd.com/api/khub/documents/kFbaUC5HGcXyGNauhgU6Gw/content#page=189) 的 Component Mode Reset Sequence 要求先释放 IDELAY/ISERDES，再释放 IDELAYCTRL，RDY 后再允许应用。这与应用层继续保持 decoder 无效是不同的资格。

具体执行：SYS 先撤 RX epoch，令 EN_VTC 异步回高、LOAD 清零。REF 监督清 `clock_enable_ref`，两 DIV 的共同 CLR 异步断言；REF 经三级回读确认 `gate_open_ref=0` 后，再等 8 REF 周期（26.7 ns）才断言 `primitive_reset`（三个 IDELAY 和三个 ISERDES）及 `idelay_reset`（IDELAYCTRL）。两 RST 保持 64 REF 周期（213.3 ns）；先撤 `primitive_reset`，再等 16 REF 周期（53.3 ns）撤 `idelay_reset`；观察 RDY 并稳定 16 REF 周期后，才置 `clock_enable_ref`。此后共同 CLR 还需真实全速 BITCLK 的三级同步及 4 周期 guard 才释放。无 BITCLK 时 REF 校准可以完成，两个输出时钟继续被 CLR 保持低，绝不伪造时钟。

两 RST 寄存器仅使用配置 INIT=1，运行中的全局 reset 不会直接异步将原语 RST 置高。运行时先关闭时钟、撤销 RX 资格；即使 REF 当时停止，恢复后仍须完成 QUIESCE 才改变原语 RST。这样不依赖两个异步 reset 树电气同时到达。CLR 断言可以把停高输出强制拉低，不能宣称断言阶段没有边沿；RST 改变时两个时钟已稳定为低，应用资格也已撤销。

物理门禁必须证明 `primitive_reset_reg/Q` 到六个原语 RST 的实际传播小于 8 ns，不能拿 RTL 的 53.3 ns 间隔代替路径报告；两 DIV/CLR 的真实 max/min setup/hold/recovery/removal 必须保留。不得使用会切除 hold/removal 的 `-datapath_only` 异常来通过这些控制路径；SERDES300、RX75 均须维持与真实 BITCLK 的生成关系。

build05 routed 证据已完成上述内部门禁。两 DIV 的原生 recovery/removal slack 为 `/1` 1.201/0.965 ns、`/4` 1.085/1.020 ns，IDELAYCTRL 为 1.449/0.104 ns。六个组件 RST 的 max datapath 为 1.058–1.326 ns，满足 8 ns 传播界限；这些 RST 的 native library 不提供 recovery/removal，min slack 如实为 null/NO_LIBRARY_CHECK，不把空值称为通过。12 条实际时钟叶网最大 1.389 ns、全部非估算，关闭保护预算 17.062 ns。

同一 routed DCP 的独立只读补证亦实际 PASS：三 lane EN_VTC 取消传播上界 5.668/5.895/5.969 ns，8REF=26.664 ns 保护窗口内最小余量 20.695 ns；组件 RST 含源时钟叶网/C→Q 的最坏传播 2.551 ns，对控制器晚 16REF=53.328 ns 释放的余量 50.777 ns。数值来自实际布线网络及 native 库传播弧之和，**不是 STA slack、模拟电路或板上证据**；异步 CLR/PRE→Q 即使 IS_DISABLED=1，也只计作 library propagation-only。补证脚本、日志及输出共 11 份证据文件与哈希收据在 `delivery/20261013_c1/verification/phy_final_probe/`，与当前 routed DCP SHA 分开绑定，不改变 R07 PENDING。

delay report kind=0 为启动/RECAL后的首次实际基准观察；kind=1 为软件 DELAY 的终态。无 RX 时钟时不产生基准观察、不制造有效值。完整字段先源端寄存，再发 report。SYS 原子捕获 baseline_data，可直接供控制状态页读取；对外 tap snapshot 则从 RX 域的稳定 shadow 获取。

完成 report 采用另一独立 mailbox，使多位结果及读回与完成标志原子关联。失败时 SYS 先保持执行器及请求/结果通道的取消复位，再结束所有权；旧 report 和 req/ack 由同一本域共享 owner 清除。新 epoch 只能经明确 RECAL 打开，不能由恢复的 BITCLK 自动重用。取消优先于同周期完成。

## PHY 顶层接口约定

保留 cfg_send、cfg_data[27:0]（bit27=RECAL）、cfg_busy；新增 cancel_sys。输出 arm_admissible_sys、ref_cal_ready_sys、rx_ready_sys、baseline_valid_sys、baseline_data[26:0]、baseline_generation_sys[31:0]、cfg_result[3:0]、phy_fault_sys[3:0]、cfg_phase[2:0]、cfg_generation[31:0]。旧 ready_sys 是 rx_ready_sys 的兼容别名，不能再用于无 BITCLK 的 ARM 准入；ref_ready 是 REF 源寄存资格。

delay_readback[26:0]、baseline_readback_rx[26:0]、tap_valid_rx、tap_generation_rx[31:0] 属于 RX 域，外部必须经快照/持稳协议读取，禁止逐位同步。tap_valid_rx 等于 RX 的 ready 源寄存器，合法稳定观察完成且当前无 DELAY 执行时才有效。tap_generation_rx 是实际测量代次，每次完成合法稳定读回窗口加一；它跨本地取消/RECAL 保留，只在全局 RX reset 清零，无时钟不改变。SYS 的 baseline_generation_sys 仅在真实 kind0 基准报告被原子捕获时加一。RECAL 在 REF 完成即结束 SYS 命令所有权，无 RX 时基准仍无效；不永久阻挡下一次 ARM。

result：0 NONE，1 SUCCESS，2 NOT_READY，3 BELOW_BASELINE，4 READBACK_INVALID，5 NO_CONVERGENCE，6 RDY_LOST，7 TIMEOUT，8 CANCELED，9 CALIBRATION_FAILED。

phy_fault_sys 是独立的 SYS 锁存故障诊断，沿用 result 编码；每次持久隔离锁存原因，全局 reset 或显式 RECAL 真正受理时清零。它覆盖启动自动 REF 校准超时/SETTLE 丢失（9）、自动基准失败与空闲 RDY 丢失，不需要制造软件命令或伪增 cfg_generation。简单 NOT_READY/低于基准拒绝未隔离 PHY，故不设置这个锁存。

phase：0 IDLE，1 ACCEPTED，2 DELIVERED，3 EXECUTING，4 SUCCEEDED，5 FAILED，6 CANCELED。阶段经握手/源寄存单 bit 取得；短阶段允许查询时跳过，但不得倒退或把 delivery 写成 success。cfg_generation 每次终态增加一次，终态保持到下一事务。

## 启动、取消与超时

- ARM 准入只需 REF 资格和无 PHY 失败锁存，不要求 BITCLK、RX ready 或基准已取样。采集激活继续等待 RX 实际 ready 和官方 FIFO TREADY。
- 无 BITCLK 的 DELAY 由 SYS 有界拒绝；执行期间失钟由 SYS watchdog 终止、持久隔离，恢复时钟不得出现旧 LOAD / 完成。
- SAFE / ABORT / RECOVER / DMA reset 在事务在途时取消 PHY；没有 PHY 在途时不无故销毁已建立的校准，采集侧公共取消仍单独执行。
- 三 lane 最坏 511 tap 跨度，每步最多 8 tap，最多 64 轮；按每轮建立/LOAD/稳定观察及前后窗口推导时间。正常 DELAY 的默认 SYS 上限为 10000 周期（100 us），远大于 RX75 的约 20 us 最坏执行量；REF RDY 上限 300000 周期（约 1 ms），SYS RECAL 上限 120000 周期（1.2 ms）。RX 活动监测以真实 RX 寄存计数 bit2 为源，SYS 连续 128 周期（1.28 us）无变化即失去资格。
- 真实 RDY 失去资格、无效读回、不收敛、超时均锁存不可用，显式 RECAL 才重建；拒绝低于基准的目标不产生 LOAD。

## RX 更新时序合同

EN_VTC 拉低后等待至少 12 RX 周期再读原语；新 CNTVALUEIN 建立 2 RX 周期后 LOAD；LOAD 每 lane 为 1 RX 周期；LOAD 后至少 12 RX 周期再读取实际值；相邻 LOAD 间隔因此大于 5 RX 周期。最后实际读回一致后继续保持低 VTC 12 RX 周期，再恢复 VTC 并等待 16 RX 周期。初始基准只读，不 LOAD 零；合法下限采用实际校准基准，不硬编码典型 tap。

无效读回的综合语义：RTL 使用普通逻辑相等比较两次稳定窗口样本，并逐轮核对期望实际 tap，配合最大 64 轮及 SYS watchdog。4-state 仿真 X 不能使条件成功；实体硬件仅保证样本一致性、目标一致性与有界终止，不宣称能检测数字硬件中不存在的 X 值。

此文是实现合同；当前内部 PASS 依据同一冻结源的 sim_06 及 build05 `release_status.json`、`route_identity.json`。R07 真实源同步输入与板级电气合同未完成，外部 6 个 no_input_delay、3 个 no_output_delay 如实保留为 PENDING；内部 no_clock/未约束端点为 0。仿真和内部布线通过不替代板级校准、数据眼、电气或板上运行验证。

## 专项实际证据（诊断阶段）

当前复位专项 `outputs/m2_closure_20261013/phy_shared_epoch_01/STATUS.json` 七次运行全部 PASS、返回 0、输入哈希稳定，共 42 次有序复位。新增用例停在旧 RECAL SETTLE 末→取消→REF 仍停时提交新 RECAL→恢复 REF，禁止旧报告终结新所有者。仅该数字竞态窗口固定 RDY=1，待新请求到达并进入原语复位即解除；它不代表自然原语校准。正常串行、非零 tap、RDY 故障、失钟及 GPIO 五种相位回归均保留。以下为历史开发诊断，不能单独签核当前源。

- `outputs/m2_closure_20261013/phy_dev_03/run_tb_delay_sequencer.log`：实际 XSim PASS；非零基准、等值/±1/±8/±9/511、相反方向、每 lane 步长/等待、X/不收敛/RDY 故障，共核验 742 次 lane LOAD。
- 同目录 `run_tb_phy_closure.log`：实际 AMD UNISIM PASS；无 BITCLK 的 ARM 准入/DELAY 拒绝/RECAL、所有 1..10 执行阶段分别停高/停低、有界 SYS 取消/超时、恢复无旧 LOAD/结果、同边界取消优先、测量代次、RDY 故障及真实 BITCLK 分频复位释放。
- `outputs/m2_closure_20261013/phy_dev_04/STATUS.json`：诊断子集全部 PASS，输入哈希未变。`tb_delay_transaction` 在 0/0.25/0.75/1.25/2.25 ns REF 相位下，160 条紧邻 GPIO 命令只受理 1 条、只终结 1 次，实际 42 tap 由 6 轮 LOAD 完成；执行 busy、RDY 失败和 RECAL 恢复通过。`tb_phy_unisim` 实际解码 144 个 RAW16 pair，两套非零三 lane tap、RDY 故障/RECAL 通过。

`phy_dev_03` 的整体状态仍是 FAIL，因为随后旧 GPIO TB 在 PHY 接收注册 send 之前错误判定完成；该 TB 交接等待已修正，后继 `phy_dev_04` 通过。不能把 dev03 整体标 PASS，也不能把这些诊断子集代替根任务完整 `SIM_PASS.json`。

命令结果与自动准备故障的边界：cfg_result/cfg_generation 只描述软件事务。RECAL 的 REF 阶段成功后，随后的自动 RX 基准建立仍可能失败；此时 baseline_valid/ARM/RX ready 清零并保持隔离，phy_fault_sys 锁存独立诊断，但不会伪造软件终态代次。软件必须同时检查资格及故障字段，不能只凭此前的 SUCCESS 认定 RX 可用。该独立诊断是上述 dev03/dev04 后的增补，已包含于历史 sim_05，当前共享 epoch 专项也保留。

`outputs/m2_closure_20261013/phy_fault_01/STATUS.json` 曾实际 PASS、输入哈希稳定：同一个原生 `tb_phy_closure` 增加“无钟 RECAL 的 REF 已 SUCCESS，随后首次真实 RX 基准读回被注入 X”及“无软件事务时 RDY 丢失”，分别锁存 4/6，旧命令结果和代次不变；恢复数据/RDY 不自清，明确受理 RECAL 清码并恢复。所有原失钟、取消、重放和分频复位测试保留；旧专项不签核当前修改。

有序复位后 `outputs/m2_closure_20261013/phy_order_01/STATUS.json` 曾实际 PASS，源与日志身份稳定：`tb_phy_closure` 直接监测原语 RST 的两时钟静低窗口、VTC guard、64REF保持、16REF先后释放、共同CLR真实BITCLK资格与首上升沿对齐；原 `tb_phy_unisim` 144 RAW16 pair 和 GPIO 默认/四种附加相位全部通过。当前共享 epoch 源须使用新的完整回归、CDC 和最终 route 证据。
