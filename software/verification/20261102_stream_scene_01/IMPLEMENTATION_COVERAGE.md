# M2 逐帧低延迟与室内目标地图：实施覆盖与收口清单

> 最终更新：新版 6213 条记录已全量导出核对，实际跨度 621.224 秒，各数值性能门槛达到，正常停止和再次启动成功；TOP VIEW LIVE 已恢复并获用户画面确认。最终结果见 `RESULT.md` 和 `status.json`。完整 CPU 预算仍 NOT_PROVEN，DP 保留原累计告警和有限观察结论，物理精度/类别准确率未验证。下列按时间保留的中间更新不替代此最终结论。

> 2026-11-02 14:09 更新：新版 `stream620_fixed_01` 已在 6213 帧后正常停止，RUN_TO_RESET、unused_bds=126、reset/Halted confirmed；最终 verified/processed/presented=6213/6213/6213，ready/owners/error/pending_display=0。本次连续预排环停止故障已关闭，详见 `STOP_PREFETCHED_RING_FIX.md`。完整遥测正在导出，全局性能及实际观测跨度尚未完成审核；DP 和完整 CPU 限定不变，不能把此次停止成功提前写成整体无条件 PASS。

> 2026-11-02 14:00 左右更新：原 620 秒试验在 6213 帧验证/处理后正常停止发生 HALT_TIMEOUT，原结果继续为 FAILED。v1.10.1 修复已通过主机回归、原生 PS 构建以及 **91 帧连续模式实物正常停止**：verified/processed/presented=91/91/91，ready/owners/error=0，实际 RUN_TO_RESET 丢弃 119 个未消费预排 BD，确认 reset/Halted 后释放。证据见 `STOP_PREFETCHED_RING_FIX.md` 和 `board_checks/stream_stop_fixed_01/`。随后新版 `stream620_fixed_01` 已显式再次启动，长测尚待完成。DP 原严格 FAIL 保留，独立审计仅支持“作用域内没有观察到新增 DP 错误”，不支持物理零事件或无条件整体验收。下文 103 条源码/ELF 核验对应修复前封存版本；新版 ELF 为 `9f6f371977ccd7316d6e98b2611135c64bd53dc9fc3cf3288f256689ee2f7b46`，构建见 `checks/application_native/stop_fix_02/result.json`。

原覆盖表审计时间：2026-11-02，首次 620 秒连续板测进行期间；随后状态以顶部追加更新为准。此文件是只读源码/已保存证据审计，**不是最终十分钟验收报告**。本次文档更新没有操作 UART、JTAG 或板卡，没有修改生产源码或重跑测试。后续新版长测由主任务补充最终结论。

## 1. 当前可确认的实现

