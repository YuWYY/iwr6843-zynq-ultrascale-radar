# M2 验证准备结果 — 20261029_sw3_sw4_prepare_01

**本机软件准备完成；2026-10-29 20:23已实际完成 SW3 无输入 RUN→RESET 单次加十次验证，11/11 PASS。但本次 Vitis 启动的 FSBL 超时复现，完整启动验收不通过；SW4尚未执行。**

最新结论及原始证据见 [SW3实板结果](/software/verification/20261029_sw3_sw4_prepare_01/reports/SW3_BOARD_RESULT_20261029.md)。最终 DMA READY、128个BD全空闲、lease=0、epoch/reset_serial=12/12资格VALID；系统SAFE、IO=0、故障为零，COM5已释放。实际启动Build ID与预期一致，38个源/ELF/bit输入哈希保持一致。下一步优先排查启动异常，再继续SW2及完整启动重复性。

19:27历史诊断：`status`、`phy status`、`progress`均收到完整回复；当时尚未执行SW3。该时点记录保留于 [历史串口记录](/software/verification/20261029_sw3_sw4_prepare_01/board_sessions/serial_diag_20261029_192736/transcript.txt) 和 [解释记录](/software/verification/20261029_sw3_sw4_prepare_01/board_sessions/serial_diag_20261029_192736/interpretation.json)，不能替代20:23新启动的结果。

2026-10-29用户补充：F20 + IWR6843ISK、出厂预烧固件；所给8个LVDS管脚已全部匹配当前XDC和参考连接器编号。见 [板卡资料补充](/software/verification/20261029_sw3_sw4_prepare_01/reports/BOARD_INPUT_UPDATE_20261029.md)。无需提供当初自行烧录文件；实际出厂固件身份及cfg仍待追溯，不影响先完成P0/P1。

## 当前使用的程序

- ELF：[radar_runtime.elf](/software/radar_runtime/build/radar_runtime.elf)，807872 字节。
- SHA-256：`b3c578564130105fbb773bc1b99d4f107434e95923e94e181ff1c1c81a10bdc6`。
- 启动应打印 `Build: 20261029_sw3_sw4_prepare_01`；软件协议版本保持 `M2-SW4.1-v1.1`，GPIO ABI 保持 `0x52430101`。
- XSA：`ef64d93c940adef7220ce498d7e3223169c825adc951b67a0ea5724df004961f`。
- bit：`dcb3d84a625f6ae5b112f89f9c85adfa1f989aa378540362b943b411afc1b204`。
- FSBL 沿用当前平台版本；用户20:22在Vitis重新初始化/下载目标。本轮代理未更改启动配置或执行目标下载，但实际执行了授权的DMA启动/复位验证。

## 已完成修改

1. 未执行 UART query 时显示 `last=NOT_RUN`；真实查询结果、结果枚举和 ABI 不变。
2. 新增 `phy status`：通过现有 mailbox 发出 READ 命令，固定两遍读取12个状态页；输出是否一致、非原子观察范围和保留的 tap 快照身份。合法未就绪值显示 N/A，其他读取错误不发布部分数据。SW2/SW3作业、SW4所有权、mailbox待处理/失同步或未验证时拒绝读取。它不配置 IO、不校准、不请求新快照、不复位或启动 RF；READ 命令本身仍包含 GPIO MMIO 请求写入。
3. 修复主机解码器尾部截断导出可能回退到旧成功结果的问题。旧代码已复现，修复后拒绝缺 END 的末尾导出；完整重导出仍可显式选择。
4. 恢复原 SW41 包的工具、测试与说明，修正 profile 生成器名称/输出路径。没有覆盖导入第二套生产源码，没有运行工程安装器。

完整差异见 [source_changes.diff](/software/verification/20261029_sw3_sw4_prepare_01/reports/source_changes.diff)。生产 profile 的参数逐项保持原样；新增 Build ID 不改变协议。

`phy status` 读取尚未就绪的baseline/tap页时，每次最多增加6次预期 `remote_rejects` 并显示N/A；此时不能仅凭累计拒绝数超过原启动自检的2判断故障。

## 本轮实际验证

