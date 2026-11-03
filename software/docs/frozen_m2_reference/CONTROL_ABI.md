# 唯一GPIO控制 ABI — 0x52430101

硬件基址不变：AXI DMA=0xA0000000，唯一AXI GPIO=0xA0010000。CH1(DATA偏移0x0)软件输出32位；CH2(DATA2偏移0x8)输入响应32位。使用官方XGpio或对应合法MMIO访问。

地址、GPIO位号、opcode及原始tap单位保持不变。本版更新了就绪资格、PHY事务结果、取消和tap测量身份语义，软件必须检查page00为`0x52430101`；不能将旧`0x52430100`、M1或旧M2-A `0x4D321000`应用直接视为兼容。本文件描述当前RTL接口，不是SG软件或板测通过记录。

## 1. 请求与应答

```text
CH1：bit31 toggle | bit30..24 opcode(7) | bit23..16 index(8) | bit15..0 argument
CH2：bit31 ACK    | bit30 error        | bit29..24 result(6)| bit23..16 index | bit15..0 value
```

每条命令必须翻转一次toggle，写完整32位；只改变参数而不翻转不会执行。先等待ACK匹配，才提交下一条。ACK表示SYS已经处理/排队，不等于RX已ARM或DMA已完成。热重启CPU时先读取GPIO现存toggle，不假定硬件已经复位。

GPIO响应的6bit结果码：0已处理/排队；1未知opcode；2忙；3配置index越界；4参数错误；5未就绪/状态页无有效快照；6RAW16合同未确认；7需恢复；8恢复确认码错误。它与page0F中的4bit **PHY执行结果**是两组独立枚举；GPIO返回0不代表延时或校准已经成功。

## 2. 命令

| opcode | 名称 | index / argument |
|---|---|---|
| 00 | SAFE | index=0,arg=0；IO允许清0，取消待交付采集命令及在途PHY事务；未结束会话记录中止，不清FIFO或DMA |
| 01 | IO_SET | index=0，arg低4位=IO_ALLOW、UART_ALLOW、RESET_HOLD、SYNC_LEVEL；高12位必须0 |
| 02 | CFG_LO | index0…7，写影子寄存器低16位 |
| 03 | CFG_HI | index0…7，写影子寄存器高16位 |
| 04 | READ | index是状态页，arg0读低16位、arg1读高16位 |
| 05 | ARM | index=0,arg=0；以arm_admissible资格原子排队；允许REF已校准但BITCLK尚未出现；每次RECOVER后最多一个会话 |
| 06 | STOP | 请求在运输包边界停止；无隐含UART sensorStop |
| 07 | ABORT | 会话存在时记录粘性错误；即使无会话也取消待交付采集命令及在途PHY事务；不伪造TLAST |
| 08 | RECOVER | index=0,arg=0xC1EA；软件确认DMA已静止后执行，清空接收与FIFO |
| 09 | SNAPSHOT | index=0,arg=0；请求RX状态及RX域tap测量快照；需RX就绪且基准有效，与DELAY/RECAL互斥；RX没时钟不假装完成 |
| 0A | DELAY_APPLY | 应用配置5、6、7的绝对tap count；必须无会话、RX就绪、基准有效且无快照在途 |
| 0B | RECAL | 无会话且无快照在途时重新执行REF校准；无BITCLK也能完成REF事务，RX基准另行等待真实时钟 |

除IO_SET和配置写外，未列明参数均应写0。当前实现对部分命令的保留参数不解释，不扩展其用途。改变UART输出允许、NRST、SYNC前必须确保串口事务空闲；紧急掉电例外。

IO_SET常用arg：0=全输出关闭；3=IO+UART允许、NRST释放、SYNC低；7=IO+UART请求但NRST拉低（实际UART被屏蔽）；释放复位再写3。电源掉落后先写0，再明确写3恢复，供电恢复本身不会自动输出。

## 3. 配置寄存器（仅无会话/无待交付命令时可写）

| index | 含义 | 默认 |
|---|---|---|
| 0 | packet_pairs，每包32bit lane-pair数，偶数2…2097150 |32768=128KiB|
| 1 | packet_limit，0连续，非0有限运输包数 |1|
| 2 | skip_pairs，会话开头跳过的pair数 |0|
| 3 | session_id，软件自行编号 |1|
| 4 | bit31..16=0x1616合同确认；bit4..0格式；bit15..5须0 |0=禁止ARM|
| 5 | FRCLK tap count 0…511 |0|
| 6 | DATA0 tap count 0…511 |0|
| 7 | DATA1 tap count 0…511 |0|

