# R06 build identity and release gates

状态：门禁实现与 T6 回归已完成；本轮最终综合、布线、CDC/PHY 资格仍须由当前冻结输入的原生报告证明。88 项是包含正向基线和负向注入的门禁回归总数，不是 88 次真实 Vivado 构建，更不是板级通过。

## 入口及不可复用的尝试

`scripts/m2_closure/build_export.tcl` 由独立 Vivado 2025.1 batch 进程执行，固定三个参数：

```text
<synth|route> <unique-build-id> <absolute-SIM_PASS.json>
```

同一 build-id 的 synth 和 route 共用 `outputs/m2_closure_<build-id>/`；每个 stage 只能首次执行一次。已有状态、manifest、receipt 不覆盖。失败尝试保留 FAIL 和原生日志；修正输入后使用新 build-id，重新完成 synth，再 route。入口固定 part、顶层、仿真顶层、BD 全局综合及禁用增量检查点，不生成 bit/XSA 或执行硬件操作。

## 完整输入身份

`native_export.tcl` 从实际打开的项目读取配置，不把易变的 XPR RunState/cache 元数据直接混进源码身份。它记录 part、top、fileset 的 include/define/generic/library 等设置、run flow/strategy/step 参数、增量设置，以及原生文件属性与处理范围。

RTL 工程的实现阶段使用综合得到的映射网表，不能调用 `get_files -compile_order sources -used_in implementation`。当前导出明确包含四项：

| 项目 | 实际来源/校验 |
|---|---|
| synthesis_sources | 原生 synthesis 源文件编译顺序 |
| synthesis_constraints | 原生 synthesis XDC 处理顺序 |
| implementation_constraints | 原生 implementation XDC 处理顺序 |
| implementation_inputs | 所有启用且 USED_IN_IMPLEMENTATION 的原生文件集合 |

同时保留 `get_files -all` 中启用的综合/实现输入及生成依赖、每项 USED_IN/SCOPED_TO/PROCESSING_ORDER 等属性。Python 要求四项清单非空、成员真实存在且已列入输入，约束项必须为 XDC，实现集合必须和文件属性精确一致。因而新增或移除活动 XDC、改变处理顺序/范围、改变原生 IP/生成文件、替换源码或改动设置都会改变身份。保守收集的 IP 支持文件不因未出现在顶层 HDL 列表就被遗漏。

`build_identity.py` 生成 canonical JSON 的 SHA-256：

- 项目内部路径转换为 `project/...`；批准的 `C:/Xilinx/2025.1` 工具文件为 `tool/...`。Downloads 与其他外部设计输入拒绝。
- 每个原生设计输入有 SHA-256；所有生产 RTL 必须在原生输入集合中。
- 所有 `scripts/m2_closure/**/*.{py,tcl,json}` 都纳入身份，包括新 helper、契约、门禁、策略与 T6；添加/删除脚本同样改变身份。
- 当前调用链复用的 `scripts/m2_real/package_review.py`、`scripts/m2_real/reopen_package.tcl`、`scripts/m2_real/common.tcl` 通过固定必需清单另外纳入 SHA-256；任一缺失或变动都拒绝沿用原 synth 身份。此次仅绑定其字节，不修改历史 helper。
- 加入真实完整仿真 gate 的路径、SHA-256 和实际测试集合。
- manifest 含唯一 build-id、自身内容身份和生成时间；后续校验重算当前集合，不只遍历旧 manifest 的键。

原生 API 修正已经在真实 RTL 项目上只读预检通过：`outputs/m2_closure_20261013_c1/inventory_preflight.log`，2026-10-13 18:09，`INVENTORY_PREFLIGHT=PASS`。该次 JSON 记录 313 个保守输入、140 个综合源、14/15 个综合/实现约束及 191 个启用实现输入，没有输入 DCP。这些是当次预检观测，生产 gate 不硬编码这些总数。第一次 build01 在该 API 预检前置步骤失败，尚未综合；原始失败日志保留，不能改记为 PASS。

## 仿真必须是当前完整套件

门禁动态读取 `required_tests.json`，当前为 13 个主 TB 加 4 个相位运行。每一项必须存在于 SIM_PASS、suite STATUS 和真实 run 日志中并为 PASS；诊断 `--tests` 子集不能取得 release gate。

gate 还检查 suite 的 `input_hashes.json` 与 gate 一致、生产 RTL 和每个要求 TB 均被绑定、定义文件被绑定、实际全部日志成员与 log_hashes 精确相同、所有输入和日志 SHA 一致、对应 `PASS <tb>` 标记存在且没有 ERROR/FATAL。旧 gate、失败 STATUS、缺失相位或事后改日志均拒绝。生产 RTL 任何后续修正都需要重新运行完整套件。

## 实际运行 DCP 的父子关系

`synth_identity.json` 只能绑定原生 `synth_1.DIRECTORY/radar_m2_top.dcp` 的实际路径、文件大小和 SHA-256。相同字节的另存发布副本也不能替代实际 run DCP。综合结束后再次导出当前原生输入并与 manifest 比较，输入在运行中变化则不能封印 receipt。

