"""Assemble traceable review documents after the frozen native build.

Does not change design inputs or qualify a bitstream. Native reports remain the
authority; the explicitly known PHY/CDC/source-timing gates remain blocked.
"""
from pathlib import Path
import csv
import hashlib
import json
import shutil
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[2]
delivery = root / 'delivery/20261012_171114'
build = root / 'outputs/m2_real_20261012_180512'
assert (build / 'synth_STATUS.txt').read_text().startswith('PASS synth')
assert (build / 'route_STATUS.txt').read_text().startswith('PASS route')
assert '16466' in (build / 'reports/route_status.rpt').read_text()
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()

status = {
    'review_date': '2026-10-13',
    'integration_build_id': '20261012_171114',
    'native_build_id': '20261012_180512',
    'overall': 'NOT_DOWNLOAD_READY',
    'highest_build_stage': 'ROUTED_PROVISIONAL',
    'audit': 'PASS', 'wiring': 'PASS', 'protected_ip_changes': 0,
    'simulation': {'status': 'PASS', 'testbenches': 10, 'runs': 14,
                   'gate': 'SIM_PASS.json', 'suite': 'xsim_final_02'},
    'bd_validate': 'PASS', 'fresh_global_synthesis': 'PASS',
    'native_route_execution': 'PASS',
    'route': {'routable_nets': 16466, 'fully_routed_nets': 16466, 'routing_errors': 0},
    'loaded_constraint_timing': {'WNS_ns': 0.804, 'WHS_ns': 0.011, 'WPWS_ns': 0.0,
                                 'setup_failing_endpoints': 0, 'hold_failing_endpoints': 0},
    'constraint_object_checks': {'status': 'PASS', 'payload_endpoints': 585,
                                  'mailboxes': 2, 'snapshots': 2, 'physical_ports': 16},
    'qualification': {'PHY': 'BLOCKED_REQP_1742', 'CDC': 'BLOCKED_CUSTOM_RTL',
                       'external_input_timing': 'PENDING', 'board_electrical': 'PENDING',
                       'radar_firmware_contract': 'PENDING'},
    'native_zip_reopen': 'PENDING',
    'bit_generated': False, 'xsa_generated': False, 'hardware_actions': False,
}
reopen = delivery / 'package_reopen_02/reopen_status.txt'
if reopen.is_file() and reopen.read_text().startswith('PASS'):
    status['native_zip_reopen'] = 'PASS'