format低位：bit0=FRCLK反相；bit1=DATA0反相；bit2=DATA1反相；bit3=MSB_FIRST；bit4=交换lane顺序。确认MSB-first、无反相、不交换时index4=0x16160008。确认值不得替代真实固件/波形检查。

配置直到ARM才整体进入RX，写一半配置不会影响进行中的采集。tap配置直到DELAY_APPLY才进PHY，影子值不是实际tap读回。一个已完成的会话仍保持session_open，需完成DMA排空/回收后RECOVER，再开始下一会话。

DELAY由RX75执行，RECAL由REF300执行，SYS拥有受理、超时和取消。flags19包含发送拍及整个在途所有权；mailbox的交付ACK不会清除它。生命周期为`accepted → delivered → executing → succeeded / failed / canceled`，短暂阶段不保证软件能逐拍观察。GPIO接受请求后，等待flags19清零、page10事务generation变化，再解释page0F终态；GPIO层直接拒绝的命令未发送给PHY，不增加page10。

三个IDELAYE3的控制时钟与ISERDESE3 CLKDIV都为RX75，IDELAYCTRL参考仍为REF300。tap读回来自实际CNTVALUEOUT，在RX合法低EN_VTC窗口读取并检查连续观察一致，完成后保持；不是软件配置回显，也不是VTC开启期间的实时计数。启动/RECAL首先读取真实校准基准，不额外LOAD零。DELAY逐lane向绝对目标前进，每次实际LOAD变化不超过8tap，已到目标的lane不LOAD；目标低于该lane当前合法基准返回PHY结果3并保留原设置。0…511仅是编码范围，0不因此保证是当前TIME模式的合法目标。

page11是已通过RX→SYS原子报告传输的基准，page12是成功取得实际基准的代次；基准无效时page11返回GPIO结果5。无RX的RECAL只完成REF校准、使旧基准失效，不增加基准或tap测量代次；真正RX采样完成后才重新置有效。tap测量值与实际采样代次作为同一个64bit快照传输，见page0B/0C。实际tap代次仅在合法稳定采样流程完成时增加，跨本地取消/RECAL保持，仅全局RX复位清零；无RX时钟不会推进。重复复制相同测量只增加page13复制代次，不能宣称新测量。

## 4. 状态页（READ后16bit响应）

| page | 32bit值 |
|---|---|
| 00 | ABI 0x52430101 |
| 01 | 实时SYS状态flags，定义如下 |
| 02 | 低16位SYS粘性故障 |
| 03 | 最近配置的session_id |
| 04 | 最近一次RX heartbeat变化或首次接纳数据观察至今的SYS周期数；未观察到活动时0xFFFFFFFF |
| 05/06 | 首个已接纳数据的SYS观察时间戳低/高32位 |
| 07/08 | 无故障、无同拍取消的源COMPLETE之SYS观察时间戳低/高32位；包括STOP取消pending ARM的终态，不代表有数据 |
| 09 | RX快照generation |
| 0A | ARM之后经历的SYS周期数 |
| 0B | RX快照中的合法窗口tap读回：FR[8:0],D0[17:9],D1[26:18]；未取得快照返回GPIO结果5；有效性还需核对page0F |
| 0C | 与page0B原子保存的实际tap测量generation；未取得快照返回GPIO结果5；不随重复SNAPSHOT增加 |
| 0D | 名义SYS_CLOCK_HZ=100000000，不是晶振频率计实测 |
| 0E | 软件IO控制请求低4位 |
| 0F | PHY资格与事务状态：[3:0]执行结果，[6:4]phase，[7]baseline_valid，[8]ref_cal_ready，[9]rx_ready，[10]arm_admissible；其余0 |
| 10 | PHY终态generation，每笔发送给PHY并终结的请求增加一次，含PHY内部拒绝、失败、取消 |
| 11 | 有效校准基准，lane位序与page0B一致；无有效基准返回GPIO结果5 |
| 12 | SYS接收到真实RX基准报告的generation；RECAL无RX不增加 |
| 13 | tap快照复制generation，仅说明一次有效快照返回，不代表新测量 |
| 14 | 低4位PHY独立粘性失效原因，与PHY执行结果复用枚举；包含无软件命令在途的自动基准失败/RDY丢失；全局PHY复位或受理显式RECAL清零 |
| 20…2B | 最新12×32位RX快照 |
| 40…47 | 8个SYS影子配置寄存器 |

