# M2 R01–R05 实现复核

日期：2026-10-13。工程：`/fpga`。需求依据：`/home/user/Downloads/F20_M2_完善要求_Codex_20261013.md`。本文件记录已授权实现的实际源码、测试及尚待签核项目，不将需求中的目标自动当作已完成证据。

## 1. 当前结论与证据身份

R01–R05 的控制域、合法 tap 步进、事务所有权、源寄存状态、复位结构和原语复位次序均已落实到生产 RTL。启动自动 REF 校准失败有独立软件诊断，每次 RECAL 受理先清除旧 REF 请求/结果 epoch；REF reset 独立同步后交给 SYS 寄存所有者。最新修正进一步让同一 epoch、同一时钟域的邮箱与执行器共享一个异步断言/同步释放资格，保证 REF 停止期间发生的取消不会被普通 D 同步器漏掉。

**当前共享 epoch 修正的 sim_06 完整 17 次回归 PASS；build05 原生综合、布线及内部发布门禁 PASS。R07 的真实输入时序与板级电气仍为 PENDING，DOWNLOAD_CANDIDATE=NO，BOARD_TEST=NOT_RUN。** sim_05/build03 属于修正前版本，build03 的 CDC-11 揭示了真实取消协议缺陷，不能保留为当前源发布 PASS。当前内部通过不等于源同步输入时序或板上功能通过。

本轮测试证据与版本边界：

- 当前完整 `outputs/m2_closure_20261013_c1/sim_06/SIM_PASS.json`：13 个 TB、17 次运行全部 PASS，包含共享 epoch 资格和旧报告反例。逐一重新核对 31 个输入和 42 份日志，哈希全部匹配；门禁 SHA256 为 `5e7a4b85b24337a6274cad33b2059afe0c4b668e3931fb4836a07eef9fb837d3`。
- 当前 `outputs/m2_closure_20261013_c1_build05/release_status.json`：SIMULATION、NATIVE_ROUTE_EXECUTION、PHY_RULE、CDC_SIGNOFF、DRC_SIGNOFF、NATIVE_LOG_SIGNOFF 及 internal_status 均为 PASS。WNS=1.085 ns、WHS=0.010 ns、WPWS=0.000 ns，setup/hold/pulse 失败数均为 0；16932 条可布线网络全部完成，路由错误为 0。实际 routed DCP SHA256 已重读核对为 `f2a2cb73ee50b2cf8a39e119a04ac1e6e210c39b4a43a275bb769cfd16ec6603`，源与报告绑定见同目录 `route_identity.json`。
- 当前 `outputs/m2_closure_20261013/phy_shared_epoch_01/STATUS.json`：七次 PHY 专项全部 PASS、返回 0、输入哈希稳定，含 42 次有序复位及取消后旧 REF 报告禁止终结新 RECAL 的反例回归。默认与四种附加 REF 相位 GPIO 事务、144 RAW16 pair 原生 UNISIM 测试均通过；专项不替代完整门禁。
- 历史完整 `outputs/m2_closure_20261013_c1/sim_05/SIM_PASS.json` 曾为 13 个 TB、17 次运行 PASS，31 个输入和 42 份日志逐一核对匹配；SHA256 为 `f3f85aa3a44752d9155aa10514a2a1f084b20e32bf4d460082bb3d97af87a53d`。它不包含最新共享 epoch 修正及反例，不能用于当前源签核。
- sim_03、sim_04、phy_reset_owner_01 和此前 PHY 诊断目录亦仅为旧版本历史证据。

build02 是旧源诊断：唯一 CDC-10 对应 `sys_resetn && !ref_reset` 合并后送 XPM。build03 已实证该 CDC-10 关闭，但后续 CDC-11 复核发现同一 epoch 分别同步后重汇合、REF 使能漏低的真实反例；该版本被阻断而非豁免。当前共享资格实现的 build05 已重新综合和布线，实际 CDC 报告仅有 CDC-3=35、CDC-9=8、CDC-15=649，CDC-10/11 和 Critical 均为 0。649 条持稳总线 Warning 按当前端点、握手及 max-delay/bus-skew 覆盖审阅，未笼统删除或降级。

当前 PHY 冻结身份：