(delivery / 'status.json').write_text(json.dumps(status, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
(build / 'STATUS.json').write_text(json.dumps(status, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')

def file_paths(path):
    return {n.attrib['Path'] for n in ET.parse(path).iter('File') if 'Path' in n.attrib}
old = file_paths(delivery / 'before/Radar_XCZU2CG_M2.xpr')
new = file_paths(root / 'Radar_XCZU2CG_M2.xpr')
with (delivery / 'retained_and_removed.tsv').open('w', encoding='utf-8', newline='') as f:
    w = csv.writer(f, delimiter='\t')
    w.writerow(['project_reference', 'action', 'disk_treatment'])
    for name in sorted(old | new):
        action = 'RETAINED' if name in old and name in new else 'ADDED' if name in new else 'REMOVED_REFERENCE'
        treatment = 'Current input' if name in new else 'Preserved in external baseline.zip; not an active build input'
        w.writerow([name, action, treatment])
shutil.copyfile(delivery / 'after/addresses.tsv', delivery / 'address_map.tsv')
shutil.copyfile(root / 'docs/m2_real/source_timing_budget.md', delivery / 'timing_budget.md')
source_hashes = json.loads((delivery / 'current_source_hashes.json').read_text())
with (delivery / 'source_manifest.tsv').open('w', encoding='utf-8', newline='') as f:
    w = csv.writer(f, delimiter='\t'); w.writerow(['project_relative_path', 'sha256'])
    w.writerows(sorted(source_hashes.items()))

# Keep complete native run logs outside .runs so the portable archive retains
# evidence without importing stale run trees as live project dependencies.
for run in ['synth_1', 'impl_1']:
    target = delivery / 'build_logs' / run
    target.mkdir(parents=True, exist_ok=True)
    for p in (root / 'Radar_XCZU2CG_M2.runs' / run).iterdir():
        if p.is_file() and p.suffix.lower() in {'.log', '.rpt', '.tcl', '.vds', '.vdi', '.jou'}:
            shutil.copyfile(p, target / p.name)

(delivery / 'constraint_coverage.md').write_text('''# 当前 routed 约束覆盖

证据：`outputs/m2_real_20261012_180512/reports/constraints_route/` 及 `route_attempt.log`。

| 项目 | 实际结果 |
|---|---|
| 固定物理 IO | 16 个 PACKAGE_PIN/IOSTANDARD 均与约定相同 |
| BITCLK / RX | RADAR_BCLK300，真实 BUFGCE_DIV /4 派生 RADAR_RX75；工具量化后 3.333 / 13.332 ns |
| 参考域 | Wizard 原生 100 MHz 输入、300 MHz 输出，与 BITCLK 独立 |
| u_cmd_mb | 141→141 个端点，8 ns max-delay，routed 最差余量 +6.987 ns |
| u_delay_mb | 28→28 个端点，8 ns max-delay，routed 最差余量 +7.319 ns |
| u_snapshot | 384→384 个端点，8 ns max-delay，routed 最差余量 +6.971 ns |
| u_tap_snapshot | 32→32 个端点，8 ns max-delay，routed 最差余量 +7.206 ns |
| 单比特首级 D / req-ack 首级 D | 22 / 8 个精确匹配 |
| reset-release 异步断言端点 | 260 个，其中 released_reg 为 13；其余级保持计时 |
| 外部输入 min/max | 未签收；模板未加载，无零延迟占位 |

585 个有效 payload 端点和正时序余量不豁免跨域结构问题。详细 CDC 分类、bus-skew、异常覆盖和物理原语规则见 `cdc_review.md`、`phy_rule_review.md`。未使用全域异步 clock groups、CLOCK_DEDICATED_ROUTE=FALSE 或 DRC 降级。
''', encoding='utf-8')

(delivery / 'build_attempts.md').write_text('''# 本轮构建身份与历史证据

最终可信统一仿真是 `m2_real_20261012_171114/xsim_final_02`，原生综合/实现是 `m2_real_20261012_180512`。输入 153 个文件已在各阶段前后比较哈希。早期尝试仅用于故障诊断，不代表最终配置验证。

| 尝试 | 结果与处理 |
|---|---|
| 初始集成 | 修复 update_module_reference 对象类型；BD 原生成员计数按 18（含差分口）校验 |
| 173710 | 主机 Python 被 Vivado 环境污染，启动前失败；改用 python -I |
| 173740 | 综合完成但 XDC 内 Tcl 控制流未被读取；改为纯声明 XDC、独立全 Tcl 核验 |
| constraint_smoke | OLD_DIAGNOSTIC DCP 仅验证 Tcl 规则，明确排除交付 ZIP |
| 175423 | 新综合及 585 端点检查通过；发现 REF 重复时钟声明，保留厂商原生声明 |
| 180035 | 新综合通过；发现 SSTL12 不支持显式 DIFF_TERM=FALSE 属性，移除该无效属性 |
| 180512 synth | 最终全新 Global 综合及 153 输入身份检查通过 |
| 180512 route | 最终 route_design 完成，16466/16466 可布线网络完成、0 路由错误；发布门槛仍阻断 |

最终 routed DCP 仅作审核，不是下载文件。较早 DCP、日志和 PASS 均按路径区分，不能证明当前源。旧 M1 bit/DCP 在外部备份保存，不是当前构建输入，也不放入新交付。执行入口使用唯一 build_id，重复阶段拒绝覆盖，失败返回非零；阶段完成不等同 PHY/CDC/实物签收。
''', encoding='utf-8')

readme = '''# M2 真实采集集成审核交付

审核日期：2026-10-13。沿用本次集成编号 `20261012_171114`；最终原生构建编号 `20261012_180512`。

已在当前 M2 主线完成真实 RTL/BD 集成、旧引用清理、原生 Wrapper/Top 更新及 GUI 连线排版。修复配置事务完成握手、首心跳前失钟漏报、FIFO reset-busy 期间 ARM 提前生效、tap 合法窗口读回及 IDELAYCTRL 器件属性。当前 13 个生产 RTL 中修改 4 个，9 个保持起始哈希；完整差异在 `source_diff.patch`。

**结果：ROUTED_PROVISIONAL / NOT_DOWNLOAD_READY。** 原生 Validate、14 次实际 XSim、全新 Global 综合及布局布线已完成；仍有 PHY 使用规则和自定义 RTL CDC 阻断，以及未签收的源同步输入预算/板级条件。本包供审核与继续开发，未生成 bit 或 XSA，未进行板卡下载或雷达复位。

| 验收维度 | 结果与证据 |
|---|---|
| 实例、源和 Top | 1 个 radar_capture_bd_0；13 条唯一生产 RTL 路径；radar_m2_top；sim Top=tb_capture_fifo |
| 连线与外口 | W01～W14、I01 完整；31 项脚本连接核验；16 BD 标量 + 差分参考接口；18 wrapper 口；16 物理 IO |
| 冻结参数和地址 | 11 个官方 IP/常量配置无改动；两 CSR 和两独立 DDR 映射保持；见 ip_parameter_diff.tsv/address_map.tsv |
| 仿真 | 10 个测试台、14 次运行全部 PASS；当前源码与官方 FIFO/UNISIM；SIM_PASS.json 绑定输入和完整日志 |
| 原生生成/Validate | Module Reference、IP、managed wrapper 已刷新，完整 Validate 通过 |
| 综合/实现 | 当前源 Global，无旧增量 DCP；16466 条可布线网络全部完成、0 路由错误 |
| 已加载约束时序 | WNS +0.804 ns，WHS +0.011 ns，WPWS 0.000 ns；外部输入预算尚未加载，不能视为采样时序签收 |
| CDC 数据总线 | 2 mailbox + 2 snapshot，共 585 个有效受约束端点，8 ns max-delay/bus-skew 未被全域 false path 覆盖 |
| PHY 规则 | BLOCKED：3×REQP-1742；IDELAY.CLK 与 ISERDES.CLKDIV 不同源，tap 更新规则还须修正 |
| CDC 结构 | BLOCKED：自定义 RTL 的组合逻辑、复位释放与分频器 CLR 风险；见最终 cdc_review.md |
| 板级与固件 | PENDING：实际修订、电气、bin/cfg、双边沿输入 min/max 和 idle 前导 |
| 排版/幂等 | 原生 GUI 分数据链、控制链、时钟复位区；保存后重开；重复集成 BD 哈希不变 |
| ZIP 原生重开 | REOPEN_STATE；详见 package_reopen_02/ 日志和最终外附核验 |

当前路由工具没有因固定球位而终止；不得据此认为电气或专用 PHY 使用规则已合格。所有 3 条 REQP-1742 保留原严重级别。参考时钟无效端接属性已移除，球位和 I/O 标准保持。详细阻断及最小后续方案见 `phy_rule_review.md`、`cdc_review.md`、`project/docs/m2_real/NEXT_PHY_CDC_PLAN.md`。

## 工程与证据

ZIP 中 `project/` 是可重新打开的当前工程，`evidence/delivery/` 是审核资料，`evidence/outputs/` 保留本轮编译、展开、运行、构建和报告。日志内原绝对路径是历史记录，活跃工程不依赖 Downloads。Vivado 2025.1 安装仍为正常外部工具依赖。未嵌入历史 ZIP、缓存、运行目录、旧 bit/XSA 或 OLD_DIAGNOSTIC checkpoint。

修改前可恢复备份：`/archive/20261012_171114/baseline.zip`，741 个文件；SHA-256 `c4f5987fd5c5de862fd4c536ff70dd5067e8529a4d3bd792390a3a4ac2e6e323`。备份不递归加入交付 ZIP。M1 历史内容留在备份，活跃工程不再引用。

关键索引：`connections.tsv`、`address_map.tsv`、`retained_and_removed.tsv`、`source_manifest.tsv`、`constraint_coverage.md`、`timing_budget.md`、`tests_review.md`、`warnings_review.md`、`build_attempts.md`、`pending_signoff.md`。原生布局图在 `layout/reopened.png` 和 `.svg`，机器状态在 `status.json`。

## 重新运行

在解压后的 `project/` 打开 PowerShell，使用 Vivado 2025.1。禁止并行编辑同一工程；build_id 换成未使用的新值。

```powershell
$vivado = 'C:/Xilinx/2025.1/Vivado/bin/vivado.bat'
& $vivado -mode batch -source scripts/m2_real/run_audit.tcl
& $vivado -mode batch -source scripts/m2_real/run_integrate.tcl
python -I scripts/m2_real/run_sim.py --output outputs/UNIQUE_BUILD_ID/xsim
python -I scripts/m2_real/simulation_gate.py --suite outputs/UNIQUE_BUILD_ID/xsim
python -I scripts/m2_real/simulation_gate.py --check
& $vivado -mode batch -source scripts/m2_real/build_export.tcl -tclargs synth UNIQUE_BUILD_ID
& $vivado -mode batch -source scripts/m2_real/build_export.tcl -tclargs route UNIQUE_BUILD_ID
```

审查入口不隐式 Validate/保存；集成入口才修改和生成。仿真执行方法详见脚本 --help；旧 gate 仅在源码、官方 FIFO 模型输入及日志哈希完全相同时可用，发生修改后须完整重跑。构建脚本不发布 bit，缺陷修复和物理条件签收后才进入正式发布步骤。

FIFO 出口 scoreboard 已检查数据/TKEEP/TLAST。源完成、FIFO 排空和 SG/DDR 完成是不同事件；本轮没有验证 SG 驱动或 DDR 采集。下一步优先修复 PHY 时钟/延时执行域及 CDC 结构，再重新回归；板测由后续签收决定。
'''
readme = readme.replace('REOPEN_STATE', status['native_zip_reopen'])
(delivery / 'README_交付说明.md').write_text(readme, encoding='utf-8')
print(json.dumps({'status': status['overall'], 'native_build': status['highest_build_stage'], 'reopen': status['native_zip_reopen']}, ensure_ascii=False))