route 在 `reset_run impl_1` 之前重新验证当前输入、仿真、manifest 和实际综合 DCP 的 SHA；不能仅依据 synth 状态字符串或另存副本开始实现。route 后再次复核身份，`route_identity.json` 绑定 synth receipt、实际 `impl_1/radar_m2_top_routed.dcp` 和完整报告成员/哈希。

原生 run 子进程完成后，另外封存真实 `synth_1/runme.log`、`impl_1/runme.log` 的原文副本、大小、SHA-256 及逐条 `CRITICAL WARNING` / `ERROR` 记录；synth receipt 绑定实际 synth log，route receipt 绑定两份实际 run log。报告目录含 `native_run_logs.json` 和对应 `*_runme.log`，运行中变化、原文或副本事后变化、遗漏消息都失败。每个记录保留 run、severity、rule、完整具体文本及重复出现序号，不以总数量作为接受依据。

`ERROR` 和 `REQP-1742` 不允许接受；其余 Critical 仍需 `native_log_accepted` 中完整精确对象、审阅理由和证据 SHA。未解释记录使 `NATIVE_LOG_SIGNOFF=BLOCKED` 并阻断 internal_status。当前外层 launcher Vivado 控制台仍在执行，不能自封存尚未关闭的日志；其完整日志保留给本轮交付前的单独人工审查，不冒充已被 runme 门禁覆盖。

状态分开记录：

| 状态 | 含义 |
|---|---|
| SIMULATION | 当前完整套件是否通过 |
| NATIVE_EXECUTION | 当前阶段原生命令是否实际完成 |
| route NATIVE_COMPLETE | 原生 route 完成且身份/报告封印成立；尚不等于发布资格 |
| internal_status | 当前报告中的时序、PHY、CDC/DRC 与实际 run 日志门禁资格 |
| SOURCE_TIMING / BOARD_ELECTRICAL | 外部真实合同，当前 PENDING |
| DOWNLOAD_CANDIDATE / BOARD_TEST | 当前 NO / NOT_RUN |

当前 route FAIL/RUNNING 不能借历史 PASS receipt 通过。原生执行完成但资格失败时仍保留真实执行状态，不伪造“工具没跑”或“已可下载”。

## 原生约束、PHY、CDC 与时序资格

`cdc_contract.tcl` 固定 7 个握手父对象及精确源/目的位集合：5 mailbox、2 snapshot。build02 原生网表已核实 649 个活动目的位：CMD141、snapshot384、tap64、delay27、delay_report32、cal_report1。CMD 源端另外保留 [133:135]，delay_report 源端另外保留 [0:26]；仅这些逐个证实无功能消费者的源位可在 opt_design 后裁剪。目的位仍严格匹配，不靠降低总数通过。其他位集合变化必须审查并重新构建。cal_ready 两处同步器的物理源允许已证实等价合并的 clock_enable_ref_reg，仍要求唯一直接寄存器 Q，逐层 net segments + leaf pin 查询避免漏过层次边界。

每个真实负载目的 D（含复制对象）必须有有效 8 ns max-delay 路径、有限非负 slack。多位总线还必须具有匹配实例的实际 bus-skew 报告和非负 slack。动态单 bit 校准结果仍检查 max-delay；仅其 inter-bit skew 明确不适用。常量 RECAL 事件要求 req/ack 三级、零目的负载寄存器和源负载零功能消费者，不以说明字符串充当证据。

门禁同时核验固定 I/O、实际时钟周期/波形/master、三组 IDELAY CLK 与 ISERDES CLKDIV 同网、状态直接源寄存器、三级 ASYNC_REG、XPM reset 结构及 counter 无原始异步 CLR/PRE。新增有序 reset 的两专用时钟门、九个 RST/CLR 端点、当前库 clear arc 和 12 个实际 clock-net 延迟见 `RESET_TIMING_CONTRACT.md`。综合阶段 R/R 的有限负估计只记录 PRE_ROUTE，不能成为物理 PASS；最终 route 的真实 native check 必须非负。六个原语 RST 的 min 无 native library check 采用逐对象库弧证明和 null 记录，绝不把 inf/空值填0。其余空对象、NaN/Inf、少端点、无证据缺检查或被例外消失的路径均失败。

最终 STA 要求 setup/hold/pulse-width 余量非负、失败端点为零、有效端点非空；路由须全部完成且零 routing error；无内部漏时钟/无约束端点或 clock loops。REQP-1742 和 DRC Error 直接阻断。自定义 capture RTL 的 CDC-10 不允许 acceptance 豁免。

其他 Critical 默认未解释即阻断。`release_policy.json` 仅能接受完整精确对象记录，必须有审查原因和当前可核对的证据 SHA；陈旧或不存在的接受项也失败。持稳总线的协议接受仍须当前每位物理约束通过，不能靠全域正则或异步 clock-group 隐藏。当前空 acceptance 清单不代表没有 Critical，须等待本轮实际报告逐对象审查。

