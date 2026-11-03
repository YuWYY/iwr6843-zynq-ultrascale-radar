# F20 × IWR6843：M2当前工程复审与Codex完善要求

**版本：M2-Closure-v9.0｜审查日期：2026-10-13**  
**唯一输入：`Radar_XCZU2CG_M2(3).zip`**  
**本轮性质：当前工程审查＋后续实施要求；不是已经替用户修改了RTL。**

> **结论：当前工程已完成源文件整理、BD连线和原生综合布线，可接受为“集成与排障阶段交付”；不能签收为可上板正式版本。下一轮不重建工程、不重新配置DDR/DMA/FIFO，而是修复PHY控制域和CDC实现，补齐源同步时序，再生成受控的下载候选。**
>
> 请Codex直接实施本文件中的R01～R07并交回修正工程，不要再仅交一份“下一轮修复计划”。工程已有的`docs/m2_real/NEXT_PHY_CDC_PLAN.md`是有价值的设计输入，但其首行明确是“计划，尚未实施”，不能作为修复完成证据。

---

## 0. 基线、已完成事项与验收范围

### 0.1 唯一修改对象

本机工程（路径变化时只调整根目录，不另建工程）：

```text
/fpga/Radar_XCZU2CG_M2.xpr
```

- 器件：`xczu2cg-sfvc784-1-e`。
- 工具：Vivado/Vitis 2025.1；不升级工具/IP版本。
- 主BD：`radar_system`。
- 唯一生产接收实例：`radar_capture_bd_0`。
- 唯一物理Top：`radar_m2_top`。
- 当前仿真Top：`tb_capture_fifo`；其余TB由明确的仿真入口逐个运行。
- 唯一生产RTL目录：工程根目录`rtl/`，不得从历史解压包重复加入同名源。

输入ZIP SHA-256：

```text
b62a2ebd512bad166eca86b0b4fb7cb3ddbd0933f4e9ae9ce17566d9883306fa
```

本次展开核对：3343个ZIP条目、2643个文件、286,559,157字节，CRC无错误。ZIP含历史日志和一个嵌套审核包；不能把嵌套审核包的哈希/布局当作本次最外层ZIP。

### 0.2 审查证据层级

本次已直接读取：XPR、主BD、官方组件XCI、13个生产RTL、10个TB与仿真启动/审核脚本、用户XDC、最终route报告、交付记录及下一轮草案。重新用Python检查了保存文件、连接和哈希，**没有在本环境重新运行Vivado、XSim或开发板**。

已复核：

- 13个生产RTL共1077行；相对M2(2)，4个文件修改、9个未变。
- 主BD为12个组件、37条普通网络、9条接口网络（8条AXI/AXIS＋1条差分参考时钟），16个标量外部端口。
- 14条要求的内部普通连接与新增AXIS接口均真实保存。
- XPR活跃文件引用全部可解析；设计Top、仿真Top、旧源退出已完成。
- 11份既有官方IP/常量的组件参数值与M2(2)一致；不把生成元数据变化当作器件配置改变。
- `simulation_gate.py --check`通过：14次已保存XSim记录对应当前源。这是**记录身份重查**，不是重新做了14次RTL仿真。
- synth与route输入清单各153项均匹配当前文件，且两份输入文件哈希映射一致。
- 当前主工程展开文件中没有`.bit`或`.xsa`；本次停止在`ROUTED_PROVISIONAL`，并未冒充正式下载产物。

本报告配套`evidence/independent_saved_file_audit.json`、`connection_check.json`、`rechecked_record_identity.log`与`source_excerpts.txt`保留了可复核依据。

### 0.3 当前状态与准确结论

| 层次 | 当前结果 | 是否还需要重做 |
|---|---|---|
| 工程复制/源文件导入 | 完成；13个新RTL唯一活跃 | 不重做 |
| BD连线/旧M1引用清理 | 完成；唯一接收实例、唯一GPIO | 不重做 |
| IP与地址配置 | 保护项无误改 | 不重配 |
| 原生Validate与生成wrapper | 已完成，有日志 | RTL变更后正常重新验证 |
| 功能/官方原语/FIFO仿真 | 10个TB、14次运行PASS | 修复后跑同一套＋针对性新用例 |
| 原生综合/布局布线执行 | 完成 | 修复后完整重新执行 |
| PHY器件规则 | **3条REQP-1742 Critical未关闭** | 必修 |
| 自定义RTL CDC签核 | **28条Critical未关闭** | 必修/逐对象证明 |
| 高速源同步输入时序 | 真实预算缺失，模板未加载 | 上板候选前补齐 |
| 实际雷达固件/RAW16匹配 | 未冻结 | 首次有效采集前确认 |
| 新SG软件及雷达实机 | 没有本版通过证据 | 后续同主线完成 |
| FFT、角度处理、DP图像 | 不是本批实现 | 取得可信原始数据后推进 |

