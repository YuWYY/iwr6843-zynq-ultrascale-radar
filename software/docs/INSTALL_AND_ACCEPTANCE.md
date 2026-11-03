# SW3.2 / SW4.1：替换、回退与验收

> 本文是早期源码包安装说明。当前主线已原位完成四页显示和3TX4RX，当前入口见 [CURRENT_RUNTIME.md](CURRENT_RUNTIME.md)。无需重新安装旧源码包；下文14源文件等数量仅指历史版本。

## 1. 安装范围

保留上传工程的 `radar_system_m2` Platform、A53-0 standalone domain、BSP、XSA、bit、FSBL 和下载配置。只替换 `radar_runtime/src`。不要把整个交付包当成新 Vitis workspace 导入；包根 CMake 是主机回归入口。

上传基线的本机路径为 `/software\radar_runtime`。实际位置不同时，下面仅替换这个路径参数。不要移动或修改现有 `app.yaml` 中的 domain 路径；它由你的现有工作区决定。

## 2. 推荐：带备份的完整目录替换

停止应用调试，关闭占用 src/build 的程序。安装本身不操作目标板；旧应用仍可能在硬件上运行，因此后续下载必须按第 5 节重新建立已知状态。

在解压后的完整交付包根目录打开 PowerShell：

```powershell
python tools/install_sources.py --application "/software\radar_runtime"
```

检查预览中的 `operation=FULL_SRC_TREE_REPLACEMENT_NOT_OVERLAY` 和 `different_local_profile=false`。确认目标是应用而非 Platform 后：

```powershell
python tools/install_sources.py --application "/software\radar_runtime" --apply --clean-build
```

脚本将旧 src 及应用 build 移至工作区外层备份目录，例如：

```text
/software\_radar_sw41_backups\radar_runtime_<时间_随机后缀>\
    src\
    previous_build\
    vitis-comp.json
    INSTALL.json
```

新活动目录只保留分类后的生产文件，以及本机 `app.yaml` 和 `.clangd`。`vitis-comp.json`、`_ide`、其他应用文件及整个平台都不改。备份不位于活动应用目录，避免被 IDE 当作第二套源文件。

有文件占用导致替换失败时，脚本尝试回滚；若回滚也失败，会给出备份位置，**先恢复文件再构建**。不要在失败状态下继续点击下载。

若你已经生成过不同的正式 `radar_capture_profile_data.h`，脚本会拒绝覆盖。先审阅其与本版生成器/运输合同的兼容性；只有明确决定归档旧 profile 并替换时才使用 `--replace-profile`。本次上传的默认 profile 与交付包相同，正常不需要此选项。

## 3. 手动安装（不运行 Python）

先把整个旧 `src` 移到 `radar_runtime` 目录外的备份位置，再把本包 `radar_runtime/src` 整体复制回来。将备份中的 `app.yaml`、`.clangd` 原样放回新 src 根目录。把旧应用 `build` 也移出活动应用，保留作追溯。

**不能只覆盖同名文件。** 分类后的新 `app/main.c` 与旧平铺 `src/main.c` 会同时存在；本版 CMake 会主动报错阻止这种安装，而不是悄悄编译两份。

不要复制旧 `.cache`、`.compile_commands`、`compile_commands.json` 或旧 `lscript.ld` / `lscript_sw0.ld` 到新 src。唯一生效脚本：`src/linker/lscript_sw4.ld`，原布局不变。

## 4. 原生 Vitis 构建

在原工作区选择 `radar_runtime`，执行 Clean/Build。检查设置仍指向 `radar_system_m2`、`standalone_psu_cortexa53_0`、`psu_cortexa53_0`。

源码列表由 `src/UserConfig.cmake` 的 14 个显式路径管理；include 搜索路径为 app/hal/capture/config。无需在 GUI 中逐个重新添加文件。IDE 展示未更新时先刷新/重新打开组件，不要重建一个名称相同的新应用。

本版保留本机 `src/app.yaml`，CMake 只改生产源列表、include 和链接脚本相对位置。平台静态库与标准库依赖沿用上传基线，不使用主机测试桩链接板卡。

构建成功后，使用实际生成的 ELF 路径运行：

```powershell
python tools/check_build.py --elf "/software\radar_runtime\build\radar_runtime.elf" --output "build_check_sw41.json"
```

也可用 `--xsa <实际路径> --bit <实际路径>` 额外核对冻结硬件文件哈希。该工具检查 AArch64 ELF、版本字串、布局符号、NOLOAD/堆/栈与重叠；**它不读取板上已加载 bit 的哈希，也不证明板测完成**。保留新 ELF、map、构建日志以及生成的 JSON。

原 ELF 的版本是 SW4-v1.0，检查器会拒绝它；不要因为旧 ELF 仍存在就认为已经构建了新应用。

## 5. 下载与冷启动

沿用此前能成功启动的完整目标初始化流程：停止旧测试、完整目标复位/掉电重启、加载冻结 PL 配置、执行匹配的 PS 初始化/FSBL、启动新 A53 应用。不要在 DMA 仍可能访问 DDR 时只重新下载/运行应用，让初始化代码改写在途描述符。

此版没有改变 PMUFW/FSBL 加载策略，现有 PMU-FW 提示不作为本版 HALT 根因结论。启动后等应用实际输出：

```text
Version: M2-SW4.1-v1.1
[SW3.2] ...
[SW4.1] capture preflight ...
```

