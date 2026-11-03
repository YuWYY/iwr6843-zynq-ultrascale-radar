# R07：真实输入时序与板级确认

核对日期：2026-10-14。`SOURCE_TIMING=PENDING`、`BOARD_ELECTRICAL=PENDING`、
`DOWNLOAD_CANDIDATE=NO`、`BOARD_TEST=NOT_RUN`。本轮核对原始资料并准备可审计的
合同与约束生成入口；不把默认配置、设计目标或旧 routed WNS作为实际采样签核。
按用户最新要求“我自行导出，你把核验做完”，本轮仅完成当前工程核验；
`HARDWARE_EXPORT=NOT_RUN_USER_MANAGED`、`BITGEN=NOT_RUN`，不替用户执行任何新导出。

## 已核实的来源及边界

TI SWRS219F（Rev F，2025-04），§7.12.4、图7-12、印刷页46，支持两数据lane、
BITCLK、FRCLK及600 Mbps / 300 MHz DDR档位。这证明器件能力，不证明当前bin/cfg
已经选择该档位。表7-7的330 ps边沿与80 ps pk-pk jitter有900 Mbps条件；图7-13
（页47）的1100 ps也不是本工程600 Mbps每lane双边沿min/max保证，均未用作预算。
本次核对的这些章节未提供可直接覆盖本连接与工作范围的完整源时序合同。
[TI SWRS219F](https://www.ti.com/lit/ds/symlink/iwr6843.pdf)

本地F20手册页33、底板原理图页14及核心原理图页8/10已核对：Bank65目标1.8 V
符合HP LVDS内部终端要求，但实物选择尚未确认。底板的默认1V8和5mil等长标注
分别是设计默认和走线目标，不能升级成实际电压与传播偏斜。转接板网表只有连接器；
TI目录中多个修订仅为候选资料。元件/连接边界见
[board_io_contract.md](../m2_real/board_io_contract.md)，原件哈希和搜索范围见
`delivery/20261014_r07_01/evidence/board_sources.json`。
[AMD UG571 v1.16，页121](https://docs.amd.com/api/khub/documents/kFbaUC5HGcXyGNauhgU6Gw/content)

## 三路输入合同

参考时钟只有`radar_bitclk_p`上的`RADAR_BCLK300`，名义周期3.333333 ns、UI
1.666667 ns；RX75和SER300保持BITCLK派生关系。FRCLK在当前RTL中是解串的字边界
输入，不建立独立时钟，也不代表DMA TLAST。独立REF300不作输入参考；差分负端
不再加第二份输入延迟。

下表单位ns，所有值必须有审核依据；当前没有可批准的数字。

| 端口 | rise_min | rise_max | fall_min | fall_max | 数据来源、参考平面、适用条件 |
|---|---|---|---|---|---|
| radar_data0_p | PENDING | PENDING | PENDING | PENDING | 实际固件边沿模式、完整路径与工作范围未确认 |
| radar_data1_p | PENDING | PENDING | PENDING | PENDING | 实际固件边沿模式、完整路径与工作范围未确认 |
| radar_frclk_p | PENDING | PENDING | PENDING | PENDING | 实际FRCLK相对BITCLK双边沿时序未确认 |

每lane、每个BITCLK参考边沿分别使用：

```text
input_min = source_min + data_path_min - clock_path_max - early_margin
input_max = source_max + data_path_max - clock_path_min + late_margin
```

source与path必须采用同一明确参考平面。若在FPGA端已测入全链路窗口，就不再叠加
同段走线；内部IDELAY与芯片路由交给工具分析。margin仅补足尚未计入的相对抖动、
偏斜、测量误差及余量，已进窗口的项目不再加到clock uncertainty。统一包络只有
覆盖三lane时才适用；当前合同按lane独立记录，不以预估分配额替代实物依据。

## 可执行入口与签核范围

`scripts/m2_closure/R07_contract.json`记录源时序、电气、实际bin/cfg及证据引用。
审核证据须复制到工程内并登记SHA-256；原理图的存在不能把实物状态设为确认。
从工程根目录运行：

```text
python -B -I scripts/m2_closure/source_timing.py inspect --root .
python -B -I scripts/m2_closure/source_timing.py generate-xdc --root . --output constraints/radar_source_timing.xdc
```

第二条仅在真实源时序审核通过后创建XDC，当前PENDING应拒绝生成，不填0ns或猜测
常数。生成器不自动修改XPR；native Vivado负责激活唯一最终文件并核对模板未重复
生效。每lane需要rise min/max与`-clock_fall -add_delay`的fall min/max；参数必须
有限、min≤max，保留来源、适用条件和合同哈希。实际生成值按1 ps网格向外包络（min向下、max向上），原始计算值与生效值并列记录，避免工具精度使窗口变窄。`inspect`退出0仅表示合同可解析；`eligible=false`仍不允许候选导出。生成器采用独占创建，不覆盖已有最终XDC。
[AMD UG903 2025.1，DDR输入延迟选项](https://docs.amd.com/r/2025.1-English/ug903-vivado-using-constraints/Use-of-set_input_delay-Command-Options)

实际生效后必须新综合/布线，逐lane导出双边沿min/max输入路径、端点、约束覆盖和
余量。低速异步`radar_3v3_present`、`radar_nerr_n`、`radar_uart_rx`保留窄CDC边界；
不为清零no_input_delay填0，不以高速false_path、全域异步组或DRC降级绕过审核。

第一版可签核的IDELAY范围仅允许明确的`RESET_CALIBRATED_INITIAL_ONLY`，对应
当前实现`DELAY_VALUE=0 ps`所表达的初始校准工作点；这不等于物理绝对tap计数为0。
软件可写的其他tap与相位均排除在该签核范围外，不能由默认点WNS推导0..511全范围
通过。需要其他工作点时，应补各设置的采样窗口与当前实现证据，保持控制ABI不变。

## 原生生成顺序与构建身份

本轮发现：仅`open_project`后直接`generate_target`，可能丢失SmartConnect派生复位
的传播极性，使`ext_reset_in`仍接常量1而`C_EXT_RESET_HIGH`回到默认1，导致内部
持续复位。b04又暴露了旧SmartConnect父级HDL引用已失效长模块别名的问题，综合失败。
本轮完整恢复在同一Vivado会话打开主BD，对主BD文件依次执行`reset_target all`、
`validate_bd_design -force`、`save_bd_design`、`generate_target all -force`，最后更新
综合编译序列；不能仅再生成局部叶模块，也不要直接Validate某个scoped child BD来
代替主BD传播。现有`run_integrate.tcl`包含父BD校验/保存/生成流程；本次处理陈旧
派生文件时额外先重置完整主BD目标并强制重生成。该原生重置/生成方式参考
[AMD UG896 2025.1](https://docs.amd.com/r/2025.1-English/ug896-vivado-ip/Using-Tcl-Commands-to-Reset-and-Generate-Target-IP)。

`build_identity.py`现于综合前核对两个SmartConnect实际采用的派生复位XCI、综合
VHDL、父级连接与常量源：EXT=0、AUX=0、ext接常量1、aux接aresetn，且文件唯一处于
当前活跃源及综合编译序列。140项发布门槛回归和43项时序合同回归已通过，共183项；
这是脚本验证；原生结果另有独立凭据。`20261014_r07_b05`的全新综合、布局布线及
内部PHY/CDC/DRC核验已通过：WNS=+1.085 ns、WHS=+0.010 ns、WPWS=0.000 ns，
16932/16932条网络完成布线且错误0。只读重开确认两run均Complete、NEEDS_REFRESH=0，
并重新核对当前输入/DCP/报告身份及2053项配置、连接和地址。详见本轮`RESULT.md`
和`verification/native_warning_review_b05.md`。这不代表真实外部输入时序或实板通过；
本次未执行硬件导出，XSA由用户自行操作。

在已封存构建后重新force Validate或生成产物，会改变需审核的生成状态。应先完成
生成，再用新的build_id重新综合、布线与导出；不得把旧DCP/XSA当作新生成状态的证据。
按用户最新要求，直接交付主线目录和核验文件，不制作压缩包；XSA由用户自行导出。

## 用户自行导出参考（本轮未执行）

以下两阶段命令仅供用户后续自行操作时参考。本轮`20261014_r07_b05`不运行
`dev-hw`或`candidate`原生导出，也不生成或交付XSA；当前核验结果不包含XSA通过声明。

`dev-hw`复用同一build_id已经签核的当前synth/route身份，不能为导出另起一个没有
构建凭据的空目录。以下以`20261014_r07_b05`示范命令形式；只有该构建已通过内部
门槛且本次导出尚未尝试时才运行。已交付目录不重复执行，后续修改使用新的build_id
从synth/route开始。PowerShell第一阶段：

```powershell
$projectRoot = '/fpga'
$buildId = '20261014_r07_b05'
$buildDir = "$projectRoot/outputs/m2_closure_$buildId"
$simulationGate = "$projectRoot/outputs/m2_closure_20261013_c1/sim_06/SIM_PASS.json"
& 'C:/Xilinx/2025.1/Vivado/bin/vivado.bat' -mode batch -source "$projectRoot/scripts/m2_closure/build_export.tcl" -log "$buildDir/dev_hw_native.log" -journal "$buildDir/dev_hw_native.jou" -tclargs dev-hw $buildId $simulationGate
$nativeExitCode = $LASTEXITCODE
if ($nativeExitCode -ne 0) { throw "Vivado dev-hw failed: $nativeExitCode" }
```

原生进程退出0只到`NATIVE_COMPLETE`，不能认定开发XSA已经PASS。待此进程退出、
`dev_hw_native.log`完整关闭后，第二阶段在host执行：

```powershell
python -B -I "$projectRoot/scripts/m2_closure/hardware_export.py" seal --mode dev-hw --root $projectRoot --build $buildDir --native "$buildDir/dev-hw_native_after.json" --simulation $simulationGate --native-log "$buildDir/dev_hw_native.log" --process-exit-code $nativeExitCode
if ($LASTEXITCODE -ne 0) { throw 'Development XSA post-process qualification failed' }
```

`--process-exit-code`必须来自刚结束的原生进程；上述检查通过时其值为0，不能用手填0
掩盖失败。封存检查完整日志的2025.1头、同mode/build命令、完成标记、最后退出记录
及ERROR/Critical，重新核对输入/仿真/DCP/报告身份，并检查XSA内容与当前HWH一致且
不含bit/binary。只有`dev-hw/export_manifest.json`成功生成且`dev-hw_STATUS.json`
变为PASS，才是`VITIS_COMPILE_ONLY_NO_PL_IMAGE`开发交付；输出文件为
`dev-hw/radar_system_m2_dev_hw.xsa`。用户如采用此流程，所导出产物仍须绑定该构建
凭据；本轮不执行上述步骤。`candidate`路径在当前R07 PENDING状态应于preflight
拒绝，不进入bitgen；本轮只进行不创建产物的Python候选预检来验证该拒绝边界。

## 待用户补充表（唯一汇总）

| 待项 / 合同内容 | 当前能确认的事实 | 需补充的具体依据与推荐取得方式 |
|---|---|---|
| 实际硬件身份、Bank65供电及完整电气路径 | 固定球位已保留；手册/图纸给出HDR1可选1.2/1.8 V、设计默认1.8 V；HP LVDS内部终端要求1.8 V | 记录F20核心/底板、转接板、IWR6843ISK实物修订及连接路径；确认Bank65跳线处于1.8 V，必要时在已识别测试点记录电压；核对全路径终端、接地、方向/P-N、Bank64参考网络、Bank44及掉电兼容性。仅断电调整跳线。 |
| 实际固件、cfg和输出模式 | 器件支持两lane600 Mbps；已有RAW16合同是接收要求；本地未找到可认定为已烧录的bin/cfg记录 | 提供实际bin SHA-256、SDK/CLI版本、使用的cfg及启动记录；确认clock/data对齐/极性、lane顺序、16bit字、FRCLK高8UI低8UI及空闲时钟前导，记录至合同固件/模式字段。 |
| 三lane双边沿窗口、完整相对路径与签核范围 | 名义BITCLK300/RX75关系明确；TI900 Mbps典型值及5mil设计目标不能作为预算 | 取得适用于600 Mbps及实际硬件的发射保证，结合完整板/线缆传播范围；或在明确参考平面进行受控测量，记录仪器/探头、误差、温度/供电与板卡范围。逐lane/edge填写八项预算和source/path/margin证据，限定IDELAY工作点并完成审核。 |

缺口未关闭时，用户自行导出的不含bit开发XSA只能用于Vitis编译准备，它不是可下载
PL镜像；本轮未执行该导出。候选bit及含bit XSA需实际约束、物证与当前原生报告全部通过；
候选门槛不要求先板测PASS，板测是后续独立步骤。本轮不扩大为SG/DDR实机验证。