历史M1的DDR与104包Simple DMA实测仍有效，但不是本版LVDS/异步FIFO/SG的实测结果。不得回退到M1测试来替代本版签核。

---

## 1. 最新原生实现结果：保留真实成绩，也保留阻断项

唯一最新route依据目录：

```text
outputs/m2_real_20261012_180512/reports/
```

| 项目 | 原生最终结果 |
|---|---:|
| 实际Top | `radar_m2_top` |
| 需要布线/已全部布线网络 | 16,466 / 16,466 |
| 路由错误 | 0 |
| WNS / WHS | +0.804 ns / +0.011 ns |
| WPWS | 0.000 ns |
| 建立/保持失败端点 | 0 / 0 |
| 无时钟寄存器/未约束内部端点 | 0 / 0 |
| 没有输入延时的输入端口 | 6，其中3个是高速数据/FRCLK，3个是异步低速输入 |
| LUT | 6968 / 47232，14.75% |
| FF | 11130 / 94464，11.78% |
| BRAM Tile | 18 / 150，12.00% |
| DSP | 0 / 240 |
| Bonded IOB | 16 |

这些是**当前已声明约束下**的时序及资源结果；WNS为正不等于未声明的高速输入采样窗口满足要求。

最终CDC统计：

| ID | 级别 | 数量 | 本轮解释 |
|---|---|---:|---|
| CDC-7 | Critical | 11 | 复位跨域结构待收敛 |
| CDC-10 | Critical | 14 | 5条功能状态及9条复位相关组合源路径；不是14种独立故障 |
| CDC-12 | Critical | 2 | 多时钟来源合入同步/复位条件 |
| CDC-13 | Critical | 1 | SYS复位到BUFGCE_DIV.CLR |
| CDC-15 | Warning | 761 | 585条持稳payload路径＋176条复位计数器CLR相关路径，需要分类，不批量豁免 |
| CDC-3 | Info | 19 | 已识别单bit同步结构，不能因此推导其他路径安全 |

四组持稳数据当前确有正余量：SYS→RX命令141端点、SYS→REF延时28端点、RX→SYS快照384端点、REF→SYS tap快照32端点，共585端点；max-delay最差余量分别6.987/7.319/6.971/7.206 ns，bus-skew余量分别6.665/7.234/6.717/7.066 ns。

**这四组正余量不是其余复位/组合状态CDC的豁免理由。修复域划分后端点数量可能改变，不能继续硬套585。**

其他原生DRC：DMA内部REQP-1934/1935各1条，SmartConnect RTSTAT-10涉及15条内部无路由负载网络。继续按具体对象记录；不因这些建议性告警去改官方FIFO/DMA实现。本次主要物理阻断是REQP-1742。

---

## 2. 已完成的合理修正：必须继承，不回滚

相对上版，当前4处代码改进已有对应仿真证据：

1. `radar_lvds_phy`加入SYS侧在途所有权，延时命令保持busy直到执行完成，不再把mailbox“已送达”当作执行完成。
2. `radar_control`将首个接收数据也作为RX活动依据，避免首个heartbeat之前停钟无法被检测。
3. `radar_receive`等待当前官方异步FIFO真实`TREADY`后才结束ARM等待，覆盖外部reset已释放但FIFO内部仍忙的情况。
4. `radar_phy_delay`/PHY在EN_VTC低且稳定窗口观察真实CNTVALUEOUT；启动/RECAL只读观察不再错误LOAD零。`IDELAYCTRL.SIM_DEVICE="ULTRASCALE"`也已显式匹配该原语模型，不是当前待修问题。

这些不是用户操作错误；仍待处理的PHY和CDC问题源于此前交付RTL的实现边界。Codex应在当前已修版本上继续完善，不以“原始AI包代码必须保持不变”为由拒绝必要修改。

---

## 3. 保护范围：保持一个正式工程、一个真实数据主线

```text
真实IWR6843 LVDS
    → IBUFDS / IDELAYE3 / ISERDESE3
    → RAW16字对齐/错误检测
    → 64位AXIS打包
    → 现有异步AXIS FIFO
    → 现有SG S2MM DMA
    → PS HP0 → DDR

PS UART1 EMIO＋唯一GPIO → 控制、配置、状态、恢复
PS UART0 → 电脑日志
```

保持：

