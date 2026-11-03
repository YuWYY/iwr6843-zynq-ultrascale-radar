# 下一轮 PHY 控制域与 CDC 修复计划

> 历史设计输入。本文下述“尚未实施”和构建结论属于原交付基线。本轮已在同一主线实施 R01～R06，具体源码与验证边界见 [实施复核](../m2_closure/IMPLEMENTATION_REVIEW.md)、[当前工作状态](../m2_closure/WORK_STATE.md) 和最新交付的 requirement_matrix。保留本文用于追溯，不把历史结论当作当前源的验收结果。

日期：2026-10-13。状态：**计划，尚未实施**。本轮只新增本文；生产 RTL、XDC、testbench、BD 和 XPR 均未因本文修改。

当前交付是可审阅、可独立验证的工程，状态仍为 **`PHY_RULE_BLOCKED / CDC_SIGNOFF_BLOCKED / NOT_DOWNLOAD_READY`**。这里存在已确认的实现错误和未关闭的 CDC 风险，不能把它们全部归为“等板级物理参数”。下一轮先修实现，再分别完成源同步时序、板级电气和上板验证。

## 1. 当前基线与最终 route 事实

基线为 `outputs/m2_real_20261012_180512/`，最终报告是 Routed/Fully Routed 状态，生成时间为 2026-10-12 18:13。详细问题依据 `delivery/20261012_171114/phy_rule_review.md` 与 `cdc_review.md`；后者早期正文的综合计数及负 hold 余量不能替代下面的最终结果。

| 项目 | 最终原生证据 | 能证明的范围 |
|---|---|---|
| route | `reports/route_status.rpt`：16,466/16,466 routable nets fully routed，routing errors=0 | 此固定引脚与冻结网表已完成布线 |
| 已声明 STA | `reports/timing_route.rpt`：WNS=+0.804 ns，TNS=0；WHS=+0.011 ns，THS=0；WPWS=0，失败端点均为 0 | 工具声明的时序约束通过；不补全尚未提供的输入时序 |
| PHY 器件规则 | `reports/drc_route.rpt`：REQP-1742 仍有 3 条 Critical Warning | 三条 IDELAYE3.CLK 与对应 ISERDESE3.CLKDIV 不同网，错误仍在 |
| CDC | `reports/cdc_route.rpt`：CDC-7=11、CDC-10=14、CDC-12=2、CDC-13=1，共 28 Critical；CDC-15=761 Warning；CDC-3=19 Info | route 没有消除自定义 RTL 的复位、组合源和分频器 CLR 问题 |
| 其他 DRC | 2 条 AXI DMA BRAM collision advisory；1 条 RTSTAT-10，涉及 SmartConnect 内部 15 条无 routable load 网络；总计 6 条、0 Error | 保留原始告警；不因厂商 IP 提示而改受保护配置 |
| 时钟/外部约束 | `reports/constraints_route/check_timing.rpt`：no_clock=0、unconstrained_internal_endpoints=0；no_input_delay=6，3 个低速输出有 false-path | 综合时的 8 个未连接 PS EMIO 无钟脚已在实现优化后消失；高速输入合同仍缺失 |

四组 held-payload 在最终 `reports/constraints_route/` 中仍为 2 mailbox + 2 snapshot、585 个存活目的端：

| 通道 | 方向 | 目的端 | 8 ns max-delay 最差 slack | bus-skew actual / slack（ns） |
|---|---|---:|---:|---:|
| `u_cmd_mb` | SYS100 → RX75 | 141 | +6.987 | 1.335 / +6.665 |
| `u_phy/u_delay_mb` | SYS100 → REF300 | 28 | +7.319 | 0.766 / +7.234 |
| `u_snapshot` | RX75 → SYS100 | 384 | +6.971 | 1.283 / +6.717 |
| `u_tap_snapshot` | REF300 → SYS100 | 32 | +7.206 | 0.934 / +7.066 |

这些实际正余量支持当前四个协议的数据保持窗口，不能豁免其余 CDC。约束检查器的 PASS 是对象与 requirement 检查；当前脚本本身没有把负 slack 设为失败门槛，最终正余量来自以上报告的独立读取。下一轮应将最终 max-delay/bus-skew 负余量纳入硬门禁。