| 文件 | SHA256 |
|---|---|
| `rtl/phy/radar_lvds_phy.v` | `b38fae364479425e987be14429882b490f459f74eb9e26d0c871f39c5a434040` |
| `rtl/phy/radar_phy_cal.v` | `be817cdde8235f13e46d21633c7ac8f7b57926d56d9f9b26cc062673928e1aab` |
| `rtl/phy/radar_phy_delay.v` | `7e1603bf28eb4470dfb387624204df013a71b1b959481188eb4b8ac1d3ba4a82` |
| `sim/tb_phy_closure.sv` | `0a04a6435ce6c9460ac4c5f9cd1793fc56633a0dc75017a79030ccffe4453028` |

## 2. 实际职责与跨域合同

| 域 | 生产模块及职责 |
|---|---|
| SYS，名义 100 MHz | `radar_control`：GPIO 接受/拒绝、会话、故障、时间戳；`radar_lvds_phy`：PHY 事务所有者、结果/代次、超时、持续隔离 |
| REF，300 MHz | 新增 `radar_phy_cal`：原语复位顺序、RDY 等待/稳定检查、时钟允许、校准结果；不驱动动态 tap LOAD |
| RX，75 MHz | `radar_phy_delay`：实际基准、低 VTC 读回、独立 lane 步进；`radar_receive`/decoder/packetizer：真实串行数据、ARM 待激活、AXIS |
| 独立 full BITCLK，300 MHz | `u_bitclk_buf`：始终保留真实输入时钟；`u_bitclk_release`：两个受控时钟输出的 CLR 释放资格 |

PHY 内部使用 SYS→RX 27bit DELAY 请求、RX→SYS 59bit 原子报告、SYS→REF 1bit RECAL 事件、REF→SYS 1bit 校准结果。原有采集命令和两个快照仍分别承担采集命令、RX 状态、tap 测量复制。完整实例、字段、保持窗口、req/ack 三级链及新增状态同步器见 `docs/m2_closure/clock_channels.md`。逻辑宽度不等于物理端点数量：旧源 build02 的 DELAY 报告源保留 59 个 KEEP 位，SYS 只消费 [58:27] 的 32 个目的位；未使用的低 27 位可被裁剪，bit57 当时仍保留。最终按当前网表分别核验源、目的及逐位覆盖，不沿用旧 585 端点数或仅凭枚举值推断裁剪。

GPIO 外部接口和原始绝对 tap 单位保留。ABI 已版本化为 `0x52430101`，详细页表见 `docs/m2_real/CONTROL_ABI.md`。内部新增端口未新增 BD 外部业务接口，官方 FIFO/DMA 未由自定义替代模块取代。

## 3. R01：PHY 控制域与器件连接

`radar_lvds_phy` 的三个 IDELAYE3.CLK 和对应 ISERDESE3.CLKDIV 均接 `rx_clk`；IDELAYCTRL.REFCLK 独立接 300 MHz。动态 EN_VTC、CNTVALUEIN、每 lane LOAD 与 tap 观察全由 RX 状态机产生。SYS 和 REF 通过受理/交付/执行报告协调，未采用跨域 busy 的组合 OR。

资格已分开：`ref_cal_ready_sys` 表示 REF 校准，`rx_ready_sys` 还要求真实基准、RX 近期活动和无 PHY 命令在途，`arm_admissible_sys` 允许在无 BITCLK 时提交一次 ARM。应用解码资格仍另等基准、64 个 RX 保持周期及接收启动条件。