| 用户计划项目 | 生产实现与现有证据 | 当前结论 |
| --- | --- | --- |
| 可恢复旧版本 | 修改前完整基线位于 `/archive/20261102_stream_scene_01/`；由主任务核验 4669 个文件 | 原成功版本已保全；本审计没有重复复制或制作 ZIP |
| CRC 和 PS 计算优化 | 查表 CRC 保留两次校验；校准/TDM 与角度权重预计算；`checks/root_cpu/result.json` 记录旧版与缓存版 JSON、复数立方体和功率图哈希完全一致 | 已实现，主机数值一致性通过；不能把缓存优化称为已独立验证的手工 NEON 优化 |
| 低开销日志 | LIVE 每秒摘要及即时错误；固定 8192 条 RAM 时序记录；停止后分段导出；每次格式化块不超过 400 字节 | 已实现；导出边界与 512 字节 formatter 测试通过；实际串口恶意拥堵不是本轮已完成的实物故障试验 |
| STREAM 与 FINITE 隔离 | 独立 `radar_stream` 生产者；16/64/连续三个严格配置；旧 `radar_session` 拒绝 STREAM 资格；模式/配置切换受空闲和所有权条件限制 | 已实现；保留原 FINITE 回退，未放宽原有限采集资格 |
| 逐帧 DMA 交付 | 128 BD，7 BD/帧，最多预排 18 帧；运行中回收、释放后补充；不覆盖未消费数据 | 已实现；生产驱动+AMD SG 模型覆盖环回绕，16/64 实物逐帧交付已观测 |
| RAW 槽与身份 | 16×512 KiB 槽，FREE/WRITING/VERIFIED/PROCESSING；epoch、运输序号、全局帧号、槽租约；每帧 399360 字节、96 条严格 HSI、两次 CRC | 已实现；过期租约、坏 CRC/HSI、错误元数据、保留所有权有主机测试 |
| 背压和停止 | 积压 8 帧或最老待处理超过 500 ms 受控停采；正常 PL 包边界停止→sensorStop ACK→排空完整帧→DMA HALT/RESET；异常保留证据和租约，不自动重启 | 已实现；主机覆盖正常尾部、过载及异常恢复边界；16/64 实物最终 RAW 队列和所有者均为零 |
| PS/PL 重叠 | 两个 3 MiB DSP 槽位于 0x02600000/0x02900000；一个 PL 引擎；准备 N+1 与计算 N、后处理 N 与计算 N+1 重叠；STREAM 仅允许 PL_BASELINE | 已实现；主机验证重叠、输入不可变、双槽复用、错误时保持所有权；16/64 实物完成全部处理 |
| 显示解耦 | DP 双缓冲和真实描述符 ACK；无 ACK 不阻塞后续采集/处理；数据版本、页面版本、实际首次 ACK 分开；迟到尾帧 ACK 不被停止状态版本覆盖 | 已实现；64 帧无 ACK 的主机试验仍全部发布，迟到第 64 帧 ACK 测试通过；实物 16/64 帧全部获得首次真实 ACK |
| SYS→PS 时基 | 原子 SYS 快照、非零 epoch、ABI/能力校验、流期间 CLEAR 锁定、映射误差界、状态异常拒绝；不把配置 100 ms 当时钟 | 已实现；XSim 和注入时钟主机测试通过；实物记录有逐帧映射及误差；延迟仅指 SYS 首次接受握手到首次 DP ACK |
| 场景处理与轨迹 | `RadarSceneProcessor`、0.35 m 聚类、最多 8 条二维 Kalman 轨迹、3/5 确认、真实 dt、500 ms 预测上限、2 秒实测轨迹 | 已实现；合成序列验证确认、交叉、缺失/过期、停驻、上下文变化；不是已验证可分辨 8 人 |
| 位置与标定边界 | 标定身份绑定阵列/配置/安装；默认单位系数、零偏置并标 UNCALIBRATED；水平有效但高度无效时输出 BEARING ONLY，不填 Z=0 或伪造 XY | 已实现并有测试；实物方向、绝对位置和高度误差仍待标定 |
| 实验类别 | UNKNOWN、MOVING OBJECT、STATIC REFLECTOR、HUMAN?；RULES_V1 使用运动、持续性、尺寸/形态和非刚性代理证据；缺证据/冲突/预测退回 UNKNOWN | 已实现；规则正负合成测试通过；评分不是准确率，未证明人体/生命体识别 |
| 持久反射图与 WALL? | 使用完整横向热图；移动邻域排除；10 秒窗口、250 ms 时间桶；支持率/长度/残差/连通性门限；配置、标定、安装或明显场景变化清除 | 已实现；墙线正例、间歇/短线/断裂负例、地图失效有主机测试；时间支持率是时间桶占比，不是逐帧百分比；不是已测绘真实墙面 |
| 第三页与四页保留 | 第三页等比例 XY、稳定 ID、轨迹、预测、类别依据、数据年龄；其他页保留；11 个生产渲染器主机状态有视觉证据 | 已实现并主机检查；实际屏幕和分类正确性不能由主机图片推定；KEY3 原模块保留且按要求未验证 |
| 原生硬件/软件匹配 | 原生刷新/构建、XSim、综合/布线、含 bit XSA、匹配 Vitis 构建、链接内存布局 | 已通过本轮已有流程；细节见第 3 节，不把构建 PASS 替代实物验收 |

主要源码：`radar_runtime/src/capture/radar_stream.*`、`radar_raw_slots.*`、`hal/radar_dma.*`、`radar_time_bridge.*`、`processing/radar_stream_scan.*`、`radar_scene.*`、`radar_spatial.*`、`display/radar_scope_view.*` 和 `app/main.c`。

## 2. 已有板上观测与未关闭门槛