当前尚未解决的 PHY 实现包括：控制时钟错误；没有把每次 tap 更新限制在厂商公开的 1–8 tap 步进范围；没有依据每条 lane 的真实校准基准保护 TIME 模式的绝对 tap 目标。已修正的 `IDELAYCTRL.SIM_DEVICE="ULTRASCALE"` 不再算未解决项。

## 2. 下一轮边界与推荐结构

固定封装引脚、I/O 标准、BD 的 12 个 cell、唯一 GPIO、FIFO、DMA、SmartConnect、Clock Wizard、PS 及地址映射保持原设计边界。内部修改集中在 `radar_lvds_phy`、`radar_phy_delay`、控制/接收状态出口、reset/CDC 公共模块及其约束和测试。不能靠降低 DRC 等级、全域 asynchronous clock groups、全域 false-path、虚构 input_delay 或 CLOCK_DEDICATED_ROUTE 放宽来关闭问题。

推荐先评审并冻结以下分工：

| 域 | 单一职责 | 不得依赖 |
|---|---|---|
| SYS100 | GPIO 应答、命令仲裁、在途所有权、失钟/事务 watchdog、故障记录和取消 | watchdog 不得等待 RX 时钟才能执行 |
| REF300 | IDELAYCTRL.REFCLK、校准复位/RDY 监督、有限校准超时、明确 RECAL | 接受 ARM 所需的 REF 校准资格不得依赖 BITCLK |
| RX75 | IDELAYE3.CLK、EN_VTC/LOAD/CNTVALUEIN、周期计数、实际 tap 读回、步进、ISERDES.CLKDIV | 不得用 REF300 临时代替缺失的真实 RX 时钟 |
| 实际 BITCLK 全速域 | 分频器 CLR 的释放资格；时钟起停/相位处理 | 不得用其自身仍被 CLR 停止的 RX75 生成 CLR 释放，否则形成循环依赖 |

三个 IDELAYE3.CLK 必须和 ISERDESE3.CLKDIV 接同一 RX75 网络；IDELAYCTRL.REFCLK 继续 300 MHz。不能只换 IDELAYE3.CLK 连线，留下 REF 域 LOAD、CNTVALUEIN、VTC 和采样逻辑。

为继续保持两条命令 mailbox、两条外部 snapshot，首选重用通道，而非隐含增加未审查的 REF↔RX 总线：

1. SYS→RX 的 144 位 mailbox 仲裁 ARM、STOP 与内部 DELAY 执行请求；保留一次一个事务，定义独立内部命令编码，不占用外部保留位。
2. 原 SYS→REF 的 28 位 mailbox 负责 RECAL/校准监督请求；普通 DELAY 的实际执行迁往 RX。
3. 数据 snapshot 保持 RX→SYS；tap snapshot 改为 RX 的合法窗口 shadow→SYS，外部三 lane 的 27 位顺序不变。
4. RX 执行完成不能复用 mailbox 的“已收取”ACK 冒充完成。可沿用独立完成 toggle，但必须定义其复位、失败、取消和一次性归属；若增加结果/代次的多位跨域字段，须使用明确的持稳协议并计入 CDC 清单，不能逐位同步后拼接。

上述复用会改变 tap 快照在无 BITCLK 时的可用性，也会改变 ready 的解释边界。**实施前必须同步定稿 CONTROL_ABI 文档与软件预期**：GPIO 地址、opcode、页号和位布局不动并不自动意味着 ABI 行为兼容。若不能保持既有软件可依赖的语义，应显式版本化，不能仍以旧 ABI ID 宣称兼容。

## 3. 第一步：先冻结无时钟/命令完成合同

以下为推荐行为，不是当前 RTL 已具备的保证：

