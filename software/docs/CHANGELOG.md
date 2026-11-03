# 改动清单

## 当前 M2-SW4.1-v1.7（20261030_pages_3tx_01）

已增加KEY3四页导航、独立页面/数据确认、英文1080p界面，以及3TX4RX的96chirp配置和严格HSI/IQ解析。完整当前说明见 [CURRENT_RUNTIME.md](CURRENT_RUNTIME.md)。按键按用户要求未验证；采集和软件切页有独立板测证据。

以下为从上传基线到v1.1的历史记录，其中“默认profile”“不包含FFT/DP”等描述不代表当前主线。

## 历史：从本次上传基线到 M2-SW4.1-v1.1

## 生产逻辑

- SW3.2：独立RUN_RESET许可、epoch+reset serial绑定、整批末尾签发、一次性消费、失效原因可见；历史计数不作为准入。
- 保留旧无输入HALT测试命令，新增qualify/qualify10及兼容别名；手动reset不签发许可。
- 在canary/BD/payload写入前检查实际静止和全空闲环，不能只信READY枚举。
- 清理NOT_RUN旧耗时，活动阶段轮询时间重新计量，去除BD1/CUR重复显示。
- SW2抽取只读capture_check，维持单owner模式；SW4使用相同门槛claim，不绕过电源/IO/UART保护。
- SW4.1：capture preflight九项只读门槛；消费许可后才开始首个sensorStop/config等命令。
- 正常真实收尾增加全部预期BD已完成/HwCnt零/无半包等核对；记录入口、正常HALT与RESET分相证据；最终RESET后核对epoch+1及全空闲环。
- success/failure/recovery不自动续签；真实HALT、RESET、RECOVER、最终handoff失败仍使结果无效。
- profile默认UNQUALIFIED、版本身份、GPIO ABI、内存布局和硬件采集合同不改变。

## 源码组织与构建

14个生产C和15个头文件按app/hal/capture/config整理；链接脚本置linker。保留三个Vitis CMake文件在src根。完整映射见 SOURCE_MAP.json，修改前后哈希见 validation/BASELINE_DIFF.json。

CMake采用14个显式相对源路径、4个include目录；检测残留平铺C文件，防止覆盖式安装误编译。旧lscript/缓存归档，不混在活动src中。目标不依赖tests/tools，不复制主机测试桩到BSP。

## 工具

安装器完整src目录置换、备份到组件外、保留本机配置、失败回滚、防符号链接/错组件/误覆盖正式profile。默认只预览。实际上传应用副本的安装测试见 validation/ACTUAL_BASELINE_INSTALL.json。

ELF检查版本更新，拒绝本次上传的旧ELF；profile输出路径改为config子目录；AArch64检查扫描分类后的14个生产C。

## 不包含

不改Platform/BSP/XSA/bit/FSBL，不交付新ELF，不加RTOS/FFT/DP/合成数据源，不提供未经审查的雷达bin/cfg，不把model测试作为板测。