下表直接读取 `board_checks/stream16_01/timing_audit.json` 和 `board_checks/stream64_01/timing_audit.json`，未裁剪帧。**两份原始审计当前均为 FAIL，原因为最终 DP sticky 位非零，不能将下表改写成整体验收 PASS。**

| 指标 | 16 帧 | 64 帧 |
| --- | ---: | ---: |
| 有效交付 / 处理 / 首次真实 ACK | 16 / 16 / 16 | 64 / 64 / 64 |
| 有效采集、处理、新画面率 | 9.580 Hz | 9.903 Hz |
| 更新间隔 P95 / 最大 | 133.482 / 133.482 ms | 100.799 / 133.481 ms |
| SYS 首接受→首次 DP ACK，含映射误差上界 P95 | 173.157 ms | 147.723 ms |
| 已记录内核 CPU P95 / 最大 | 57.093 / 57.093 ms | 57.521 / 57.833 ms |
| 最终 ready / owners | 0 / 0 | 0 / 0 |
| 新采集 CRC / HSI / DMA / metadata 错误记录 | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| 最终 DP 标志 | 0x8 | 0x8 |

DP 的 `0x8` 是 `radar_display_error_flags()` 的 TX FIFO 历史 sticky 位，**不是 8 次错误**。64 帧前后 `display_diag_before.txt` / `display_diag.txt` 保存相同的 ISR/EISR=0/0、TX_fifo_latched=1、TX_fifo_observations=1、寄存器 0x0110=0。`TX_fifo_observations` 仅在相关 ISR 顺带观测时增加，不能直接等同于所有 FIFO 事件计数。其历史启动作用域、寄存器语义以及能否据此证明“无新增”正在独立审计；这里保留原 FAIL 与 DP 未决状态，既不抹去启动事件，也不把历史标志自动归因于 STREAM。

十分钟门槛尚不能由 16/64 帧结果关闭。620 秒连续四页试验正在由主任务运行，必须等完整导出后核验：实际观测跨度≥600 秒、全部帧/运输序号/租约、全部页面及唯一首次 ACK、更新间隔最大值、含误差延迟上界、无持续队列增长、无新增作用域内错误、正常停止尾帧及所有权释放。最终长测报告优先于本文的中间状态。

## 3. 已封存构建与离线证据

- `checks/stream_final_03/summary.json`：生产软件+AMD 驱动主机模型 PASS，UBSan 启用，4 个 C 测试和 2 个 profile 测试，运行期间源码稳定。该模型不证明真实 DDR/cache/总线时序。
- `checks/stream_ack_export/summary.json`：生产 coordinator 64 帧、迟到尾帧 ACK、无 ACK 不阻塞、完整导出、CRC 拒绝和错误租约保留的 6 项定向测试 PASS。测试时未改生产源码。
- `checks/root_cpu/result.json`：CRC 及旧/缓存空间结果逐位一致，HOST ONLY。
- `checks/scene_checks.json`、`checks/scene_visual_review.json`：生产场景算法合成测试及 11 种生产渲染状态；无现场分类真值，无 KEY3 验证。
- `checks/time_bridge_rtl/20261102_130723/result.json`：原生 XSim 的 CSR/元数据并发、快照、epoch、清除冲突、FIFO 满及序号等 PASS；不是全系统实时吞吐仿真。
- `checks/time_bridge_host/20261102_131058/result.json`：不可信时钟、epoch/复位、漂移/倒退、映射过期、误差界等注入测试 PASS。
- `checks/native_build/route_timebridge_01/result.json`：本轮布线源前后相同，原生流程 PASS；`route_timing.rpt` WNS=0.627 ns、WHS=0.011 ns，已设定约束满足；`route_drc.rpt` 保留 99 条 Warning（流水寄存器建议、BRAM advisory、无可布线负载等），没有将警告删去或宣称 DRC 零消息。
- `checks/platform_native/matched_stream_03/result.json`：匹配平台/应用原生构建 PASS，普通内存末端 0x001BA3C0<0x00200000；BD、RX、RAW、DP、DSP 保留区互不重叠。封存 ELF SHA-256：`ae41c21136bf18c424a5979dcc3c9cadde06db16aa89d7848133f6b08f35540d`。
- 本次只读收口重新比对 `built_application_source.json` 的 **103 个条目**，与当前 `radar_runtime/src/` 一致；当前 ELF 哈希也与上述构建一致。没有触发重编译。