- PS DDR 2GiB/64bit、单rank、既有时序；UART0、DP板级路由、HPM0/HP0、PS PL0基础配置不变。
- GPIO `0xA0010000/64KiB`，双32位，唯一控制/状态入口；DMA寄存器`0xA0000000/64KiB`。
- SG S2MM-only、64bit stream/MM、Burst64、len23、addr32，MM2S/DRE/Micro/Status Stream不擅自开启。
- FIFO async、8192×64、TKEEP/TLAST、CDC3、写计数32bit、水位7168；复位仍只有实际存在的`s_axis_aresetn`。
- 控制SmartConnect 1SI/2MI；数据SmartConnect 2SI/1MI；payload及descriptor均映射低2GiB DDR。
- SYS100、REF300、BITCLK300及RX75的角色不混淆；系统控制面不依赖雷达先输出时钟。
- 固定封装引脚及对应板级电压体系，不静默换球位、降速或绕开专用时钟资源。

允许：内部模块拆分、适量增减RTL文件、替换为合适的官方XPM同步/复位组件、必要的内部握手/快照扩展、XDC精确更新、测试与脚本修正。

**“一条主线”指一个生产数据源/链路，不是禁止控制面存在多个时钟域、CDC握手、仿真TB或诊断计数器。无需为保持13个文件而把新状态机挤在原文件。**

禁止：恢复M1合成源、添加真实/合成MUX、第二GPIO、Simple DMA备用分支、重建全部BD、回滚当前已验证修正；不自动下载、复位板卡或运行DDR破坏性扫描。

---

## 4. R01｜P0：修正PHY控制域，关闭REQP-1742

### 已确认事实

`rtl/phy/radar_lvds_phy.v:90–118`：三条IDELAYE3的`.CLK(ref_clk)`，而对应ISERDESE3的`.CLKDIV(rx_clk)`。最终`drc_route.rpt`对三条路径全部报告REQP-1742。

这是实际实现规则不满足，不是仅缺固件或PCB参数。当前仿真能跑、route能结束，均不替代器件规则。

### 必须采用的职责边界

| 域 | 合理职责 |
|---|---|
| SYS约100MHz | 外部命令应答、事务所有权、取消、超时、错误记录 |
| REF300MHz | IDELAYCTRL参考时钟、校准复位和资格监督 |
| RX75MHz | IDELAYE3控制时钟、LOAD/CNTVALUEIN/EN_VTC更新、tap合法读回、ISERDESE3的CLKDIV |
| 真实BITCLK全速域 | 必要的分频器CLR释放资格，不能依赖被CLR停住的RX75 |

三个IDELAYE3.CLK必须与对应ISERDESE3.CLKDIV使用同一RX网络；IDELAYCTRL.REFCLK保留300MHz。[S1,S2]

**不能只将`.CLK(ref_clk)`改成`.CLK(rx_clk)`，却继续由REF状态机异步驱动LOAD、CNTVALUEIN和EN_VTC。整个延时执行事务及读回都要迁入正确域。**

### 执行要求

- 优先复用既有命令mailbox/快照；现有NEXT_PHY_CDC_PLAN提出的SYS→RX执行DELAY、SYS→REF管理RECAL是可行候选。
- 在实施前用一张时钟域/通道表固定每条跨域字段的来源、目的、宽度、数据持稳窗口、ACK和终态含义。
- 允许有必要的内部新增通道，但每条都必须有完整CDC约束和测试，不能偷偷引入未约束总线。
- 分开`ref_cal_ready`、`rx_phy_ready`、`arm_admissible`；不能把“RX无时钟”误判成“连UART配置都不允许”。
- 同步更新tap快照所在时钟域及实际端口/约束检查器。

### 完成条件

- 最终网表中三组CLK/CLKDIV确为同网，REQP-1742=0。
- 保持专用原语和固定物理球位；不得使用降低规则严重性或`CLOCK_DEDICATED_ROUTE=FALSE`绕过。
- 官方UNISIM功能测试通过，同时保留独立的原生DRC证据。UNISIM不检查到某条规则不等于规则已满足。

---

## 5. R02｜P0：延时更新不能任意跳变，加入合法步进与基准保护

### 已确认事实

- `radar_phy_delay.v:66–81`直接把27bit目标送到三个lane并发一次共同LOAD。
- `radar_control.v:197–205`只检查每个软件目标不超过511，未限制单次实际变化，也未保护TIME校准基准。

UG571对相应VAR_LOAD操作给出了每次1～8 tap变化及TIME模式的校准对齐要求；TIME初值0ps不等于实际CNTVALUEOUT=0。[S1]

### 必须实现

- 启动/RECAL后，首次合法窗口读取并保存每个lane的实际基准及有效代次，禁止以影子配置值或复位零代替。
- 在RX域中按实际读回向目标逐步推进，例如：