| 项目 | 结果 | 证据 |
|---|---|---|
| C 主机状态机/驱动模型回归 | 60组 PASS，含8组新增诊断测试 | [normal日志](/software/verification/20261029_sw3_sw4_prepare_01/logs/host_normal_tests.log) |
| UBSan C 回归 | 60组 PASS，无未解释未定义行为报告 | [UBSan日志](/software/verification/20261029_sw3_sw4_prepare_01/logs/host_ubsan_tests.log) |
| Python工具回归 | normal/UBSan环境各31项 PASS，最终无SKIP | [normal补齐日志](/software/verification/20261029_sw3_sw4_prepare_01/logs/host_normal_tools_completed.log)、[UBSan补齐日志](/software/verification/20261029_sw3_sw4_prepare_01/logs/host_ubsan_tools_completed.log) |
| 128KiB C导出→Python重组 | 模型数据逐字节、长度、CRC、元数据通过 | 上述工具测试 `test_real_production_export_roundtrip`；不是雷达数据 |
| Vitis 2025.1原生应用构建 | 14个C单元全新编译、链接及size PASS，0编译警告 | [原生日志](/software/verification/20261029_sw3_sw4_prepare_01/logs/native_vitis_app_build_attempt02.log) |
| ELF布局与当前硬件身份 | PASS；BD/RX/CPU结果区域互不重叠 | [布局](/software/verification/20261029_sw3_sw4_prepare_01/reports/native_elf_layout.json)、[身份](/software/verification/20261029_sw3_sw4_prepare_01/reports/native_elf_identity.json) |
| 保护文件 | 39项前后SHA一致；XSA内嵌bit与下载副本一致 | [变更审核](/software/verification/20261029_sw3_sw4_prepare_01/reports/change_audit.json) |
| 当前控制台诊断 | 完整启动输出及状态可读，SW0/SW1报告PASS | 最新串口记录；不等于完整启动流程无错误 |
| 当前启动SW2查询 | NOT_RUN | 本次未开启雷达UART或NRST |
| SW3无输入RUN_RESET资格 | 1+10共11/11 PASS，READY、128个BD空闲、资格VALID | SW3实板结果与完整原始串口 |
| 完整启动流程 | FAIL：20:23:21 FSBL超时复现，随后仍下载运行应用 | Vitis日志保留重复启动记录，因果未确定；不得计入三次干净启动 |
| 本轮SW4真实采集 | BLOCKED，NOT_RUN | [待项](/software/verification/20261029_sw3_sw4_prepare_01/reports/blockers.json) |

保留的过程问题：首次主机工具回归因便携CMake不在PATH跳过1项，已修正子进程PATH并补跑全部31项；首次原生构建编译/链接完成，但post-link的size工具不在PATH，整体返回非零。第二次在进程内设置原生工具PATH后重新编译全部14个单元并通过。失败日志没有删除或覆盖，未更改系统PATH。

## 用户下一步

1. 先核查本次重复启动记录与FSBL退出超时，保留单次完整的Vitis/FSBL/串口证据，不以应用启动成功覆盖初始化错误。
2. 建立三次无错误完整启动记录，其中至少一次正常重新上电；新的完整启动会清除本次SW3资格。
3. 在后续正常启动上复测SW2和SW3，逐步操作见 [BOARD_RUNBOOK.md](/software/verification/20261029_sw3_sw4_prepare_01/reports/BOARD_RUNBOOK.md)。当前SW3单项已经完成，无需在同一次启动上反复测试。

本轮没有实际 bin/cfg 文件或完整的时钟、电气、输入时序证据；[待审模板](/software/verification/20261029_sw3_sw4_prepare_01/reports/capture_review.pending.json) 的八项审批均未通过，没有用主机测试夹具生成生产profile。SW4仍会按原设计拒绝启动。完成资料审查、新profile构建及当前启动资格后，才执行首包和两次复现；采集步骤已列在执行单P3。

## 重现与恢复

本机完整验证入口（包含清理并重建应用，运行前不要同时在IDE构建）：

```powershell
python -B /software/verification/20261029_sw3_sw4_prepare_01/scripts/verify_local.py --stage all
```

该入口不连接JTAG或串口。使用工作区内的便携Linux CMake与WSL GCC运行主机测试，再使用现有Vitis CMake/Arm GCC和当前BSP原生构建；不安装系统工具。每次重跑保留独立日志，新的ELF身份以重跑结果为准。

修改前50个文件已复制到本轮 `baseline/` 并逐个校验，位置见 [baseline_manifest.json](/software/verification/20261029_sw3_sw4_prepare_01/baseline_manifest.json)。恢复时按清单恢复所需源文件和对应ELF，先保留当前修改；不要只恢复部分源码却继续使用另一版本ELF。FPGA文件本轮未改。

原始工具包来源及哈希、恢复成员清单见 [restored_from_package.json](/software/verification/20261029_sw3_sw4_prepare_01/restored_from_package.json)。本轮没有生成交付压缩包。