| 情况 | 推荐明确结果 |
|---|---|
| REF 校准成功、BITCLK 尚未出现 | SYS 能接受一次 ARM 并返回排队 ACK；不要求 RX ready、FIFO ready 或 tap snapshot 已完成 |
| ARM 等待时始终无 BITCLK | 当前 SYS 首数据 watchdog 仍有界运行；超时锁存失败，不制造数据/完成/新快照；允许按既定安全条件 RECOVER |
| 无 BITCLK 时 DELAY | 返回现有 NOT_READY，不能留下一个阻止后续 ARM 的无限 busy 事务 |
| 接受 DELAY 后失钟 | SYS watchdog 有界失败；锁存 PHY 未就绪并取消/隔离旧执行；busy 清除不等于成功 |
| RECAL 时无 BITCLK | REF 校准阶段有界结束，重建 ARM 资格；RX 基准取样尚未完成单独保持无效，不能让整条 RECAL 永久阻塞 ARM |
| 无 BITCLK 时 tap snapshot | 无新测量就不增加 generation、不返回伪零；旧样本保持旧代次/有效性边界；取消需要有明确恢复入口 |
| 失钟后重新出现 BITCLK | 旧 DELAY 不得迟到 LOAD，旧 completion 不得完成新命令；需要 RECAL 时继续保持失败锁存，不能自动恢复 |

须分离内部 `ref_cal_ready`、`rx_phy_ready` 和 `arm_admissible`。当前 flags5 是 PHY 稳定、flags6 是解串释放、flags7 是近期 RX 活动、flags19 是延时事务 busy；修复不得把四者继续混为一个 ready。保留旧 flags 的语义时，ARM 的准入应使用独立 REF 资格；会话故障逻辑不能在“ARM 已排队但 RX 尚未首次就绪”时因初始 `rx_phy_ready=0` 立即误判校准丢失。真实 RDY 丢失仍须及时取消。

在写 RTL 前给每条命令画出 accepted、delivered、executing、succeeded/failed/canceled 的所有权转换。WATCHDOG 在 SYS 域计数；使用可参数化、可审计的有限上界，不能按停止的 RX 时钟计时。工程默认超时应由最大 511 tap 跨度、每步周期数、校准阶段与跨域往返延迟推导并留余量，再换算为 SYS 周期；现有 REF 的 300000 周期校准 timeout 不等于新 RX 执行 timeout。

取消必须先使执行端保持不可运行，再释放源端所有权。可评估对该事务通道两端共同异步断言、分别同步释放的持久取消复位，并仅在明确恢复流程重用通道；不能只清 SYS busy 或翻转一次会丢失的取消脉冲。若用 epoch，须证明停止的 RX 恢复后先识别取消代次再允许 LOAD，且陈旧完成被丢弃。方案选择必须有对应测试，不能以“有 watchdog”代替取消正确性。

## 4. 第二步：实现 RX 域合法延迟执行器

先做独立执行器及官方 UNISIM 测试，再接入顶层。参考流程为：等待 REF 校准资格同步到 RX → 保持解串/数据路径未就绪 → EN_VTC 拉低 → 至少 10 个真实 RX 控制周期 → 读取各 lane 实际 CNTVALUEOUT → 校验目标 → 分步 LOAD/等待/实际读回 → 全 lane 到位 → 等待规定稳定期 → 恢复 VTC → 重新建立 RX 就绪 → 发一次终态通知。

- 初始校准和 RECAL 的首次观察只读，不 LOAD 零；BITCLK 尚未出现时保留“未取样”，不能以软件影子目标或复位零充当基准。
- 每 lane 的下一步由实际观察值计算：`next = current + clamp(target-current, -8, +8)`。做有符号扩展和 0…511 边界检查，避免 9 位减法回绕。三 lane 可不同方向；已到位 lane 不再重复加载。
- 当前一个共用 LOAD 必须改成可证明的 lane 掩码/独立 LOAD，或证明每次共用 LOAD 对已到位 lane 合法；优先明确每 lane 的寄存 LOAD 与 CNTVALUEIN。
- 使用厂商流程表固定 CNTVALUEIN 建立周期、LOAD 脉宽、相邻更新间隔、低 VTC 读回等待和恢复后稳定时间。所有数字标明 RX 控制周期；不得照搬原 REF300 的计数并仍声称相同物理等待时间。
- TIME 模式保存每 lane 的实际校准基准及有效代次。绝对 tap ABI 不变时，对可能低于合法对齐基准的目标拒绝应用；不能偷偷改成相对 offset 或 ps。基准如何受 VTC/温漂/RECAL 影响须由 UG571 规则与实测策略共同定稿，不能把单次模型观察值当终身安全下限。
- 读回 X、RDY 丢失、步进不收敛、过时事务、总时限到达均是显式失败；不能发送成功完成、不能使用目标回显替代实际读回。