## 4. 明确未完成或不能宣称完成的项目

1. **完整 CPU≤80 ms/帧的计量尚未闭合。** `RadarStreamRecord.cpu_us` 覆盖复制/首 CRC+HSI、准备/第二 CRC、PL 驱动调用、导入/测角/场景/发布、首个真实 ACK 对应渲染至排队的已计时工作；异步等待 PL 的墙钟间隔不应算成该 CPU 工作。另有累计 `health_cpu_us`，没有纳入每帧值。
2. **仍有未归属 CPU 工作。** DMA 收割/RX cache invalidate、时间元数据与映射、部分入队/任务准入及 HSI、RECEIVE 中 FLAGS 读取、主循环快照/复制、DP 轮询与 ACK 调度、UART/命令以及控制路径等不全在帧内计时中。显示排队时间戳之后的描述符提交尾部、未成为首次 ACK 的绘制也不完整。中断在已计时切片内的时间已隐含，不应重复加算；切片外中断未单独计量。
3. **当前可审计 CPU 下界：** 正常排空、最终 ACK 后，`(sum(全部导出帧 cpu_us)+health_cpu_us)/N` 是该完整试验“已计入工作”的摊销下界；64 帧约 **55.502 ms/帧**。它包括启动/停止期间 health，不是稳态全 CPU，也不是逐帧 P95。低于 80 ms 不能证明总预算通过；100 ms 配置周期或实测约 10 Hz 同样不能证明还有 20 ms CPU 余量。
4. **慢速波形仅生成候选。** `candidates/slow_idle100/README.md` 明确 `NOT_RUNTIME_READY`；未生成可选 runtime header、未改变默认配置、未上板。生产空间处理仍为 31 µs chirp；候选需 124 µs 和 `[0,124,248] µs` TDM，必须联合更新几何/补偿/速度刻度和门限后单独验证，禁止直接发送候选 cfg。
5. **未完成物理标定或分类准确率验证。** 未知位置/左右方向/距离偏置/高度误差仍未校准；人员、椅子、停驻/遮挡及墙线缺少独立完整序列与人工真值。合成 HUMAN?/WALL? 正例不是真实人体或墙体检测证明，当前自然场景也不能给出准确率。
6. **极端物理故障边界未全部上板注入。** 主机与 XSim 已覆盖相关异常语义，但没有把它们冒称为实际拔时钟、真实 CRC 错包、串口饱和、DDR 故障或 DP 断链的实物试验；这些不应为了收口额外打断当前稳定长测。
7. **KEY3 明确未验证；不制作压缩包。** 维持用户边界。

若将来必须关闭完整 CPU 门槛，最小生产变更是在非嵌套的主循环 capture / processing / UI / console-dispatch 服务边界计时，按真实运输区间取快照，覆盖 continue/错误路径；外层总计与已有内部 `cpu_us` 分别报告，避免重叠相加。需要另一个源码/ELF 身份及相应板测，不能在当前 sealed 版本报告上补造数字。

## 5. 不改 sealed 源码的必要收口动作

1. 完成正在运行的 620 秒试验后，只分析已有日志和全部 RAM 导出；保留不裁剪时序、完整输入哈希、最终 ACK 与队列/租约计数，不重跑已通过的短测。
2. 独立 DP 作用域审计保留原 FAIL、原启动事件和原始寄存器证据；先确认寄存器/ISR 观测能力，再明确能证明“无新增错误”还是仅“无新增已观察错误”。由主任务链接新的审计，不原地伪造旧 PASS。
3. 将最终长测报告、DP 审计以及上述 CPU/慢速波形/标定待项写入工程当前状态说明。当前 103 源条目与 ELF 已核对，无需再做一次全量构建或重复全部 host/XSim。

本清单可支持的结论是：**逐帧采集、PL 计算、时间身份、场景轨迹和实验标签已接入生产路径；短批次实物吞吐/延迟达到了数值目标，最终长测与 DP 作用域尚待关闭。完整 CPU 预算、慢速候选运行和物理分类/定位精度仍为未完成项。**