复核进一步发现旧的“IDELAY/IDELAYCTRL 同时退出复位、ISERDES 待 RDY 后才退出复位”未满足厂商 Component Mode 顺序。现已拆分原语 RST、控制器 RST 和应用资格，并增加真实 BITCLK 的 `/1` 受控全局时钟。依据为 [UG571 v1.16，2025-01-14，p188–189](https://docs.amd.com/api/khub/documents/kFbaUC5HGcXyGNauhgU6Gw/content#page=189)：先释放组件，再释放控制器，校准完成后才放行应用。实际实现时序见 R05。

源码同网和 UNISIM 数据测试已覆盖该实现；build05 实际网表三组 CLK/CLKDIV 同网、REQP-1742=0、两路专用时钟结构及当前固定引脚的布线可行性已通过原生门禁。板级电压与真实接口时序仍属 R07。没有使用规则降级、`CLOCK_DEDICATED_ROUTE=FALSE` 或不存在于目标器件合同的 HARDSYNC_CLR 绕过。

## 4. R02：实际基准、合法步进与失败诊断

`radar_phy_delay` 在校准后第一次合法低 VTC 窗口读取三条原语的真实 CNTVALUEOUT，连续两拍相等才保存基准和读回。启动及 RECAL 不用 LOAD 零覆盖校准。基准在本地隔离/RECAL 时失效，仅新的真实 RX 观察可重建。

执行 DELAY 时先重新读取当前实际值，以扩展有符号差值计算 `clamp(target-current,-8,+8)`。每条 lane 使用独立 LOAD；已到目标的 lane 不 LOAD。9bit 原始目标及实际值在 0..511 编码范围内，步进单调靠近目标，不采用会回绕的 9bit 无符号减法。低于该 lane 实际校准基准的目标返回 3，保留原设置，不把文档典型 Align_Delay 写死成统一下限。

| 操作 | 实现等待 |
|---|---|
| EN_VTC 拉低至初次采样 | 至少 12 RX 周期，再取连续两次样本 |
| CNTVALUEIN 建立 | 2 个 RX 周期后产生 LOAD |
| LOAD | 对需要变化的 lane 保持 1 个 RX 周期 |
| LOAD 后实际读回 | 至少 12 RX 周期，再连续观察及核对本轮目标 |
| 最终读回一致后 | 低 VTC 再保持 12 RX 周期，恢复 VTC 后等待 16 RX 周期 |
| 最大步进轮数 | 64 轮；覆盖 511 tap 最大跨度，每轮每 lane ≤8 |

成功只能来自实际目标一致且完成稳定窗口。READBACK_INVALID=4、NO_CONVERGENCE=5、RDY_LOST=6、TIMEOUT=7、CANCELED=8、CALIBRATION_FAILED=9 均可观察。启动自动 REF 的 RDY 超时/稳定期丢失也通过原有校准报告进入 `phy_fault_sys=9`，不因没有软件命令而丢失诊断。

`phy_fault_sys` 是 SYS 独立锁存，映射 page14；全局 reset 或真正受理显式 RECAL 清零。自动 RX 基准失败锁存 4、空闲已就绪 RDY 丢失锁存 6；它们不伪增软件 cfg_generation，也不把以前的命令 SUCCESS 改写成另一笔命令结果。软件同时检查结果、故障及资格。

仿真 X 的边界：普通逻辑相等比较使 X 样本不能通过仿真条件；实体硬件实际检查连续样本、步进目标一致及有界完成，不宣称硬件能检测四态仿真特有的 X。实际 Align_Delay、温漂和数据眼仍需板上测量。

## 5. R03：所有权、无钟、取消与代次

GPIO ACK 只表示接受。PHY SYS 所有者依次观察 accepted、mailbox delivered、源寄存 executing 和最终原子报告；交付 ACK 不结束 busy。短暂 phase 允许软件查询时跳过，终态及结果保持到下一笔受理。

无 BITCLK：REF 校准、GPIO/UART 控制面仍工作；ARM 可排队；DELAY 有界拒绝 NOT_READY；RECAL 可完成 REF 部分，但不伪造 RX 基准和测量。SNAPSHOT 需要当前 RX 资格；受理后失钟不制造新快照，可由 RECOVER 取消。

执行中超时、取消或硬故障：SYS 先持久撤销 RX/REF 事务 epoch，清除两端旧 req/ack/result，并保持隔离 8 个 SYS 周期后结束 busy。停止的 RX 不需要运行即可清除 LOAD/执行资格。恢复时钟不会自开旧 epoch，硬失败需显式 RECAL。取消与成功报告同 SYS 边界时取消优先，终态代次只增加一次。

每次 RECAL 也先将 REF 请求和结果两端共同隔离 8 个 SYS 周期，随后开放并派送新请求；REF 监督本身持续运行以完成有序复位。此步骤专门排除“上电失败报告仍在 CDC 途中，却终结下一笔 RECAL”的竞态，不新增通道、不伪造命令身份。

### REF 停钟取消反例与修正

旧实现使用普通三级 D 同步器传递 `cal_epoch_resetn` 到 `cal_enable_ref`，但邮箱在 raw epoch 拉低时异步清零。REF 若停在旧显式 RECAL 的 SETTLE 末拍，取消的 8 SYS 低电平可以完全落在 REF 停钟窗口内。邮箱已经换代，监督器却仍保留旧 `explicit_request/report_pending`；恢复 REF 后旧报告能提前终结新软件代次。延长一个固定 SYS 低脉冲不能证明停止的 REF 已看见取消。

已在保持旧生产 RTL 的独立目录 `outputs/epoch_old_02/` 实际复现。旧 PHY SHA256 为 `00fed5d9532cf1a510a9235674d59f1ef2ed3e4c94489daf8b626201a6270bb3`；复现 TB 为 `17760e3167f577d62094e956011b066959292ff15e93eb1562590e9735d9fc82`。`sim_result/run_tb_phy_closure.log:143` 在 138880 ns 报告旧 SUCCESS 已结束新 RECAL，但 `old_sequence=current_sequence=41`、`refready=0`，新复位/校准尚未完成；日志 SHA256 为 `65a58513414f4170c804fdb6b6294f5c7b2608d6d277205fdef0b417f69cfd68`，输入清单 `sim_result/input_hashes.json` SHA256 为 `4ef885bcb5e5ccc935bd23ea343a309455cd963d4b375d3d57c659382ee7b259`。

**注入边界：**该反例仅在暂停 REF 的数字协议窗口暂时 `force idelay_rdy=1`，以隔离官方 IDELAYCTRL 的停 REF watchdog；新请求真正到达且进入原语复位即解除 force。因此它证明取消/报告身份缺陷，不声称是自然 UNISIM 校准行为或板上复现。此前未 force 的诊断出现旧失败报告 code9，但未具备同样完整的时序性质记录；最初漏过 SYS 采样沿的取消刺激返回 code7，仅是 TB 刺激错误，不计为反例证据。

修正将 REF 的唯一 `u_cal_epoch_ref_release` 输出同时送监督器 `cal_enable_ref`、RECAL 邮箱 REF 目的端和校准报告邮箱 REF 源端。它在 REF 停止时异步撤销并保持资格，恢复首沿即可清旧监督请求；监督器仍由全局 `ref_resetn` 释放，继续完成 QUIESCE，不被 epoch 直接异步拉原语 RST。SYS 侧由唯一 `u_cal_epoch_sys_release` 共享；RX 双邮箱和执行器同样共享 `u_rx_release`，SYS 侧共享 `u_rx_epoch_sys_release`。四个 PHY mailbox 均使用 `EXTERNAL_SRC_RESET=1/EXTERNAL_DST_RESET=1`，不再重复内部释放链。

当前 `sim/tb_phy_closure.sv:137` 对任何新终态检查新请求交付和新复位序列完成，`:272` 起保留完整反例刺激，`:281` 明示 RDY 注入。新源 `phy_shared_epoch_01/run_tb_phy_closure.log:143` 实际 PASS、42 次有序复位，日志 SHA256 为 `2010b06226c3c8127113483db05692c07df8f2b21b1070b1a176558297769d38`；该专项全部输入清单 SHA256 为 `b7e5b0c83c19cde7d5b77cb11085873f8798c150d598aaaf9b3432310af23dd8`。其余不使用这项固定 RDY 注入的正常串行、非零 tap、RDY 丢失、停钟与取消回归均保留。

| 参数 | 默认值及名义时间 | 含义 |
|---|---|---|
| DELAY_TIMEOUT_SYS_CYCLES | 10000，100 us | 超过 64 轮步进及 CDC 往返的保守上界 |
| CAL_TIMEOUT_SYS_CYCLES | 120000，1.2 ms | RECAL 受理至终态的 SYS 上界，包含隔离与校准 |
| CAL_RDY_TIMEOUT_CYCLES | 300000 REF，1 ms | REF 等待 RDY 上界 |
| PHY RX_ACTIVITY_TIMEOUT | 128 SYS，1.28 us | 真实 RX bit2 heartbeat 的活动监督 |
| 接收 RX_CLOCK_TIMEOUT | 1000000 SYS，10 ms | 会话侧失钟监督，与 PHY 执行超时不同 |
| FIRST_DATA_TIMEOUT | 500000000 SYS，5 s | ARM 后首数据上界 |

首次真实数据也建立会话活动基准，因此在第一个慢 heartbeat 前停钟不会遗漏故障。纯采集停钟且无 PHY 事务在途可以保留校准，通过显式 RECOVER 重建会话；这不等于执行失败后的 PHY 可以省略 RECAL。

tap 测量及代次在 RX 一起复制到 64bit 快照。`tap_valid_rx` 来自 RX ready 源寄存器，执行中为 0；`observation_generation` 仅在真实稳定观察流程完成时加一，跨本地 RECAL/取消保留、全局 RX reset 清零，无时钟不变化。page0C 是测量代次；page13 是复制代次，重复复制不会冒称新测量。page12 则是 SYS 成功收到新基准报告的代次。

## 6. R04：源寄存状态及完成优先级

`radar_receive` 的 complete、fault、idle_observed 已改为 RX 寄存输出。fault 保持至命令公共 reset；abort 或现有 fault 优先于成功 complete；idle 是带 decoder/stream/deserializer 资格的电平，资格撤销即清，不当作永久事件。pending 本来就是源寄存 ARM 状态。

`radar_control` 使用统一 faults_next 计算下一拍 SYS 粘性故障，abort_level 与它同拍源寄存。SAFE/ABORT/有效 RECOVER 产生 SYS 事务取消，即使当前没有采集会话也可取消 PHY。完成时间戳仅在无故障、无同拍取消时记录，增加的源寄存和三级同步延迟在 ABI 中说明，未把它声称为精密采样时间。

REF cal_ready/executing、RX ready/executing、heartbeat 和新的 clock-gate 回报均有明确源寄存器，再进入三级同步链。RTL 的 cal_ready 与 clock_enable_ref 更新始终相同，实际综合将 cal_ready 合并为 clock_enable_ref_reg/Q；build05 已重新核对该直接寄存源、三级链和时钟身份。多位目标、结果、计数与 tap 使用原子持稳通道，不逐位同步拼接。

理想 RTL 仿真不能重现实际布线组合毛刺。因此 `tb_status_cdc` 的 ACTIVE→FAULT、最终真实 TLAST 握手、STOP 取消 pending ARM、命令取消保留阻塞 AXIS 等测试，仅证明协议行为；最终自定义状态 CDC-10 必须结合实际源 Q、目标第一级 D 及其时钟逐条关闭。

## 7. R05：复位树及原语时钟关闭窗口

公共 `radar_reset_release` 默认将原始异步资格集中送入 `g_async/u_reset_sync` 的三级 XPM 结构。`PREQUALIFIED=1` 仅对已经由本域唯一 owner 释放的资格执行 HOLD，不再增加第二条同步链；PHY `u_deser_release` 采用此模式。HOLD 计数器只使用目的域同步资格；released 支持停钟时断言，解除仍由本域时钟计数。后级恢复/移除路径必须保留时序检查，不能以宏被识别就签署整树安全。

顶层 common_resetn、command_resetn 由 SYS 寄存所有者输出，分别处理 stream/快照恢复与命令取消。官方 FIFO 仍只有现有一个 reset 输入；写侧保持 64 个真实 RX 周期后还需等待官方 TREADY。ARM 在内部 reset-busy 时继续 pending，不能凭固定周期数假定 FIFO 已就绪。

REF 有序复位：SYS 先撤 RX epoch，使 EN_VTC 回高、LOAD 清零。REF 清 clock_enable_ref，两个 BUFGCE_DIV 输出由共同 CLR 置低；收到三级 gate_open_ref 回报为 0 后，再等 8 REF 周期才断言两 RST。保持 64 REF 周期，先撤三 IDELAY/三 ISERDES 的 primitive_reset，再等 16 REF 周期撤 IDELAYCTRL 的 idelay_reset。RDY 再稳定 16 REF 周期才允许输出时钟。

u_bitclk_buf 继续提供不依赖 RX 的真实全速时钟；u_bitclk_release 用该时钟完成三级同步及 4 周期 guard，才释放 `/1` 与 `/4` 的共同 CLR。ISERDES CLK/CLK_B 来自 `/1`，CLKDIV 和 IDELAY CLK 来自 `/4`，保持真实输入来源。运行中两个 RST 寄存器不异步立即置高，只有 FPGA 配置 INIT=1 是初值特例；REF 停止时先关闭时钟/撤资格，REF 恢复后仍经过 QUIESCE。

CLR 异步断言可能把停高输出强制拉低，不能宣称断言期间无边沿；真正改变原语 RST 时两时钟已静低。T5 直接监测该窗口、RST 先后间隔、恢复首沿对齐及恢复后的完整高低脉宽。物理签核还必须证明六个 primitive RST 路径的传播界限、两 CLR 的真实 max/min，以及完整时钟树关闭预算。名义 guard 不是实际布线测量；不得用 `-datapath_only` 切除 hold/removal 来通过控制路径。

build02 对 `u_phy_sys_release` 的原 SYS/REF 组合复位报出唯一 CDC-10。原始 REF reset 独立进入 SYS 的 `u_ref_reset_sys_release`，SYS 寄存器 `phy_allow_sys` 捕获同步资格且只由 SYS reset 异步清零；后级仅由该 Q 驱动。这已在 build03 关闭 CDC-10。REF reset 撤销资格在连续 SYS 下一拍接管；SYS reset 在全部时钟停止时仍异步断言安全。共享 epoch 资格加入后，build05 当前 DCP 已重新确认 CDC-10/11 均为 0。

build05 的九个 reset 端点与十二个时钟叶节点均有原生证据：两 DIV 的 recovery/removal max/min slack 分别为 `/1` 1.201/0.965 ns、`/4` 1.085/1.020 ns；IDELAYCTRL 为 1.449/0.104 ns。六个 primitive RST 的 max datapath 为 1.058–1.326 ns，满足 8 ns 显式传播界限；这些原语的 RST 没有库级 recovery/removal 检查，min 报告如实保存 `NO_LIBRARY_CHECK` 和 null slack，不能称为恢复/移除通过。十二条实际已布线时钟叶网 `estimated=0`，最大 1.389 ns；结合库 CLR→O 上界 0.213 ns，关闭保护预算为 17.062 ns，组件先于控制器释放的 16REF/8ns 保守余量为 45.328 ns。

独立只读 `phy_final_probe.tcl` 已于 19:46:27 在同一 routed DCP 实际 PASS，没有修改约束或保存设计。从共享 SYS 时钟驱动端计算、包含源叶网与 C→Q 的三条 EN_VTC 取消传播上界分别为 5.668/5.895/5.969 ns；对 8REF=26.664 ns 保护窗口，最小余量 20.695 ns。组件 RST 的相同 REF 参考传播上界最坏 2.551 ns，对 16REF=53.328 ns 控制器释放间隔，余量 50.777 ns。这是实际 routed net 与 native 库传播弧求和后的保守界限，**不是 STA slack，也不是模拟电路或板上验证**；被异步例外禁用的 CLR/PRE→Q 弧仍如实记录为 library propagation-only，不冒称有效 recovery/removal 检查。`delivery/20261013_c1/verification/phy_final_probe/` 保存脚本、日志及输出共 11 份证据文件与其哈希收据，与实际 DCP SHA 单独绑定。

## 8. 回归覆盖与证据边界

| 测试 | 已实际覆盖 |
|---|---|
| tb_decoder | 256 组极性/位序/相位、8192 words、畸形 FRCLK |
| tb_packetizer | 有限包、skip、随机停顿、overflow、半包 STOP、阻塞拍 ABORT、TLAST/奇数长度/帧错误 |
| tb_io_guard | 默认关闭、供电资格、UART/NRST、掉电与明确恢复 |
| tb_cdc | 65 次原子传输、快照、RX 停止、取消 |
| tb_control | ABI 1.0.1、BITCLK 前 ARM、DELAY/SNAPSHOT 资格、源寄存 abort、故障/取消优先、诊断页 |
| tb_phy_unisim | 144 个真实 RAW16 pair、两套非零三 lane tap、异相时钟、RDY 故障与 RECAL |
| tb_capture_fifo | 真实串行 UNISIM + 官方异步 8192 FIFO；ARM/idle/no-idle/STOP/ABORT/失钟/DMA reset/RECOVER；满容量 8194 beats 出口逐拍核对；62 个 pending 周期 |
| tb_delay_transaction ×5 | 0/0.25/0.75/1.25/2.25 ns REF 相位；160 条紧邻命令只接受/终结 1 条；42 tap 实际 6 轮 LOAD；RDY/RECAL |
| tb_delay_sequencer | 非零实际基准、等值/±1/±8/±9/511、相反方向、低于基准、742 次 lane LOAD、X/不收敛/RDY |
| tb_early_clock_loss | 首数据早于首 heartbeat 时停钟、粘性故障、恢复 |
| tb_phy_closure | 实际原语、无钟 ARM/DELAY/RECAL、RX 执行状态 1..10 停高/停低、取消/超时/同拍完成、无旧 LOAD/完成；基准/空闲故障；最新 42 次有序复位；启动 RDY 超时/SETTLE 丢失及迟到报告竞态；独立 REF 停钟复位、全部时钟停止时 SYS 异步复位；旧 RECAL SETTLE 停钟后取消/新 RECAL 不接受旧报告 |
| tb_status_cdc | 源 COMPLETE/FAULT/idle、最终握手、pending STOP、取消保持 AXIS |
| tb_reset_cdc | 真实 XPM 3FF+HOLD、8 种停高/停低取消相位、仅有效样本快照、无旧 req/ack |

所有 TB 有有限仿真 deadline。官方 primitive 的通用复位提示不验证本设计已经满足复位时序；实际顺序由新增断言检查，物理时间由本轮 native 门禁检查。仿真注入 RDY/X 是可控数字故障测试，不是模拟板上时钟质量、眼图或模拟校准。

## 9. 当前门禁与 R07 边界

1. build05 内部综合/布线、三组 PHY 同网、REQP-1742、全局时钟结构、当前引脚布线及时序门禁已通过；649 个持稳总线目的端按当前合同覆盖，普通状态与共享 reset owner 结构通过。内部 no_clock 和 unconstrained_internal_endpoints 均为 0。
2. DRC 没有 Error/Critical；仍保留官方 DMA 的 REQP-1934/1935 提示及 SmartConnect RTSTAT-10 Warning，按原生对象审阅，不宣称报告没有 Warning。RTSTAT-10 详细对象和 EN_VTC 取消链传播的独立只读补证已完成，与主构建报告分开绑定。
3. **R07：SOURCE_TIMING=PENDING、BOARD_ELECTRICAL=PENDING。** 当前有 6 个 no_input_delay、3 个 no_output_delay 外部端口，未填写猜测值来掩盖未知合同。当前内部 PASS 不签署这些外部接口，不产生可下载候选；BOARD_TEST=NOT_RUN。
4. 交付或重新生成前仍需保持 sim_06、build05 manifest、约束、报告及 routed DCP 身份一致。补证脚本不修改约束或设计，不自动升级 R07 或板测状态。

## 10. 推荐下一步顺序

先补真实输入时序合同：确认所用雷达固件及 RAW16/FRCLK 组织、600 Mb/s 工作点、极性/位序、输出时序界限、抖动、板级偏斜和 idle-clock preamble。用这些实际依据填写输入约束并重做物理签核，不凭经验给出“看起来能过”的 input delay。

再实现并验证 SG 软件所有权：descriptor/缓冲区地址与长度、cache 一致性、DMA 状态和错误、有限传输、停止/复位/回收顺序，以及 GPIO ACK、PHY 终态、packetizer COMPLETE、SG 完成的不同含义。遵循现有唯一控制 GPIO、FIFO 和 SG DMA 主线，不增加第二套采集架构。

最后做有限 RAW 板测：先小包单会话，对照真实供电、时钟、串口配置、PHY基准、FIFO计数、SG实际长度与DDR数据，再测 STOP/ABORT/失钟/恢复。以实际 lane 原始样本和计数证明数据完整性；在这些证据成立前不把 RAW 运输包称为有效雷达帧，也不扩展到FFT/角度算法。