此阶段的最小原生验收是三个 IDELAY CLK 与 ISERDES CLKDIV 同网且 REQP-1742=0。该规则满足后仍继续做后续 CDC 和板级门禁。

## 5. 第三步：收敛状态出口、复位与分频器 CDC

### 5.1 五条功能状态源先寄存

`complete`、`fault`、`abort`、`idle`、`ready` 都由所属源域寄存器输出，再进入目的端 3 级同步链。禁止同步器第一级前直接放状态译码、宽 OR 或多域 ready 组合。

- `complete` 由明确的完成/取消事件锁存；特别覆盖 ACTIVE=010 → FAULT=101 的转移，不能因中间译码毛刺被 SYS 误认为 COMPLETE=100。
- `fault` 与 `abort` 保持各自源域粘性和统一清零规则；RECOVER/复位清零也要经过定义的跨域生命周期。
- `idle` 将计数器阈值与 decoder enable 的组合判断寄存，定义一拍延迟及连续空闲资格；不能把任意窄脉冲视为可跨域状态。
- `ready` 分 REF 校准资格与 RX 数据路径资格；源域正规寄存、目的域同步，安全撤销由明确的故障/复位机制负责。不能仅将多个域的 busy 组合 OR 后宣称事务完成已同步。

3FF 不能保证滤掉被第一级采中的源端毛刺。修复目标是源结构和语义正确；理想 RTL 仿真看不到模拟布线毛刺，仍须复查综合 CDC-10 的实际端点。

### 5.2 复位分清异步断言与本地资格

对当前 22 条复位 Critical、176 条复位 count/CLR Warning 逐路径列清单：真实复位所有者、组合输入、目的域、停止时钟行为、断言最小宽度、释放条件和影响的 mailbox/snapshot 代次。

推荐将每域的原始异步断言集中进入认可的复位同步链；需要的 HOLD_CYCLES 计数器使用该域已经同步的复位/使能，不把原始多域组合条件直接接到所有 count_reg 的异步 CLR。`released` 仍必须能在目的时钟停止时进入安全态，其异步断言路径、同步释放路径分别审查。可在局部采用适用的 XPM_CDC_ASYNC_RST 或等效可识别结构；采用 XPM 本身不替代对组合 reset source 的证明。

REF ready、DMA reset、RECOVER 等不同时钟来源不应直接以组合 fan-in 重复扇入同一异步 release 网络。先区分必须无时钟立即断言的硬复位/安全条件，与可以在目的域同步后阻止启动的资格条件。若确有必须合并的异步源，须提供单调断言、最小脉宽和独立撤销顺序证明，不能批量 waiver。

### 5.3 BUFGCE_DIV CLR 单独原型验证

当前 SYS→`u_rx_div/CLR` 的 CDC-13 不属于四组 payload。推荐在实际、未受该分频器 CLR 停止的 BITCLK 全速时钟上建立明确的释放资格；不得在 RX75 上产生自身的 CLR 释放。先独立验证 BITCLK 初始静止、停高/停低、SYS reset 在任意相位撤销、重复复位以及首个完整 /4 周期，再集成。