```text
signed_delta = target - current
step = clamp(signed_delta, -8, +8)
next = current + step
```

- 已到目标的lane不重复无意义LOAD；不同lane允许不同方向，需独立使能或严格证明共同LOAD合法。
- 算术扩展为有符号并检查0～511边界，不能9bit减法回绕。
- 将EN_VTC前后等待、CNTVALUEIN建立、LOAD脉宽、相邻更新和读回等待明确为**RX控制时钟周期**；按适用UG571流程落实，不沿用REF300的计数并冒充原有时间。
- 不把文档典型Align_Delay取值硬编码成所有器件、所有温度下的固定安全下限。记录实际基准何时有效、RECAL何时失效；若软件采用绝对tap，低于合法基准的目标必须拒绝。
- 真实读回达到目标且稳定后才报成功；X/无效读回、RDY丢失、不收敛和超时均有可见失败。
- 外部tap单位原先是原始tap；不能静默改成ps或相对offset。确需更换语义则更新ABI版本及文档。

### 必测边界

等于当前值、±1、±8、±9、大跨度、三lane方向相反、0/511、目标低于实际合法基准、RDY中途掉落、读回无效、反复RECAL。对**每次实际LOAD**断言变化上限和周期关系，不只检查最后结果相等。

---

## 6. R03｜P0：无BITCLK、失钟、取消和恢复必须有界且不会重放旧事务

这是R01迁域后必须同步解决的问题，不可只把延时逻辑移到RX，然后让其永久等待。

| 场景 | 要求行为 |
|---|---|
| REF正常、BITCLK未出现 | 允许软件配置/查询控制面；ARM按合同排队一次，不能要求RX先ACK才允许雷达启动 |
| BITCLK一直不出现 | SYS域首数据watchdog到期，当前会话显式无效，不假造数据/完成 |
| 无BITCLK时请求DELAY | 有界返回NOT_READY或明确拒绝，不留下永久busy |
| DELAY已受理后任一执行阶段失钟 | SYS域watchdog结束所有权；隔离旧LOAD/完成，不能当成功 |
| RECAL时无BITCLK | REF校准能有界完成；RX基准暂未取样单独标无效，不永久阻挡下一次ARM |
| 无BITCLK时请求tap snapshot | 无新测量不增加generation；旧样本不伪装成新样本；忙状态有恢复入口 |
| 停钟期间SAFE/ABORT/RECOVER/DMA reset | 控制面可响应；恢复时旧命令不能迟到执行 |
| 时钟恢复 | 重新满足PHY与FIFO复位/ready条件；失效会话不自动继续 |

必须画出并实现：`accepted → delivered → executing → succeeded / failed / canceled`。mailbox的交付ACK不是最终成功。

watchdog在SYS域计时，不能在可能停止的RX域计时。默认超时由最大步数、每步周期数、校准及CDC往返上界推导并留余量。

取消可采用两端一致的持久复位/禁用或显式epoch机制，但必须证明：

1. 源端busy解除前，旧执行已被隔离；
2. 停止的RX时钟重新出现后不会补打一记旧LOAD；
3. 旧完成toggle不能完成下一笔新命令；
4. req/ack/reset基准不会不一致而锁死；
5. 无法恢复时明确要求软件执行RECAL/重新初始化，不自动无限循环。

不要为一条真实输入主线而省略必需的CDC所有权。协议有变化就更新CONTROL_ABI；保持地址和位号不自动代表行为兼容。

---

## 7. R04｜P0：把组合状态跨域改成可证明的源端寄存结构

### 直接证据

- `radar_receive.v:37`：`complete=(state==3'd4)||canceled`。
- `radar_receive.v:38`：`fault=(faults!=0)`。
- `radar_receive.v:39`：`idle_observed=idle_gap&&decoder_enable`。
- `radar_control.v:48`：`abort_level=(sys_faults!=0)`。
- `radar_phy_delay.v:27`：组合`ready`。
- 顶层将这些信号送入目标域同步器；最终CDC-10仍记录组合源。

同步器不能保证滤掉已经被第一级采中的组合毛刺，官方CDC文档明确提示此风险。[S3] 例如状态二进制从ACTIVE=010到FAULT=101，物理位延迟不同可能出现瞬时100译码；这只是风险机理解释，不是声称仿真已经观察到该模拟毛刺。

### 修改要求

