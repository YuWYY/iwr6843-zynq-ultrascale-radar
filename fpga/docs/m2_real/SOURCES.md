# 技术依据与基线

用户基线：Radar_XCZU2CG_M2(1).zip（本包README记录SHA）；F20核心模块原理图20210926第8页；底板原理图20240606；TI PROC073D(001_IWR)_Sch.PDF；用户转接板.tel（只有三组连接器，没有额外LVDS终端）。

以下公开文档用于原语接口和机制核对，不能替代实物时序签核：

- AMD UG974 2025.1 ISERDESE3：https://docs.amd.com/r/2025.1-English/ug974-vivado-ultrascale-libraries/ISERDESE3
- AMD UG974 2025.1 IDELAYE3：https://docs.amd.com/r/2025.1-English/ug974-vivado-ultrascale-libraries/IDELAYE3
- AMD UG974 2025.1 IDELAYCTRL：https://docs.amd.com/r/2025.1-English/ug974-vivado-ultrascale-libraries/IDELAYCTRL
- AMD UG974 2025.1 BUFGCE_DIV：https://docs.amd.com/r/2025.1-English/ug974-vivado-ultrascale-libraries/BUFGCE_DIV
- AMD UG974 2025.1 OBUFT：https://docs.amd.com/r/2025.1-English/ug974-vivado-ultrascale-libraries/OBUFT
- AMD PG085 Resets：https://docs.amd.com/r/1.1-English/pg085-axi4stream-infrastructure/Resets
- AMD PG085 Flags：https://docs.amd.com/r/1.1-English/pg085-axi4stream-infrastructure/Flags
- AMD PG021 Scatter/Gather Mode：https://docs.amd.com/r/en-US/pg021_axi_dma/Scatter/Gather-Mode
- TI IWR6843数据手册：https://www.ti.com/lit/ds/symlink/iwr6843.pdf

本版保留RAW16，不依据其他型号的应用例程推断当前IWR6843板上固件的HSI/IQ/chirp格式。定时模板故意不填编造的板级skew数字。
