# 首页距离可信提示增量交付

版本 **M2-SW4.1-v1.8.1**，build `20261101_range_quality_01`。本轮仅增加首页当前距离峰的可信状态，不开展近距离测量优化。

## 当前行为

- 当前峰为搜索边界bin6/204时，显示 `-- M / RANGE UNRESOLVED`，不再把0.26m当作可信距离，也不自动替换成2.82m等其他峰。
- 原始谱、峰功率、距离历史及后三页空间点保留。只有内部独立峰、没有ADC削顶，且同session/时间戳有同bin CFAR候选，才显示数值及金色标记，并提示 `PEAK CHECK PASSED`。
- 缺少同帧检测结果、未确认峰或ADC削顶时分别显示原因。零输入不会误标为近端目标；角度/高度无效、零Doppler不会单独否定距离峰。
- 这是峰值检查，不是目标跟踪、物理精度标定或可信XYZ认证。后三页未新增距离有效性筛选。

## 核验

- 针对性质量逻辑单测、严格编译和UBSan通过，见 [unit](checks/range_quality_unit.json)。
- 使用已保存的399360字节真实RAW重跑原生产处理链：原peak6、12个空间候选和11个名义XYZ保持不变，质量为RANGE_EDGE；质量调用前后原始数据、完整距离/空间结果逐字节一致。256个谱点对原串口导出误差约0.000996dB，见 [真实回放](checks/range_quality_real_replay.json)。
- 六类状态加真实首帧的生产绘图检查通过；25次像素比较确认首页仅侧栏和原9×9峰标记变化，后三页逐像素一致。见 [视觉记录](visuals/range_quality/REVIEW.md)。
- 原生应用构建及内存布局通过，接手时的range/spatial/scan、帧格式、KEY3、页面调度、链接地址和硬件输入身份保持，见 [保护项核对](checks/protected_sources.json)。
- 冷启动后接受 **10帧**真实数据，CRC/HSI96/FFT384/TX各32及空间结果检查通过，DP无新增FIFO观察或DPDMA错误。本轮为小范围增量核验，未重复旧69帧测试，也未把旧PASS当作新ELF板测。
- 首帧质量 `QUALIFIED`，原峰bin `64`，显示数值有效位 `1`。中间session5实际观察到bin6为RANGE_EDGE、distance_valid=0、displayed_mm=-1；最终质量 `QUALIFIED`。
- 板测控制器首次在LIVE占用时请求详细诊断，触发正常的SCOPE BLOCK拒绝，原FAILED记录保留。已将诊断移至停采间隙，在同一运行ELF继续核验，没有复位或重新下载，也没有绕过采集所有权。两份原始日志及此唯一已解释拒绝由[独立审计](checks/board_quality_independent.json)核对。
- 实物画面确认：`USER_CONFIRMED_QUALITY_LABELS_AND_SESSION_CONTINUING`。

## 运行与范围

当前已恢复总览LIVE。`display status`新增 `[VIEW RANGE]`，无效距离用`displayed_mm=-1`，原峰仍可作为诊断读取。板测见 [quality_20261101_155104](board_sessions/quality_20261101_155104/result.json)。

未修改波形、距离范围、FFT/CFAR/测角、空间检测输出或FPGA；未实施背景消除、标定和跟踪。KEY3未修改、未测试。已知DP软件重启恢复问题本轮未修复，仍采用先正常停止采集/显示、完整断电后受控启动。

应用ELF：`/software/radar_runtime/build/radar_runtime.elf`  
SHA-256：`854f594cac75c905fed0302f3ad21019525e539c5895520fe374ae7069543f72`

修改前基线在`baseline/`，接受后的源码与ELF在`success_baseline/`，差异在[changes](checks/source_changes.diff)。唯一活跃工程仍为`/software`；无ZIP。说明见[CURRENT_RUNTIME.md](../../docs/CURRENT_RUNTIME.md)。