时间戳来自源域寄存状态经3FF同步后的SYS观察，包含新增源寄存及同步流水线延迟；不是精密chirp触发时间或RF相位时间。ARM/RECOVER清零记录，SYS tick本身持续计数。故障或SAFE/ABORT/RECOVER与COMPLETE同拍时，不新写成功结束时间戳；它不证明FIFO排空、DMA/SG完成或DDR有效。跨两次READ读取会变化的32bit值时，应以generation/有效标志前后复核，变化则重读。

flags bit：0 IO逻辑允许；1同步PG；2PG资格；3同步NERR_n；4掉电锁存；5RX PHY就绪（rx_ready）；6解串复位已释放；7接收控制面近期观察到RX活动；8FIFO写复位释放；9session_open；10观察到first数据；11观察到源COMPLETE；12RX源故障状态；13采集命令在途；14快照在途；15RX快照有效；16RECOVER活动；17RX ARM待激活；18RX寄存idle资格；19PHY延时/校准命令在途；20DMA上游复位释放；21逻辑UART允许；22逻辑NRST拉低请求；23逻辑SYNC输出允许。其余0。

bit8仅表示外部FIFO写复位已释放，不能替代官方FIFO内部真实TREADY；ARM激活仍等待TREADY和解串资格，此后还要观察真实FRCLK idle。命令取消会清RX控制/状态寄存器，bit12可撤销；page02 SYS粘性故障及packetizer快照错误才是跨取消保留的诊断依据。

flags中的IO输出状态是数字逻辑条件，不是板外电压测量。实际OBUFT使能还受raw PG关断；不要把NRST位当作回读电压。

SYS faults位：0电源异常；1NERR；2REF校准丢失或已观察首数据后RX PHY资格丢失；3活动中失CLK；4活动中DMA复位；5ARM后首数据watchdog到期；6未结束会话通过IO_SET禁用/复位；7RX错误；8显式ABORT/未结束会话SAFE。默认首数据watchdog为500000000个SYS周期（名义5s），接收控制失钟watchdog为1000000个SYS周期（名义10ms）；首数据也建立活动基准，不必先等待heartbeat。只在有效RECOVER或全局复位清除。

## 5. 原子RX快照

SNAPSHOT受理后等待flags14清零且flags15置位，并确认page09和page13复制generation更新，再分两次读各32bit页面。冻结数据直到下一次请求完成，不对异步多bit值逐位拼接。两组快照分别原子，不能假定RX状态与tap采样发生在完全同一RX拍。tap组只有源端测量有效、稳定且非执行中才ACK；接受后RX停钟会保留busy和旧数据，不产生新generation，可用RECOVER取消。

| 快照word/page | 含义 |
|---|---|
|0/20,1/21|已接纳raw pair计数低/高（64bit）|
|2/22,3/23|已被FIFO实际接收的64bit AXIS拍数低/高|
|4/24|实际握手TLAST次数，仍不等于DDR完成|
|5/25|FIFO写占用高水位|
|6/26|RX当前session_id|
|7/27|bits18..16 state；bits15..0 RX错误|
|8/28|解码器观察到的有效pair计数，包括采集窗口外，不当作存储长度|
|9/29|解码器畸形FRCLK计数|
|10/2A|bit0AFULL_seen，1PROG_FULL_seen，2deser_resetn，3stream_resetn，4burst_first(瞬时)，5ARM_pending，6decoder_enable，10..7 phase，15..11 format，16pending_ARM_canceled|
|11/2B|当前FIFO写计数（32bit）|

state：0IDLE，1WAIT_GAP，2ACTIVE，3DRAIN，4COMPLETE，5FAULT。pending ARM在真正有RX时钟、FIFO/解串准备好后才激活；WAIT_GAP只等待真实观察到的idle，不伪造空闲。

RX错误bit：0overflow；1FRCLK格式；2abort/解串就绪丢失；3错误ARM时机；4非法包长。发生错误不自动重新对齐为“下一张有效帧”；软件应丢弃受影响会话。

page0B/0C可以保留此前有效快照；它们不是“当前PHY仍有效”的保证。读取前后检查page0F基准有效/RX就绪、flags19以及page13复制代次。无BITCLK时新SNAPSHOT返回NOT_READY；若时钟在受理后停止，则必须等待恢复或执行RECOVER，不可把旧page0B当作新的测量结果。

## 6. PHY事务结果与恢复

page0F的执行结果（4bit）为：