- 完成、故障、取消、空闲资格、校准资格各自在**所属源域**先形成寄存/粘性状态，再进入目的域同步链。
- 明确每个状态置位、清零和RECOVER规则；一次性事件使用能保持到接收的机制，不能缩成可能丢失的脉冲。
- 源端寄存新增一拍后，调整接收结束、时间戳及先后判断，不能通过放宽检查容忍假COMPLETE。
- READY撤销分成资格信号与必须立即安全断言的复位/故障路径，不再用多域组合OR代替完整设计。
- 理想RTL仿真通常不产生真实路由毛刺，因此必须同时看最终综合/route CDC路径与源寄存器结构。

### 完成条件

自定义功能状态上的CDC-10关闭。任何残留条目给出精确对象、时钟、协议及豁免理由；不以总数下降判定完成，不批量waive所有CDC-10。

---

## 8. R05｜P0：复位释放、跨域复位来源与分频器CLR

### 当前问题位置

`radar_reset_release.v:11–18`将原始`allow_n`同时送给同步器、16bit计数器和`released`的异步复位。目的域的等待计数并不能自动消除这些物理异步CLR路径的释放问题。

`radar_capture_bd.v:58`的公共复位组合了系统复位、RECOVER和DMA输出；PHY还有独立SYS/REF组合复位。最终报告中有22条复位相关Critical（11+9+2）及176条相关CDC-15路径。

`radar_lvds_phy.v:78–80`直接用SYS复位控制`BUFGCE_DIV.CLR`，有单独CDC-13。

### 修改要求

- 区分“必须在目的时钟停住时也立刻进入安全态”的异步断言，与“恢复运行前同步确认”的释放资格。
- 原始异步断言集中进入可识别复位同步结构；HOLD计数和普通状态使用本域已同步的复位/资格，不再将多域组合条件广泛扇入每个计数器的CLR。
- `released`/实际输出安全态仍须支持无RX时钟的断言；不能为了消除报告把安全断言变成必须等RX边沿。
- 对必须合并的异步来源列明单调性、最小脉宽、撤销顺序、reset所有者及是否取消mailbox/snapshot，不用笼统“这是reset”作为豁免。
- 可以使用器件适用的XPM CDC/复位资源，但正确实例化不自动代替整个复位树的分析。
- 分频器CLR释放资格不能由其自身被停止的RX75生成，否则会形成释放死锁。采用适用于XCZU2CG的原语/源时钟资格结构并检查原生结果。[S4,S5]
- 不凭共用UNISIM模型中的跨器件参数，擅自增加UG974 2025.1未对本器件公开的`HARDSYNC_CLR`属性。
- 官方异步FIFO仍只连接现有一个reset输入；复位保持、真实两侧时钟及内部reset-busy均要满足，ARM需继续等待真实TREADY。[S6]

### 测试与签核

BITCLK初始为0/1、无时钟、停高/停低、任意SYS释放相位、任意执行阶段停钟/恢复、RECAL/DMA reset/RECOVER与完成同周期、FIFO释放前后均覆盖。每项有SYS周期总deadline，断言无假数据、无旧包延续、无自锁。

自定义Critical应修复；保留必要复位同步器条目时必须有精确对象级证据，不能以添加false_path替代本域释放的恢复/移除检查。

---

## 9. R06｜P1：约束检查与构建身份要形成真正的自动验收

### 9.1 当前不是已有负余量，而是脚本会漏判未来负余量

`check_constraints.tcl:138–149`检查路径存在、REQUIREMENT=8ns并记录最差SLACK，但没有因负SLACK返回失败；bus skew主要生成报告，未设置负余量失败条件。

当前4组路径的实际余量均为正，本次没有据此否定它们；要求是防止下一版“对象存在即PASS”。

修改后：

- 对每条存活held-payload目的端核对路径实际受约束且未被更高优先级false_path覆盖。
- max-delay和bus-skew负余量必须退出非零；没有路径、空对象、少端点、NaN/不可解析值也失败。
- 目的端数量和域关系按新实现重新列出，不硬套2mailbox/2snapshot/585端点。
- 三个IDELAY.CLK和相应CLKDIV同网、源寄存状态、复位/CLR类型都加入对象级检查。
- 最终routed WNS/WHS/WPWS、路由、无钟、未约束内部端点与未解决Critical参与release门禁。

### 9.2 synth与route之间缺少强制身份联结

`build_export.tcl:41–46`在route阶段只检查`synth_1`完成状态，重新记录当前输入后启动impl。当前synth/route两份153项输入完全一致，**没有发现本次混用**；但脚本不能防止后续“改了源码又只跑route”的旧DCP风险。

必须补：

