# 软件工程（radar_runtime）

Zynq UltraScale+ Cortex-A53 裸机应用，配套硬件见仓库根目录 `fpga/`。

## 目录

| 路径 | 内容 |
|---|---|
| `src/app/` | `main` 主循环、控制台、按键、`radar_autoboot` 自动启动状态机 |
| `src/capture/` | 采集会话、STREAM 引擎、profile、RAW 槽租约、导出、CRC32 |
| `src/hal/` | GPIO 控制 ABI、DMA、内存、时钟、DSP 驱动、时间桥 |
| `src/processing/` | 距离/空间/场景/反射处理、新鲜度与时间统计 |
| `src/display/` | DisplayPort 初始化、四页渲染、视图缓存、中文字库 |
| `src/config/` | 合同常量、几何参数、生成的 profile、硬件身份头 |
| `src/linker/` | `lscript_sw4.ld` |
| `docs/` | 运行期文档，先读 `docs/CURRENT_RUNTIME.md` |
| `verification/` | 各里程碑的交付记录与接口合同（仅文本） |
| `tools/` | 配置生成、串口解码、结果比对、构建检查 |
| `tests/` | 主机单测、BSP 桩与厂商模型适配 |

## 构建

Vitis 2025.1，AArch64 standalone：

1. 用 `fpga/` 导出的 XSA 建立平台（BSP）；
2. 以 `src/CMakeLists.txt` 建立应用（或把 `src/` 加入已有应用）；
3. 链接脚本使用 `src/linker/lscript_sw4.ld`；
4. 生成 `radar_runtime.elf`。

应用启动时会核对 `src/config/radar_dsp_hw_identity.h` 中的 XSA/bit 哈希与
`RADAR_EXPECTED_ABI`，不匹配则拒绝启用 PL 计算链。

## 运行

默认 SAFE；`scope live` 走完整准入流程后启动雷达。常用命令见
`docs/CURRENT_RUNTIME.md`，自动启动流程见
`verification/20261103_autoboot_01/AUTOBOOT_IMPLEMENTATION.md`。