build03 已实际完成 route，但 routed 的 12 条 CDC-11 使发布保持 BLOCKED；其停 REF 期间 cal_epoch 低事件丢失反例不能豁免。build04 的共享复位合同改为 13 个精确 XPM 父对象、8 个 epoch/目的域共享 owner、FIFO/deser 两个本域 PREQUALIFIED HOLD。原 mailbox/snapshot 内部 XPM 必须不存在；每组 raw source 在该目的域只能有一个同步入口，全部指定 consumer（含 heartbeat 8 位）必须由该 owner 的实际 released 寄存器驱动。原 cal_enable_ref 普通 D 链被删除，状态源从 12 项变为 11 项；649 个 payload 端点与九个原语 RST/CLR 物理门禁保持。

2026-10-13 19:34 完成 build04 映射网表的只读诊断重放。原生综合已完成，原流程因检查器对生成层级和 RTL 输入端口名的假设错误而中止；修正后在同一实际网表上，完整 strict 检查通过。实际 XPM 父层级为 `g_async.u_reset_sync`；重建层级会移除或改名 `src_resetn_in` 等模块端口，因此共享检查改由独立的消费者模块、目的时钟和必需寄存器角色枚举物理 CLR/PRE，再逐端验证唯一 owner Q。选择集合不依赖待证明的 owner fanout。req/ack/sync_ff 仍强制精确三级，649 个数据目的位及 11 路状态仍由各自精确合同另外检查。

| 共享目的域组 | 实际核验的复位/资格消费者 |
|---|---:|
| common_sys | 524 |
| common_rx | 465 |
| command_sys | 148 |
| command_rx | 297 |
| delay_sys | 70 |
| delay_rx | 323 |
| cal_sys | 13 |
| cal_ref | 11 |
| 合计 | 1851 |

该合计含 1850 个寄存器 CLR/PRE 叶端和 1 个 REF supervisor 的 `cal_enable_ref` 资格边界；后者是共享 owner 的直接控制输入，不冒充异步复位引脚。两项 PREQUALIFIED HOLD 另行核验无内部 XPM，且来源均为精确指定的本域寄存器。

证据为 `outputs/m2_closure_20261013_c1_build04/reset_probe/native_probe.log`、`strict_03/constraint_metrics.json` 和 `strict_03/python_qualification.json`。完整原生结果包含 649 个负载端点、7 个握手通道、11 路状态、13 个 XPM 和九个 RST/CLR 的 max/min 检查；Python payload、bus-skew 和 `reset_metrics(stage=synth)` 全部通过。会话仅 read-only 打开项目/综合 run，并于 19:34:14 正常退出。这是 `DIAGNOSTIC_PASS_NOT_BUILD_SEAL`，不能替代修正 recipe 后 fresh build05 的身份封印与实际 routed 资格。

## T6：88 项可重复门禁回归

运行 `python -B -I scripts/m2_closure/test_gates.py`。测试在新建且严格限定为 OS temp 的 `m2_t6_*` fixture 中执行，不打开 Vivado、不改生产设计。最终 recipe 冻结后的最新结果为 **88/88 全部通过（2.234 s）**：`outputs/m2_closure_20261013_c1/T6_gate_tests_10.log`；此前 `_08`、`_09` 和更早 73 项结果均作为历史保留。当前检查器还已完成上述 build04 原生只读网表重放；这些诊断和主机门禁测试均不代替 fresh build05 的当前输入身份与 routed 结果。

| 类别 | 数量 | 覆盖重点 |
|---|---:|---|
| 构建身份 | 21 | 当前基线、源/脚本/设置变化、三个历史helper各自变动、XDC增减、实现清单遗漏、未注册RTL、旧SIM/缺相位/坏日志、实际DCP替换、实际synth日志变化、发布副本冒充、禁止覆盖manifest |
| 原生运行日志 | 8 | 正常两run、缺impl日志、原件/副本变化、遗漏精确消息、ERROR与REQP不可接受、重复Critical逐条保留且不能以数量豁免 |
| 负载与报告资格 | 26 | 正常边界、负max-delay/skew/STA、NaN/不可解析、空集合/少位/少端点、错误skew实例、多bit漏skew、event功能消费者、未完成route、旧PASS、PHY同网数量、不可豁免功能CDC10 |
| Reset物理资格 | 18 | 正常9端点、缺端点/缺min、负removal、NaN或超界传播、false-path、缺库clear证据/少arc、负关门余量、PRE_ROUTE不冒充release、原语缺库证明或伪造0、native检查误标不存在、缺clock leaf或route仍用估计 |
| 共享复位所有权 | 15 | 正常完整8组、缺proof/父XPM/epoch组、旧endpoint-local XPM复活、重复组或raw链、空来源、少consumer/降低期望数量、独立consumer owner、错误时钟、旧D型cal_enable冒充owner、本域HOLD误加raw XPM/跨域 |

这些测试证明门禁对受控输入作出预期决策。它们不证明新 FPGA 设计已经完成综合/布线，不替代当前网表对象、时序报告、当前源 XSim 或实板验证。