- synth结束记录输入清单、实际综合DCP哈希、目标器件、Top及本次build_id。
- route只接受对应输入清单与DCP身份均匹配的synth。源、BD、XCI或约束变化则强制重新综合，不能以`STATUS=Complete`代替。
- 记录实际使用的DCP而非仅发布副本，并在实现产物清单中串联来源。
- `build_manifest.py:9–14`目前硬编码三份用户XDC；增加实际源时序XDC后必须进入身份清单。建议按真实active fileset生成，连同影响构建的脚本、参数和设置归档。
- 负向测试：输入改变、仿真记录过期、LOG被改、final XDC新增但未登记、旧DCP与新源码不符、route失败后旧PASS残留，全部能被门禁拒绝。

### 9.3 状态名不混淆

保留分层字段，避免仅用`PASS route`让人误以为能上板：

```text
SIMULATION = PASS / FAIL / NOT_RUN
NATIVE_ROUTE_EXECUTION = PASS / FAIL
PHY_RULE = PASS / BLOCKED
CDC_SIGNOFF = PASS / BLOCKED
SOURCE_TIMING = VERIFIED / PENDING
BOARD_ELECTRICAL = VERIFIED / PENDING
DOWNLOAD_CANDIDATE = YES / NO
BOARD_TEST = NOT_RUN / PASS / FAIL
```

下载候选的条件不包含“已经下载实测成功”，否则形成逻辑循环；板测是在候选完成后独立执行的下一层验收。

---

## 10. R07｜P1：补高速输入时序和真实数据配置，不能伪造默认值

### 10.1 源同步时序

当前`constraints/radar_source_timing_template.xdc`没有有效预算且未加入constrs_1；最终no_input_delay列出：

```text
radar_data0_p / radar_data1_p / radar_frclk_p  高速接口缺口
radar_3v3_present / radar_nerr_n / radar_uart_rx  异步低速入口
```

6个计数不能都用0ns机械清零。低速入口要按真实异步信号处理；三条高速信号必须相对BITCLK建立实际双边沿min/max窗口。

完善`docs/m2_real/source_timing_budget.md`：写清TI实际输出速率/边沿关系、数据手册版本及字段、连接器/转接板/走线的相对偏斜、抖动及预算分配。保留来源，不把板级未知量填成0；若采用保守上界，应说明保证依据和覆盖范围。

如果同一组min/max覆盖三lane，证明覆盖最差lane；否则分别约束。生成clock、CLK_B反相和DDR两边沿不能重复或漏约束。确认预算在工具端点处的定义，避免同时计入板延时两次。

实际时序输入生效后重新跑route STA和数据手册报告/输入路径报告，再接受源同步时序结果。不要改成COUNT模式或静默关闭动态延时来回避原问题。

### 10.2 板级电气与数据合同

沿用固定球位，首次候选前确认实际板卡/转接版本：

| 信号 | 球位 |
|---|---|
| PL100MHz P/N | AE5 / AF5 |
| BITCLK P/N | L7 / L6 |
| FRCLK P/N | L3 / L2 |
| DATA0 P/N | K8 / K7 |
| DATA1 P/N | J6 / H6 |
| UART TX/RX | AG10 / AH10 |
| NERR/3V3存在 | AE12 / AF12 |
| SYNC/NRST | AE10 / AF10 |

现有约束为Bank64参考时钟DIFF_SSTL12、内部终端关闭（匹配已提供交流耦合/偏置/外部终端图）；Bank65雷达LVDS及内部终端；Bank44控制LVCMOS33。它们的**声明正确不能代替实际VCCO/跳线与连接确认**。NRST仍只拉低/释放，UART未允许时高阻，PGOOD只代表3.3V存在，不能证明固件ready。

记录实际IWR6843ISK修订、bin名称/哈希、cfg和CLI版本；RAW16假设的双lane600Mbps、16bit传输字、FR高8低8、空闲模式和启动前导要与实际固件波形一致。当前decoder不是TI HSI/chirp解析器，TLAST仍是约定的raw transport chunk，不自动代表一帧雷达图像。

这些外部资料不齐时：Codex仍应完成R01～R06所有确定的实现和本机验证，交回`INTERNAL_GATES_FIXED / SOURCE_TIMING_PENDING / NOT_DOWNLOAD_READY`等准确状态；只列真实缺失项，不再把RTL错误也推给用户资料缺失。

---

## 11. 固定实施顺序与有限测试，不无限扩大范围

### 第一批：修内部实现，不等待外部板测

1. 读取当前所有权/复位/延时逻辑和本文件，保存工程外版本快照。
2. 一次定稿R01/R02/R03的域划分、命令完成/取消及ABI语义；不反复调整GPIO或DMA架构。
3. 小模块实现：RX延时执行器、状态源寄存、复位/分频器资格；保持当前已有修正。
4. 接回唯一`radar_capture_bd_0`，同步更新CDC/XDC检查和测试。
5. 新源执行原14次回归＋针对性用例；保持真实UNISIM及当前官方异步FIFO，不合成测试激励到生产Top。
6. 同一冻结输入集完整synth/route，原生审核PHY/CDC及已加载约束；若失败直接修受影响模块，不重配全部IP。

