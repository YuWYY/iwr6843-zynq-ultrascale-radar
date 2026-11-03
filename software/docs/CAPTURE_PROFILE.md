# 正式采集 profile

> 历史通用流程。当前主线已使用受控3TX4RX HSI单帧配置，参数和生成身份见 [CURRENT_RUNTIME.md](CURRENT_RUNTIME.md)。下文的UNQUALIFIED默认值描述上传时基线，不是当前生成头的状态；不得据此覆盖当前profile。

默认 `src/config/radar_capture_profile_data.h` 与上传基线保持字节一致：UNQUALIFIED。不要通过把approvals、format确认字或SW2计数手工改成非零来启动RF。

正式输入为：实际烧入雷达的bin、实际完整cfg、基于 `capture_review.template.json` 的审查记录。至少核对固件/SDK版本及CLI返回，非自动发射与可控启停，Lane数量/速率、16bit位序、FRCLK字边界、HSI/CQ/填充、ADC原始流条件、空闲时钟或足够的有效前导、以及电压/端接/时序条件。

生成命令（在本包根目录）：

```powershell
python tools/generate_capture_profile.py --firmware actual_radar.bin --cfg actual_radar.cfg --review capture_review.json --output radar_runtime/src/config/radar_capture_profile_data.h --force
```

也可以把output指向已安装应用的同一路径。生成后重新原生构建并完整启动。生成器会检查字段、完整配置、命令白名单、容量/描述符上限和审查标志；不支持的cfg必须先人工分析并受控扩展白名单，不能改用无约束UART透传。

生成器计算文件SHA，不会读取雷达Flash或验证示波器波形；`capture confirm ID` 仍是使用者确认实际烧写与接线匹配，不是软件测量。预先填“已批准”不能代替真实证据。输出中的expected hash也不是实时测量身份。

`dma plan` 只给运输容量规划，不修改该profile，也不改变capture once实际收包大小。测试目录的模型profile只验证软件流程，不应作为真实雷达配置下载。