再发送控制台命令。FSBL 出现但应用未 READY 时输入 `dma` 或 `progress`，不等于应用已接收指令。

## 6. 第一轮：只验 SW3.2，不启动雷达

雷达 IO 保持 SAFE；不要先 connect。逐条输入、逐条等待：

```text
progress
dma init
dma qualify10
dma status
progress
capture preflight
```

`dma qualify10` 是 10 次 RUN→RESET，整批末尾才签发一次许可。观察：

```text
RESULT: PASS SW3 RUN_TO_RESET_ONLY 10/10
[DMA] state=READY ... reset_confirmed=1 ... lease=0
free/pre/hw/post=128/0/0/0
halt=NOT_RUN (0 us)
[QUAL] capture_entry=1 kind=RUN_RESET reason=VALID
       issued_epoch=<E> current_epoch=<E> reset_serial=<N>/<N> one_use=1
```

`E`、`N` 以本次操作次数为准，不要求固定为某个数字。`full_control_qualified=0` 与 `run_halt_reset=0` 可以保持，因为没有执行旧的 HALT 诊断。`max_poll_gap` 不再包含用户等待几分钟的间隔，但不预设板上应为某个微秒值。

若批次中途失败，不能使用前几次 PASS：`capture_entry` 应为 0。HALT_TIMEOUT 且满足无生产者安全条件时，可按提示授权一次 `dma reset`；RESET_LOCKED 必须完整目标初始化。人工 reset 后还需要重新 `dma qualify`，不会自动签发许可。

不要默认运行 `dma check10`。它保留旧无输入 RUN→HALT→RESET 诊断；这项结果尚未通过，不等于新版资格测试失败。

建议首次交付验收为 **3 次完整冷启动，每次 qualify10**，并保存所有原始串口文本。这是待完成的板测目标，不是本包宣称已完成的结果。

## 7. SW2 与正式 profile

在实际雷达供电、共地、控制 IO 电压、PGOOD、nERR 和跳线配置核验前，不执行 `confirm io`。电压调整断电操作；软件确认命令不测量电压。实际固件必须符合审查记录的非自动发射/可受控停止要求。

硬件确认后依次运行并等待：

```text
status
profile
confirm io
connect
query
check10
reset3
```

`check10` 是 UART 查询批次，`dma qualify10` 是 DMA 测试，两者不同。`reset3` 会真实复位雷达。遇到 FAULT/REFUSE，停止后续步骤，保存返回，不以人工填写通过计数替代板测。

用实际 bin、完整 cfg 和 `docs/capture_review.template.json` 补全审查记录，再生成正式配置（不要把测试 profile 复制到生产目录）：

```powershell
python tools/generate_capture_profile.py --firmware actual_radar.bin --cfg actual_radar.cfg --review capture_review.json --output "/software\radar_runtime\src\config\radar_capture_profile_data.h" --force
```

生成器不烧写雷达 Flash、不验证模拟波形、不开始 RF。实际固件烧写和输出合同核对需要单独完成。修改 profile 后重新原生构建、完整启动，并重做本次启动的 SW3/SW2 验证；旧启动的计数不会继承。

## 8. SW4.1 单次真实采集

当前 profile 合格、当前雷达版本查询匹配、SW2 两组验证通过、DMA 许可有效后：

```text
capture profile
capture confirm <输出中的完整profile_id>
capture preflight
capture once
capture status
```

preflight 九个 gate 都 PASS 才继续。`capture once` 是明确授权动作，之后会按完整会话发送 sensorStop/config/sensorStart；profile confirm 本身不会发射。

成功条件包括真实完整运输包和严格收尾。即使 CPU 已复制或 CRC 已算出，只要正常 HALT/RESET/RECOVER/最终交接失败，结果仍无效。

成功后：

```text
capture export
```

保存串口文本文件后，在主机执行：

```powershell
python tools/decode_capture.py serial_capture.log --out captures\capture_001
```

得到 `.bin` 与 `.json`。CRC 证明该次导出数据一致性，不证明 ADC I/Q、RX、chirp/frame 语义正确。115200 串口为低速调试出口；先做一包，不把导出耗时当作实时采集性能。

## 9. 第二次采集与故障恢复

成功会话结束后许可已经消费且 DMA epoch 已改变；保留结果未清除也会阻止覆盖。导出完成后：

```text
capture clear
dma qualify
connect
query
capture preflight
capture once
```

正常成功收尾的 SW2 已 SAFE；重新 connect/query 刷新在线控制状态。本次启动的 check10/reset3 历史可保留，但新的雷达 reset epoch 仍要求有效当前版本。故障恢复会撤销 capture confirm，必须补齐相应验证与确认。

会话失败时先 `capture status`、`dma status`、`dma trace`。`capture recover` 是显式授权的 NRST 保持、一次 DMA reset、确认静止后的 PL RECOVER；不是通用强制清零。控制失联、无法确认供电、RESET_LOCKED 等条件下程序会拒绝，需按提示完整初始化。恢复成功不能把失败数据升级为 PASS，也不能恢复已消费的许可。

## 10. 回退

停止调试，将新 src 移出组件，把备份的旧 src 原样放回；需要时恢复备份的应用 build，或直接清理后重新构建旧源码。Platform、BSP、XSA、bit 一直未修改。回退后仍按完整目标初始化启动，不能热切换正在拥有 DMA 缓冲区的应用。