### 必需的针对性测试组

| 测试组 | 最小覆盖 |
|---|---|
| T1 延时规则 | ±1/±8/±9、边界、实际基准、三lane方向、VTC/LOAD/读回周期 |
| T2 缺时钟 | ARM先于BITCLK、无时钟DELAY、RECAL无RX、无假tap代次 |
| T3 事务取消 | 延时每个阶段停钟、SYS超时、再来时钟、不迟到LOAD/ACK、不重放 |
| T4 状态跨域 | COMPLETE/FAULT、idle资格、fault清零、版本页/时间戳没有错误成功 |
| T5 复位/分频 | 停高/停低、任意撤销相位、DMA reset/RECOVER、reset-busy与首个真实FIFO握手 |
| T6 构建门禁 | 改源码/约束/日志/综合DCP触发拒绝，负时序/偏斜不报PASS |

每组设有界deadline；原有持续反压、部分包STOP/ABORT、溢出无假TLAST、满后排空测试继续通过。每个新TB纳入`simulation_gate.py`强制列表并绑定输入和日志哈希，而不是只交一张PASS截图。

**TB当前用sink替代DMA：**`tb_capture_fifo`可以证明接收与官方FIFO配合，不证明SG descriptor、HP0、DDR及软件所有权。不要把测试范围扩大描述成全系统SG通过。

### 第二批：外部时序齐备后形成下载候选

确认板级接口及源时序输入，启用实际约束，重新构建，审查所有阻断项已关闭；生成新bit和含bit的XSA，检查二者身份一致。

如果仍缺外部参数，可以交可恢复工程、DCP和准确PENDING列表，但不得冒充完整正式交付。

### 第三批：同一个正式软件入口进行板测

新XSA→M2 Platform→`radar_runtime`，使用官方XGpio/XUartPs/XAxiDma SG驱动，不运行旧SimpleTransfer程序。

先控制接口查询和复位，再已知码型/延时窗口，再有限真实raw包/ADC文件；最后在同一SG通路验证跨描述符和连续采集。软件及实机是后续任务，不要求本次硬件修复顺带完成完整FFT/DP/Linux。

---

## 12. 下一次交付的明确通过条件

### 12.1 内部RTL/构建修复必须交付

- 原IP保护清单不变，必要内部新模块有职责、依赖和接口说明。
- R01～R06逐项：原位置、diff、原因、回归用例、实际运行证据。
- 三条IDELAY.CLK/ISERDES.CLKDIV同网证明，REQP-1742=0。
- 延时每次步长/合法基准/读回和SYS超时取消已实现，不只有计划文字。
- 自定义功能CDC-10关闭；全部其他自定义Critical逐对象关闭或给出审核可接受的协议/物理证据，剩余未解释项则维持BLOCKED。
- 复位取消的两端一致性、停钟恢复和不重放经过测试。
- 原14次及新增必要测试通过，输入/日志身份匹配。
- 本次完整synth/route与输入/DCP身份一致，脚本能拒绝过期/混合产物。
- 最终时序对象及有效约束覆盖准确，max-delay/bus-skew余量合格。

### 12.2 下载候选另须满足

- 真实源同步时序预算与板级电气输入已经评审，不存在高速输入无依据的空模板。
- 完整routed时序通过，无DRC Error及未解释的关键告警。
- 当前Top/BD/pin/生成wrapper一致，XSA来自本次bit而非旧副本。
- 初始化/软件配置与新硬件匹配；不要求板测已通过才生成第一份候选。

### 12.3 用户板测的验收边界

有效CLI查询、受控复位、码型接收、有限raw包长度与内容、RX/IQ/chirp解释、SG跨描述符、连续真实帧和一次受控异常恢复逐层取得证据。缺某层就保留未完成，不用历史M1替代。

当前无需重新跑两小时DDR测试。除非修改DDR配置、更换硬件或出现直接指向DDR的错误，否则不重复这一专项。

---

## 13. 交付目录及工程化要求

建议下次一份干净包，保持可重开源码与证据分层，避免多次递归ZIP：

```text
M2_Closure_Delivery/
├── project/                    当前M2可重开工程，唯一生产RTL和生成依赖
├── evidence/
│   ├── baseline_identity.json
│   ├── change_list.md
│   ├── before_after.diff
│   ├── requirement_matrix.md   R01～R07逐条状态
│   ├── sim/                    全部原生日志、输入与日志哈希
│   ├── build/                  本次synth/route、DCP身份与脚本结果
│   ├── reports/                PHY/CDC/IO/输入/时序/偏斜/DRC
│   └── pending.md              只列真实未关闭事项
├── outputs/                    仅合格候选才含bit/XSA，不混旧产物
└── README.md                   解压根目录、打开方式、最高完成阶段
```