不要直接把 `HARDSYNC_CLR=TRUE` 当作修复处方：UG974 2025.1 的 UltraScale/UltraScale+ BUFGCE_DIV 属性表未列该项，本机共用 UNISIM 模型含跨家族条件；“模型存在参数”不足以证明 XCZU2CG 支持该硬件模式。UG572 描述 CLR 的异步断言/同步释放功能，也不能替代对本设计外部复位来源和实际恢复/移除路径的验证。需以器件适用原语、原生网表和 timing/CDC 报告确认所选实现。[UG974 BUFGCE_DIV](https://docs.amd.com/r/2025.1-English/ug974-vivado-ultrascale-libraries/BUFGCE_DIV)，[UG572 BUFGCE_DIV](https://docs.amd.com/r/en-US/ug572-ultrascale-clocking/BUFGCE_DIV)。

## 6. 第四步：约束与实际网表一起更新

仅当 RTL 结构冻结后同步更新精确层级约束和 checker：

1. 导出所有跨域路径清单，包含通道重用后的方向、宽度、完成/失败/取消、ready 和 reset。四个逻辑通道如果改名/变宽，应更新明确父对象断言；不能保留旧 585 端点计数当新设计 PASS，也不能增加未计数的隐含 bundled bus。
2. 对每个持稳协议重新计算最早捕获时刻：当前 3FF 加观察旧末级后捕获使窗口为约 `delta + 3*Tdst`；新流水线/完成握手的实际代码必须重新推导。8 ns 可继续保守使用的前提是新的全部路径与协议均满足，不能直接沿用旧结论。
3. 维持纯声明 XDC；计数、非零断言、频率/波形/MASTER_CLOCK 检查在完整 Tcl checker 中。CDC XDC 继续只在综合后解析，精确覆盖真实存活/复制寄存器；避免全域 false-path 覆盖 payload 的 max-delay。
4. 最终 route 对每组目的 D 检查有效 requirement、最差 max-delay slack、实际 bus-skew slack，负值硬失败；检查例外 coverage/ignored、同步器 ASYNC_REG 结构和物理放置。对 reset 的异步断言例外不能同时隐藏需要保留的本地同步释放恢复/移除检查。
5. 默认目标：自定义功能 CDC-10 与 PHY REQP-1742 归零，其他自定义 Critical 逐条关闭或形成对象级、可重复证据。没有证据的条目维持 BLOCKED，不按总数下降宣布签核。

## 7. ARM-before-BITCLK 与失钟验证矩阵

每项以 SYS 周期设置有限总测试 deadline，并对完成次数、busy、generation、LOAD 次数和实际 FIFO AXIS 接收逐项断言。必须使用真实 BUFG/IDELAY/ISERDES UNISIM 与现有官方 AXIS FIFO；RTL 单元测试和最终网表审查分别保留。

| 场景 | 必须观察到的结果 |
|---|---|
| 上电后 BITCLK 永远不来，REF 正常 | REF 校准监督不死锁；ARM 返回一次排队 ACK；无数据/伪完成，首数据超时有界故障；RECOVER 可响应 |
| BITCLK 缺失时先 DELAY，再 ARM | DELAY 有界 NOT_READY、不占永久 busy；ARM 仍能排队 |
| 先 ARM，延迟启动 BITCLK | 新 RX 时钟先释放控制域、取得基准/准备 PHY、释放 FIFO并等待官方内部 ready；ARM 仅激活一次；之后真实 idle gap 才开始接纳 |
| BITCLK 一出现立即发有效 burst，无 preamble | 不声称无损获取首 burst；预备期数据不得误入有效会话；报告/合同明确需要 idle-clock preamble |
| RECAL 完成 REF 阶段时无 BITCLK | 不为等待 RX 取样而永远占用延时 busy、阻断 ARM；tap generation 不伪更新 |
| DELAY 已 ACK，执行前/低 VTC/LOAD/步进/读回/恢复 VTC 各阶段停钟 | SYS watchdog 有界失败；LOAD 被隔离，失败锁存；恢复时钟后无迟到写入、无旧完成污染新事务 |
| 停钟期间 ABORT、SAFE、RECOVER，或 DMA reset | 安全状态可到达；旧 ARM/DELAY/snapshot 取消；时钟恢复不会自动重放旧命令 |
| SYS/REF/RX 任一复位与握手完成同周期 | 不漏掉失败，不双重完成，busy/req/ack 不因 toggle 基线不一致永久锁住 |
| BITCLK 停高/停低、恢复不同相位与短暂片段 | 首个分频周期、reset release、失钟判定无假活跃/假首拍；不足以完成一次 PHY 准备时不得启动捕获 |
| 三 lane 等于目标、±1/±8/±9、0/511 边界、大跨度且方向相反 | 每次实际 LOAD 变化 <=8；等待周期正确；不溢出；全部实际读回一致后才成功 |
| 实际基准高于软件目标，读回 X，RDY 中途掉落 | 拒绝/失败可见；不能把软件目标或旧 snapshot 当作本次成功 |
| COMPLETE/FAULT 转移，fault 清零，idle 阈值进出 | 源端状态寄存结构成立，SYS 时间戳不因组合译码误触发；数字测试之外复查网表 CDC |

现有 14 项功能/原语/FIFO 回归必须全部对新源重新运行，并保持输入 SHA-256 与日志绑定。扩充的每个新 testbench 也必须加入 `simulation_gate.py` 的 required 清单，不能只留下不参与 gate 的 PASS 日志。重新综合、实现、结构校验、原生重开均须使用该同一冻结输入集。

## 8. 分阶段交付与解除阻断条件

顺序建议为：命令与状态合同定稿 → 单域寄存状态和复位/分频器小原型 → RX 延迟执行器与步进测试 → 两通道重用及取消协议 → 全量真实 FIFO 回归 → 全局 synth/route/CDC/DRC → 重新打包/独立解压验证。每步保留可回退的工程外备份和独立输出目录；不得在本次冻结交付中暗改生产源并沿用旧 PASS。

`PHY_RULE_BLOCKED` 解除需要 REQP-1742=0、控制域一致、合法 tap 步进/基准保护和真实原语验证全部通过。`CDC_SIGNOFF_BLOCKED` 解除需要每条自定义问题关闭、实际 payload 时序/偏斜通过、复位/失钟/取消有证据。两项解除后仍须完成：实际板版本和电压/终端确认、传感器固件/cfg 与 RAW16 合同、双边沿源时序/板级偏斜输入、DMA/DDR 完整系统与板上测试；只有这些也满足才讨论可下载交付。

## 9. 本次交付包复现边界

包布局为 `project/` 与 `evidence/`。`simulation_gate.py --check` 可从兄弟 `evidence/delivery/SIM_PASS.json` 及 `evidence/outputs/...` 读取原始证据，仍逐一对照解压 `project/rtl`、TB/模型等输入哈希；它只校验已完成仿真证据，不等于重新运行仿真。`check_structure.py` 的基线 fallback 指向 `evidence/delivery/before/radar_system.bd`，但执行会在解压 `project/delivery/20261012_171114/` 写入连接/参数/状态报告，不能称为文件系统只读检查。

`package_review.py verify --zip <确切ZIP路径>` 可重复检查既有 ZIP 的成员 SHA-256、CRC 与引用审计。若从解压目录运行必须显式传 `--zip`；默认 ZIP 路径属于原工程的 delivery 布局，不存在于该副本。`--extract <绝对新目录>` 要求目标从未存在，重复验证应另用新的准确目录；构建同名 ZIP 和复用已存在解压目录都按设计拒绝覆盖，不执行清理。

`reopen_package.tcl` 在独立 Vivado batch 中只打开经过路径校验的临时解压 XPR，日志目录在副本之外；每轮使用新的日志目录，保留完整原生 Validate 输出。Vivado 2025.1 在 `open_project -read_only` 下以 Project 1-228 拒绝 `validate_bd_design`，因此核验流程改为正常打开受隔离的副本，不保存、不生成目标、不运行 synth/route；原始失败日志保留。不能把这一 API 限制误报为原工程 BD 验证失败，也不能把正常打开副本称为项目只读模式。

重开验证证明副本的活动文件、原生 BD/受保护 IP/结构仍可解析且没有原工程或 Downloads 活动依赖；不会因重开成功解除上述 PHY/CDC 阻断。包内 `active_references.tsv` 保留导出时原始路径以提供审计溯源，是否存在活动原工程引用必须以解压原生重开的 `get_files -all` 检查为准，不能对所有证据文本简单搜索路径后混为活动依赖。

本文的具体实现建议是基于上述冻结 RTL、最终原生报告和厂商规则作出的设计推导，尚无“下一轮修复已通过”的证据。厂商基础依据继续采用 `phy_rule_review.md` 所列 UG571 的 TIME/VAR_LOAD、CNTVALUEOUT、步进与 Align_Delay 条款，以及 [UG974 2025.1 IDELAYE3](https://docs.amd.com/r/2025.1-English/ug974-vivado-ultrascale-libraries/IDELAYE3)。
