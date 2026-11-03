# RAW16-2L-600 原始传输合同

## 明确支持的输入

两条真实LVDS lane，每lane 600Mb/s，BITCLK 300MHz DDR，1:8解串得到75MHz。FRCLK也作为数据解串，**不是**用FRCLK驱动fabric clock，不是雷达图像的帧时钟。一个传输字16个UI，FRCLK高8UI、低8UI，idle是低；可以统一反转FRCLK极性。D0、D1的P/N反相和每字MSB/LSB顺序可配置。

ISERDES字节中Q0按最早bit处理。接收器跨8bit并行字节寻找完整16bit FRCLK模式，允许8种字节内相位，不在每次相位搜索时伪造ADC数据。有效区只接受规范的16bit字。错误捕获后会话作废，不尝试在一个已失步的会话里自动拼接有效帧。

支持空闲BITCLK持续、FRCLK idle低的模式；无BITCLK时数字控制面仍工作，排队ARM会等待真实RX时钟。首次保全数据必须在数据开始前同时满足：IDELAYCTRL RDY、解串复位释放、ARM已经进入RX、FIFO已完成内部复位且真实S_AXIS TREADY为高、接收器已识别FRCLK idle。外部`capture_resetn`经过64个真实RX周期延时后释放，仅代表FIFO复位输入释放，不能证明FIFO内部reset-busy已经结束。当前官方8192拍异步FIFO仿真中，复位输入释放后还经历约31个RX周期才接受数据；该观测值不应被当成板级通用固定等待常数。

接收器在FIFO TREADY仍低时保持ARM pending；TREADY为高且解码预热完成后才激活打包器，仍须由idle条件进入有效接收。没有合格idle时，打包器处于WAIT_GAP，`arm_pending`此时可以已清零。若时钟与第一数据字同时出现，预热/复位阶段的数据不被追认为已捕获，不能保证第一burst完整；必须提供足够的真实空闲时钟前导，或等下一段合格idle后发送新burst。运行中时钟缺失超过默认10ms将使尚未结束的会话无效；首个有效pair也建立失钟检测依据，不要求先等heartbeat翻转。不要拿默认超时支持未经确认的帧间长时间门控时钟。

**在没有确认固件输出符合上述合同前，保持format确认字段为0，ARM将被拒绝。**确认值1616同时意味着软件已确认双lane/600Mb/s/16bit/FRCLK有效性条件，而不只是设置一个软件标志来跳过验证。

## 数据保存与分包

lane0、lane1的第k个16bit字组成：

```text
pair[k] = {lane1_word[k], lane0_word[k]}   // 未开启lane交换时
TDATA   = {pair[2*j+1], pair[2*j]}
```

64bit内低地址/低位字顺序是lane0[k]、lane1[k]、lane0[k+1]、lane1[k+1]。MSB_FIRST标志只恢复16bit字的数值位序；不擅自改变I/Q或RX通道顺序。

- `packet_pairs`：每DMA运输包原始lane-pair数，必须偶数2…2097150；字节数为4×packet_pairs（8…8,388,600），全部TKEEP=FF。
- `packet_limit`：有限包个数；0为同一数据路径上的持续分包。
- `skip_pairs`：会话开始后跳过的原始pair个数，不是每chirp自动丢弃头部。
- `session_id`：软件指定会话标识，在状态sidecar保存，不插入原始载荷。

**TLAST只表示上述运输包末尾，不天然对应chirp、TI HSI burst或雷达frame。**包可以跨多个SG descriptor，软件以RXSOF/RXEOF与actual length组合。配置packet_pairs须计入实际HSI、CQ、填充和ADC载荷，不能只用采样点数公式冒充完整传输长度。

本版原样保留16bit线侧头部/数据/填充；不编造HSI magic、不固定RX1/RX2/I/Q交织、不假称已经解析已知目标距离。上层需依据实际bin/cfg解释。后续需要语义解析时沿同一通路增加，不新增第二套生产数据源。

## 停止、故障和复位

STOP在当前运输包结束后停止；若尚未开始接收，则可以取消空会话。STOP时已有半拍或不足一个完整运输包，只有后续真实pair补齐配置的运输包边界后才产生正常TLAST并完成。雷达不再提供剩余数据时，不填0、不补TLAST、不伪报完成；由应用超时/ABORT宣告会话无效并执行恢复。ABORT也不撤回已呈现的AXIS拍，该拍保持到握手或明确复位；即使该拍本来携带真实TLAST，故障会话也不会因此恢复为成功。

TREADY不能控制IWR6843。打包器有一个64bit待发寄存器和一个32bit半拍暂存；现有8192拍异步FIFO是唯一大缓冲。旧待发拍未被接收时不覆盖。无法保存新pair时锁存overflow、冻结会话，保留旧AXIS拍直到接收或明确的复位。

软件不得把SOURCE_COMPLETE当作DMA完成。源端完成说明本次配置的最后运输拍已经被FIFO输入接收；FIFO出口可能仍因TREADY为低而保留全部或部分数据。软件还须分别确认FIFO/DMA数据已排空、SG descriptor完成及actual length、相关错误状态和缓存一致性。真实UNISIM PHY与当前官方FIFO的`tb_capture_fifo`验证了源端完成早于FIFO出口排空、出口反压保持与完整排空顺序；其DMA侧是测试接收端，未实例化AXI DMA/PS/DDR，不证明DDR落盘、SG软件或板级采集成功。掉电/掉RDY/失CLK/格式错/溢出发生后，受影响会话不能仅因为部分SG descriptor完成而被宣称有效。

RECOVER会清空FIFO与接收逻辑。0xC1EA是软件已确认“雷达停止、DMA无在途事务或已复位”的确认码；不是硬件可以从GPIO读出总线已排空的证明。正常完成后的下一会话也先排空/回收后RECOVER，以免旧数据混入新会话。

## 校准

300MHz参考驱动IDELAYCTRL；复位延长64参考周期。可在无会话时配置三个9bit tap count（FRCLK/D0/D1），VAR_LOAD前后各留≥12周期EN_VTC关闭窗口，再恢复VTC并等待就绪。软件可请求原子tap读回。

默认延时为0，仅是合法起始参数，不代表居中。应通过真实已知码型扫描出每lane可用窗口，再选择有裕量的位置。FRCLK对齐通过只验证控制lane模式，不证明两条data lane BER为0。重校准将原语回到初始延时；需要时重新应用已确认tap设置。