包内文档引用实际包布局，不把源树布局与清洁审核包布局混用。备份在工程外，不删除M1历史验收资料。`read-only audit`不修改工程；需要Validate/保存/生成的步骤明确标注为可写，并隔离临时副本。

保留一次独立解压重开验证，核对实际active file路径；证据日志中的历史绝对路径用于溯源，不必为了消除字符串而删日志。真正活动引用不得指向原机器Downloads或M1源目录。

---

## 14. 全局进度与后续方向

当前已越过：平台学习 → M1实机DDR/Simple闭环 → M2 IP配置 → 新RTL导入 → BD完整集成 → 模型功能仿真 → 首次综合布线。

当前正在：**PHY/CDC实现正确性收尾与外部输入时序签核**。

下一站：可信LVDS raw数据 → SG软件与有限帧实机 → 数据格式/标定/回放 → 官方FFT等处理模块 → PS DP显示。论文可继续保留会话ID、配置/标定版本、错误与时延记录，但没有查新和对照实验之前不宣称算法首创或性能收益。

不要再在“要不要重新配IP、重建Platform、保留两条数据线”之间切换。当前架构不需要推翻；需要的是把已经定位的实现问题真正关闭。

---

## 15. 本轮优先阅读的原始证据位置

以下路径相对于本次用户上传的主工程根目录：

| 对象 | 位置 |
|---|---|
| 当前生产PHY | `rtl/phy/radar_lvds_phy.v:36–71, 78–118` |
| 当前延时执行器 | `rtl/phy/radar_phy_delay.v:20–30, 66–103` |
| 当前复位结构 | `rtl/common/radar_reset_release.v:8–22` |
| 当前功能状态源 | `rtl/capture/radar_receive.v:35–39`；`rtl/control/radar_control.v:48` |
| 当前系统复位/快照域 | `rtl/top/radar_capture_bd.v:58, 90–123` |
| 当前约束判定漏洞 | `scripts/m2_real/check_constraints.tcl:133–155, 175–195` |
| 当前构建身份边界 | `scripts/m2_real/build_export.tcl:16–18, 41–46, 62–67`；`build_manifest.py:9–19` |
| 原生最终报告 | `outputs/m2_real_20261012_180512/reports/` |
| 原生XSim最终套件 | `outputs/m2_real_20261012_171114/xsim_final_02/` |
| 已有诚实的后续计划 | `docs/m2_real/NEXT_PHY_CDC_PLAN.md`，**尚未实现** |

---

## 16. 厂商依据

[S1] AMD **UG571 v1.16，2025-01-14**，Table 2-11（印刷页165–166）、TIME/VAR_LOAD流程（页173附近）、Align_Delay（页175）、Figure 2-29（页192）。已核对相关页面图表。用于确定本器件专用IO资源规则；不是板级实测数据。

https://docs.amd.com/api/khub/documents/kFbaUC5HGcXyGNauhgU6Gw/content

[S2] AMD **UG974 2025.1：IDELAYE3**。

https://docs.amd.com/r/2025.1-English/ug974-vivado-ultrascale-libraries/IDELAYE3

[S3] AMD **UG906 2025.1：Combinatorial Logic**。同步器前组合逻辑的毛刺风险。

https://docs.amd.com/r/2025.1-English/ug906-vivado-design-analysis/Combinatorial-Logic

[S4] AMD **UG974 2025.1：BUFGCE_DIV**。

https://docs.amd.com/r/2025.1-English/ug974-vivado-ultrascale-libraries/BUFGCE_DIV

[S5] AMD **UG572：BUFGCE_DIV**。

https://docs.amd.com/r/en-US/ug572-ultrascale-clocking/BUFGCE_DIV

[S6] AMD **PG085：Resets**。应结合当前实际FIFO单一reset接口和真实模型使用，不凭指南概述增加不存在的端口。

https://docs.amd.com/r/1.1-English/pg085-axi4stream-infrastructure/Resets

[S7] AMD **PG021：Scatter/Gather Mode**。描述符及包组织能力，不代表本工程SG已实测。

https://docs.amd.com/r/en-US/pg021_axi_dma/Scatter/Gather-Mode

**最终执行指令：在当前M2(3)基础上修复并验证，不再交另一份纯计划书；不能完成板级签核时仍先交内部问题已修复的真实工程和准确待办。**