| 值 | 名称 | 含义 |
|---|---|---|
| 0 | NONE | 尚无结果或新事务已受理 |
| 1 | SUCCESS | DELAY真实读回达到目标；RECAL仅表示REF校准完成，RX基准仍需单独确认 |
| 2 | NOT_READY | PHY二次检查拒绝当前DELAY，不留下在途所有权 |
| 3 | BELOW_BASELINE | 目标低于实际合法基准，保留已有校准和tap设置 |
| 4 | READBACK_INVALID | 合法窗口两次读回无效/不一致；仿真未知值也走失败 |
| 5 | NO_CONVERGENCE | 步进后实际值不符或迭代不收敛 |
| 6 | RDY_LOST | 已建立的校准资格丢失 |
| 7 | TIMEOUT | SYS事务deadline或执行期间时钟活动监督超时 |
| 8 | CANCELED | SAFE/ABORT/RECOVER/DMA reset取消在途事务 |
| 9 | CALIBRATION_FAILED | REF校准失败报告 |

phase值：0 IDLE；1 ACCEPTED；2 DELIVERED；3 EXECUTING；4 SUCCEEDED；5 FAILED；6 CANCELED。phase保留终态，直到下一笔受理或全局PHY复位。结果与generation一起使用，不能把上一笔SUCCESS用于下一笔命令。

当前默认操作deadline：DELAY为10000个SYS周期（名义100us），RECAL为120000个SYS周期（名义1.2ms）；REF等待RDY最多300000个REF周期（名义1ms）。PHY另以128个SYS周期监督真实RX heartbeat（名义1.28us），与page04的接收会话失钟计数不同。它们是设计运行上界，不是硬件性能实测值。

超时、执行中取消或硬故障先持久复位事务两端和执行器，再等待8个SYS周期隔离后结束busy，时钟恢复不补发旧LOAD、旧结果也不能完成下一事务；恢复需要显式RECAL。低于基准的普通拒绝不破坏原设置。空闲SAFE/ABORT不强制重新校准，也不产生page14结果8；但若其取消了实际在途事务，则按上述失效流程处理并锁存原因8。

每次显式RECAL受理时，REF请求及结果mailbox两端也统一隔离8个SYS周期，再开放并派送本次请求。这样上电自动校准产生的迟到失败报告不能终结新RECAL；该保护仍计入原1.2ms操作deadline，不改变GPIO命令、单位或generation定义。

page14独立记录使PHY隔离的原因，不改变软件事务的cfg_result/generation。例如RECAL的REF部分已经成功，随后自动RX基准读回失败，则page0F仍可能保留该RECAL的SUCCESS、page10也保持原代次，但page14为4、baseline/RX/ARM资格撤销，需要显式RECAL。无命令在途的RDY丢失由page14原因6可见；不能单看旧cfg_result=SUCCESS推断当前RX可用。page14在全局PHY复位或受理显式RECAL时清零；普通RECOVER本身不冒充重新校准成功。

初次自动REF校准的RDY等待超时或稳定等待阶段RDY丢失，同样通过page14原因9可见，即使REF从未就绪、从未发出软件命令。此时page0F命令结果与page10仍保持原值，不伪造一次软件事务；恢复须显式RECAL。启动尚在合法等待期间则page14保持0、REF/ARM资格为0。

三个资格分别是：REF校准完成、RX已有真实基准且近期时钟活动正常、允许提交ARM。只有第三个允许无BITCLK排队，它不声称已有数据或可执行DELAY。初次启动或无RX的RECAL后，软件可完成UART配置并排队一次ARM，再启动真实雷达时钟；超过首数据deadline会使该会话无效，恢复时不得自动继续旧ARM。

## 7. 取消、FIFO与软件所有权

SYS寄存复位所有者合并DMA reset、RECOVER、abort及事务取消，断言传播最多两个SYS拍，不依赖RX边沿；目的域用3级XPM异步断言/同步释放。RECOVER保持128个SYS周期；FIFO的单个现有reset随后还需3级同步和64个真实RX保持周期，并等待官方内部TREADY，不以固定64拍宣称FIFO已能接收。

SAFE/ABORT只取消命令和接收控制，已有阻塞TVALID/TDATA/TLAST继续保持到真实握手或正式stream reset，packetizer在恢复RX边沿后进入FAULT；不得借取消撤销已承诺的AXIS拍。RECOVER、DMA reset或全局复位才清FIFO/packetizer与快照。没有RX时STOP可能因ARM mailbox尚未交付返回BUSY；立即取消应使用ABORT/SAFE，之后按软件确认流程RECOVER。

RECOVER的`0xC1EA`是软件声明“雷达已停止、DMA已静止/复位”，硬件不能通过GPIO验证SG outstanding。软件先使DMA/descriptor所有权安全，再恢复；任何源COMPLETE、FIFO排空或本机TB PASS都不等于SG descriptor、HP0或DDR已通过实测。
