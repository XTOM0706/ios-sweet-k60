# K60 相机可行性：从 CSIPHY 出第一帧到 iOS 相机 App

2026-11-01 离线轮。设备不在手，本篇所有结论按三类标注：

- **【已验证】** 有可点开的链接或本机命令输出，任何人都能重放；
- **【静态推导】** 从 `dump/live.dts`、上游内核源码、厂商 DT 推出来，逻辑闭环但没有上机；
- **【待上机】** 必须拿设备才能定论，判据写在 §10。

量级估计一律标 **【估计】**，是人月区间不是承诺。

---

## 0. 一句话结论

**能出画面，但不是"把相机做出来"，是"拿 CSIPHY/CSID/VFE-RDI 抽一条裸 RAW 支路，
在用户态补去马赛克，再把帧塞进被 patch 过的 `cameracaptured`"。**

分三句说清：

1. **Linux 侧没有现成答案可抄。** 上游 `camss` 有 VFE680/CSID680 驱动，但 **SM8450 家族在
   mainline 里连一个 camss 设备树节点都没有**（`sm8450.dtsi` 只有 camcc 和 cci）。VFE680 的
   提交信息自己写着"as found on sm8450"，可实际只被 SM8550 / X1E 的资源表接上，SM8450 没人接。
2. **厂商 `techpack/camera` 搬不进 XNU。** 它是 Linux 内核模块，挂死在
   `dma_buf`/`iommu`/`interconnect`/`PIL`/`HFI` 上，而且真正出图的 ISP（IPE/BPS）由 LX7 DSP 跑
   `CAMERA_ICP.elf` 闭源固件驱动 —— 那套固件协议没有任何公开文档。
3. **iOS 侧没有先例。** 全部本地仓库 + UniXNU 全部设备文档，相机一栏都是
   `not implemented`；连"最完整"的 pandora（SM8850，能跑 App）也没有相机。这不是没找对仓库。

**只做一件事的话：先把 CCI 打通、读出 OV64B40 的 chip ID。** 理由和判据见 §9。

---

## 1. 口径：这颗 SoC 到底叫 SM8450 还是 SM8475

任务里写 SM8450，K60 实际是 **SM8475（骁龙 8+ Gen 1）**【已验证，见设备规格页】。SM8475 是
SM8450（waipio）的提频版，相机子系统的寄存器布局同族；本篇所有地址都来自 K60 自己的
`dump/live.dts`，不依赖我们对"应该是什么"的猜测。

一个旁证：K60 的 DT 里两个 CSIPHY 的 compatible 是
`"qcom,csiphy-v2.1.3-xiaomi-m11a"` —— 小米给这颗机器单独打了 CSIPHY 变体，
`m11a` 是小米内部型号。也就是说**即便上游有 v2.1.3 的支持，也不等于支持 K60 的 PHY**【已验证，DT 第 23864 / 23918 行】。

XNU 侧的现状（背景，不重复验证）：锁屏能进、触摸卡在 IOVDD 投票、GPU 只有内核侧 GMU、
相机空白；`UniXNU/drivers` 只有 Adreno740 / AdrenoMetal / AppleKeyStoreState / ArmCpu /
ArmDTPlatform / ArmVirtPlatform / DarwinBoot / GopFramebuffer / QcomGeniSpi / RamBlock /
SoftwareAES / VirtioMmioBlock / VirtioMmioConsole 十三个组件，没有任何 camera/CSI/CCI。

---

## 2. K60 的相机硬件清单

全部来自 `~/hackios/dump/live.dts`（34801 行的厂商 AOSP DT），行号可复查。

### 2.1 四颗 sensor 与它们的挂载点

| DT 节点 | 位置 | 型号 | 像素 | CSIPHY | CCI | MCLK | RESET | 附属 |
|---|---|---|---|---|---|---|---|---|
| `qcom,cam-sensor0`（24133） | 后主摄 | **OV64B40** | 64MP，OIS+PDAF | csiphy2 | cci0 master1 | MCLK2 / gpio102 | gpio126 | eeprom0 + actuator0 + flash0 |
| `qcom,cam-sensor1`（24167） | 前置 | **OV16A1Q** | 16MP | csiphy4 | cci1 master0 | MCLK5 / gpio105 | gpio127 | eeprom1 |
| `qcom,cam-sensor2`（24200） | 后超广角 | **OV08D10** | 8MP | csiphy1 | cci1 master0 | MCLK1 / gpio101 | gpio24 | eeprom2 + flash2 |
| `qcom,cam-sensor3`（24353） | 后微距 | **OV02B10** | 2MP | csiphy0 | cci0 master0 | MCLK0 / gpio100 | gpio128 | eeprom3 + flash3 |

sensor 型号不写在 DT 里（DT 只有 `compatible = "qcom,cam-sensor"`），型号来自整机规格：
主摄 OV64B40、超广角 OV08D10、微距 OV02B10、前摄 OV16A1Q【已验证，见 IT之家/九锋网规格页】。
这四颗全部是 OmniVision，**没有一颗在 mainline 有驱动**（`drivers/media/i2c/Makefile` 里只有
`ov64a40`、`ov08d10` 等，没有 ov64b40 / ov02b10 / ov16a1q）【已验证】。

注意 DT 的 `cell-index` 与"物理第几颗"不对应：`cam-sensor0` 走的是 csiphy2，不是 csiphy0。
上机时按 `csiphy-sd-index` 认，不要按编号认。

### 2.2 CSIPHY：6 个实例，2 个是小米私有变体

| 节点 | 基址 | 中断（DT） | compatible |
|---|---|---|---|
| csiphy0（23787） | `0x0ace4000` | `<0 0x1dd 1>` | `qcom,csiphy-v2.1.3` |
| csiphy1（23814） | `0x0ace6000` | `<0 0x1de 1>` | `qcom,csiphy-v2.1.3` |
| **csiphy2（23841）** | `0x0ace8000` | `<0 0x1df 1>` | **`qcom,csiphy-v2.1.3-xiaomi-m11a`** |
| csiphy3（23868） | `0x0acea000` | `<0 0x1c0 1>` | `qcom,csiphy-v2.1.3` |
| **csiphy4（23895）** | `0x0acec000` | `<0 0x7a 1>` | **`qcom,csiphy-v2.1.3-xiaomi-m11a`** |
| csiphy5（23922） | `0x0acee000` | `<0 0x59 1>` | `qcom,csiphy-v2.1.3` |

每 0x2000 一块，寄存器窗口一致；三个供电：`gdscr`（= `cam_cc_titan_top_gdsc`）、
`csi-vdd-1p2`（= `pm8350c_l10`，1.2V）、`csi-vdd-0p9`（= `pm8350_l5`，0.9V），
外加 `cphy_rx_clk_src` / `csiphyN_clk` / `csiNphytimer_clk_src` / `csiNphytimer_clk` 四路时钟。

**"xiaomi-m11a" 是这条路线上第一个真坑**：主摄和前摄走的正是这两个被改过的 PHY。上游的
CSIPHY 3PH 驱动只有 `GEN2 1.0 / 1.1 / 1.2.1 / 1.2.3 / 2.1.2 / 2.2.0` 和一张"4nm 2PH v2.1.2"
表，**没有 2.1.3**【已验证，`camss-csiphy-3ph-1-0.c` 的版本注释块】。所以"抄上游 camss 的
CSIPHY 初始化序列"这条捷径在主摄/前摄上是断的，只能先从 csiphy0（微距）或 csiphy1（超广）
验证通路，或者去小米开源内核里翻它那版 csiphy。

### 2.3 CCI：两条总线，四条 bus

| 节点 | 基址 | 中断 | 时钟 | 备注 |
|---|---|---|---|---|
| cci0（23949） | `0x0ac15000` | `<0 0x1cc 1>` | `cci_0_clk_src` 37.5MHz | m0/m1 两个 master，各 100k/400k/1M/custom 四档时序 |
| cci1（24235） | `0x0ac16000` | `<0 0x10f 1>` | `cci_1_clk_src` 37.5MHz | m0/m1 |

CCI 是 Qualcomm 自己的 I2C 主控（不是 GENI），带硬件队列、每笔事务写 32 位命令字、完成走中断。
上游 `drivers/i2c/busses/i2c-qcom-cci.c` **已经支持 `qcom,sm8450-cci`**【已验证，第 871 行】，
所以 Linux 上今天就能用 CCI 读写 sensor 寄存器 —— 这是"Linux 侧到底做到哪一步"最实在的答案：
**CCI 有，CSI 通路没有**。

### 2.4 CSID / VFE / SFE / 离线 ISP

| 节点 | 基址（含 wrapper / camnoc） | 中断 | compatible |
|---|---|---|---|
| csid0（25294） | `0x0acb7000` +0xd00，wrapper `0x0acb6000` +0x1000 | `<0 0x1d0 1>` | `qcom,csid680_110` |
| csid1（25343） | `0x0acb9000` | `<0 0x1d2 1>` | `qcom,csid680_110` |
| csid2（25392） | `0x0acbb000` | `<0 0x280 1>` | `qcom,csid680_110` |
| csid_lite0（25441） | `0x0acc6000` +0xa00 | `<0 0x1d4 1>` | `qcom,csid-lite680_110` |
| csid_lite1（25484） | `0x0acca000` | — | `qcom,csid-lite680_110` |
| vfe0（25316） | `0x0ac62000` +0xf000，camnoc `0x0ac19000` | `<0 0x1d1 1>` | `qcom,vfe680_110` |
| vfe1（25365） | `0x0ac71000` | `<0 0x1d3 1>` | `qcom,vfe680_110` |
| vfe2（25414） | `0x0ac80000` | `<0 0x281 1>` | `qcom,vfe680_110` |
| vfe_lite0（25462） | `0x0acc6000` | — | `qcom,vfe-lite680_110` |
| vfe_lite1（25505） | `0x0acca000` +0x2800 | `<0 0x168 1>` | `qcom,vfe-lite680_110` |
| **sfe0（25248）** | `0x0ac9e000` +0x8000 | `<0 0x1ce 1>` | **`qcom,sfe680`** |
| **sfe1（25271）** | `0x0aca6000` | — | **`qcom,sfe680`** |
| ipe0（25631） | `0x0ac42000` +0x16000 | — | `qcom,cam-ipe680` |
| bps（25651） | `0x0ac2c000` +0x7800 | — | `qcom,cam-bps680` |
| jpegenc（25679） | `0x0ac2a000` +0x1000 | `<0 0x1da 1>` | `qcom,cam_jpeg_enc_680` |
| lx7（25602） | `0x0ac01000`（csr）/`0x0ac01800`（cirq） | `<0 0x1cf 1>` | `qcom,cam-lx7`，`fw_name = "CAMERA_ICP"` |
| cpas-cdm0（25143） | `0x0ac24000` | `<0 0x1cd 1>` | `qcom,cpas-cdm` |

这张表有两个必须记住的事实：

1. **K60 的"前端"是 SFE（Sensor Front End）两个实例，另有三套 VFE0-2。** 上游 camss 的
   SM8550 资源表里只有 vfe0-2 + vfe_lite0-1，没有 sfe；`sfe680` 是小米/高通下游的命名，
   对应到上游是哪块硅，**没有任何公开文档能确认**【静态推导】。这是整条路线上最大的未知。
2. **真正出彩图的 ISP 是 IPE/BPS + LX7 固件**，不是 VFE。VFE 只把 CSI-2 收到的数据搬进内存
   （RDI），连去马赛克都不做。

### 2.5 供电轨

CSIPHY 公共部分：

| 轨 | 来源 | 电压 |
|---|---|---|
| `csi-vdd-0p9` | `pm8350_l5`（PMIC RPMh LDO） | 0.9V |
| `csi-vdd-1p2` | `pm8350c_l10` | 1.2V |
| `gdscr` | `cam_cc_titan_top_gdsc` | — |

每颗 sensor 自己的模拟/数字轨（全部是 `regulator-fixed` + GPIO 使能，**意味着 XNU 侧就是
拉一个 TLMM 脚，不需要 PMIC 编程**，这是好消息）：

| sensor | 轨 | 电压 | 实现 | 使能脚 |
|---|---|---|---|---|
| 主摄 OV64B40 | `cam_vdig` = `camera_wide_dvdd_ldo` | 1.1V | GPIO LDO，vin=pm8350c 某路 | gpio2 |
| | `cam_vana` = `camera_wide_avdd_ldo` | 2.8V | GPIO LDO | gpio70 |
| | `cam_v_custom1` = `camera_wide_avdd2_ldo` | 1.86V | GPIO LDO | gpio97 |
| 前摄 OV16A1Q | `cam_vana` = `camera_front_avdd_ldo` | 2.8V | GPIO LDO | gpio64 |
| 超广 OV08D10 | `cam_vana` = `CAMERA_ULTRA_AVDD` | 2.8V | GPIO LDO | gpio6 |
| 微距 OV02B10 | `cam_vana` = `CAMERA_MACRO_AVDD` | 2.8V | GPIO LDO | gpio118 |
| | `cam_vio` = `pm8350c_l1` | 1.86V | PMIC LDO（RPMh 投票） | — |

注意 `pm8350c_l1` 是这条链上**唯一需要 RPMh 投票的 sensor 供电**（微距的 IOVDD）。其余都是
TLMM GPIO 直拉。上电顺序按 DT 的 `rgltr-load-current`/`rgltr-min-voltage` 表推（vdig → vana →
custom1 → MCLK → reset 释放），不复现高通"先 1.1 再 2.8"的具体毫秒数就只能靠试。

### 2.6 时钟域与 GDSC：这块最枯燥但最确定

`camcc` 在 `0x0ade0000`（0x20000 大小）【已验证，DT 第 815 行；mainline `sm8450.dtsi` 同址】。
所有相机时钟都在这一块里，`cam_cc_*` 的 GDSC 寄存器在 `0x0adf0000` 一带：

| GDSC | 偏移 | 管谁 |
|---|---|---|
| `cam_cc_bps_gdsc` | `0xadf0004` | BPS |
| `cam_cc_ipe_0_gdsc` | `0xadf0078` | IPE0 |
| `cam_cc_sbi_gdsc` | `0xadf00d0` | SBI |
| `cam_cc_ife_0_gdsc` | `0xadf1004` | IFE0 |
| `cam_cc_ife_1_gdsc` | `0xadf2004` | IFE1 |
| `cam_cc_ife_2_gdsc` | `0xadf2050` | IFE2 |
| `cam_cc_sfe_0_gdsc` | `0xadf3050` | SFE0 |
| `cam_cc_sfe_1_gdsc` | `0xadf3098` | SFE1 |
| `cam_cc_titan_top_gdsc` | `0xadf31dc` | 整个相机子系统的总闸 |

**上游 camcc 驱动的 `power-domains` 写得很清楚：`RPMHPD_MMCX` 和 `RPMHPD_MXC` 两路**
【已验证，`sm8450.dtsi` camcc 节点】。也就是说就算 GDSC 位都写对，没有这两路 RPMh 投票，
camcc 也不会真正上电。我们的 RPMh 投票器（触摸 IOVDD 那条线）已经有，扩两个 RSC 资源即可
【静态推导，需上机确认资源 ID】。

---

## 3. Linux 侧 SM8450 相机栈现在到什么程度

### 3.1 mainline camss 有的东西（照抄价值最高）

`drivers/media/platform/qcom/camss/` 现在包含 `camss-csid-680.c`、`camss-vfe-680.c`、
`camss-csiphy-3ph-1-0.c`、`camss-vfe-vbif.c` 等【已验证，Makefile】。VFE680 是 2025 年
3 月进的（Bryan O'Donoghue / Linaro），提交信息原文：

> Add silicon enabling support for VFE680 as found on **sm8450, x1e** and derivatives thereof.

【已验证，见 lkml 与 raspberrypi 镜像】。CSID680 同理。

### 3.2 SM8450 的洞：连设备树节点都没有

三个层次都缺，逐个可查：

| 层 | 状态 | 证据 |
|---|---|---|
| `arch/arm64/boot/dts/qcom/sm8450.dtsi` | **只有 `camcc` 和 `cci0/cci1`，`grep -i camss` 命中 0 行**；没有 `isp@` 节点 | 我拉的是 2026-11 的 master 分支，文件 6324 行 |
| `camss.c` 的 compatible 表 | 只有 `msm8916/8939/8953/8996/qcm2290/qcs8300/sa8775p/sc7280/sc8280xp/sdm660/670/845/sm6150/6350/8250/8550/8650/x1e80100/glymur`，**没有 `qcom,sm8450-camss`** | of_match 表 5813–5831 行 |
| `camss.h` 的 `enum camss_version` | 有 `CAMSS_845`（那是 SDM845！）、`CAMSS_8550`、`CAMSS_8650`、`CAMSS_X1E80100`，**没有 SM8450/WAIPIO** | 第 82–100 行 |

结论：**VFE680 的代码是为 SM8550/X1E 落地的，SM8450 家族没人接资源表和设备树。**
SoC 2021 年 12 月发布，到 2026 年 11 月仍然如此 —— 这不是"再等几个版本就有了"的问题。

### 3.3 但 K60 的地址和 SM8550 高度重合（这条是机遇）

把 K60 的 DT 和上游 `sm8550.dtsi` 的 camss 节点并排看：

| 块 | K60（SM8475） | 上游 SM8550 | 是否重合 |
|---|---|---|---|
| csid0 / 1 / 2 | `0xacb7000` / `0xacb9000` / `0xacbb000` | `0xacb7000` / `0xacb9000` / `0xacbb000` | **完全一致** |
| csid_wrapper | `0xacb6000`（+0x1000） | `0xacb6000` | **完全一致** |
| csiphy0..5 | `0xace4000` 起每 0x2000 | `0xace4000` 起每 0x2000（到 csiphy7） | **完全一致** |
| vfe0 / 1 / 2 | `0xac62000` / `0xac71000` / `0xac80000` | 同上 | **完全一致** |
| vfe_lite0 / 1 | `0xacc6000` / `0xacca000` | `0xaccb000` / `0xaccf000` | 不同 |
| csid_lite0 / 1 | `0xacc6000` / `0xacca000` | `0xacca000` / `0xacce000` | 不同 |
| sfe0 / 1 | `0xac9e000` / `0xaca6000` | 无此块 | — |

也就是说：**CSIPHY、CSID、VFE0-2 这三层可以几乎照搬上游 SM8550 的寄存器序列，
只有 lite 实例的基址和 SFE 是空白。** 这是整篇里最有价值的一条结构性发现，
也是"为什么值得做"的主要理由。

### 3.4 有没有人贴过 SM8450 能用的照片

**没找到。** 搜索到的 SM8450 相机活动全部是 Android 侧（厂商 techpack / LineageOS），
mainline camss 侧对 SM8450 的公开成果为零：没有设备树补丁、没有资源表补丁、
没有 lkml 上的 "sm8450 camera" 系列。同一颗硅的 SM8550 反而有完整的 v9 系列
（CSIPHY 3ph Gen2 v2.1.2 + CSID680 + VFE680 + ISP 资源）【已验证，LWN 上的 v9 00/16】。

### 3.5 上游 camss 只做 RDI：不做 ISP，也就不需要固件

Kernel 文档写得很直白：camss 支持"CAMSS 输入 → CSIPHY → CSID → VFE 的 **RDI** 接口 →
原始数据直接落到内存"【已验证，docs.kernel.org qcom_camss】。VFE680 驱动里还留着一句 TODO：

> This macro only works because RDI is all we support right now.

【已验证，`camss-vfe-680.c` 的 `RDI_WM()` 注释】。这意味着 **上游这条路不跑 ISP、
不加载任何固件、也不需要 3A**。对 XNU 移植是好消息（少一大块闭源依赖），
对"照片好不好看"是坏消息（出来是 Bayer RAW）。

### 3.6 Linux 侧唯一能"今天就开始"的实验

`i2c-qcom-cci.c` 支持 `qcom,sm8450-cci`，`drivers/media/i2c` 里有 `ov08d10.c`
（正好是 K60 的超广角）【已验证】。所以在任何一台能跑 mainline 的 waipio 板上，
**给超广角供电 + 起 CCI + 读 OV08D10 的 chip ID 是现成代码能做的事** ——
这也说明"能否驱动 sensor"和"能否出图"是两件难度差一个数量级的事。

---

## 4. Android 的 camera 驱动（techpack）能不能搬进 XNU

### 4.1 它是什么

`qcom/opensource/camera-kernel`，代号 Spectra。入口 `camera_main.c` 里挂了一串子模块：
`camera_base` / `camera_tfe` / `camera_isp` / `camera_sensor` / `camera_icp` / `camera_sync` /
`camera_jpeg` / `camera_fd` / `camera_custom` / `camera_ope`，ISP 侧又分
`cam_ife_csid` / `cam_ife_csid_lite` / `cam_vfe` / **`cam_sfe`** / `cam_isp_dev`
【已验证，LineageOS 的 `android_kernel_qcom_sm8450-modules`】。

### 4.2 它依赖什么

| 依赖 | 在 XNU 上对应什么 | 难度 |
|---|---|---|
| `platform_device` / `of_` / `devm_*` | 自己写一套 glue，或按 IOKit 重写 | 中 |
| `regulator` 框架 + `regulator_set_voltage` | 我们有 GPIO 直拉 + RPMh 投票 | 低 |
| `clk` 框架（camcc 80+ 时钟） | XNU 没有 clk 框架，全裸写寄存器 | 中高 |
| `iommu` / `dma_buf` / `dma-heap` / `ION` | 我们有 ArmSmmu4K；`dma_buf` 要自己造 | 高 |
| `interconnect`（camnoc/cpas 带宽投票） | 要写 BCM/vote 逻辑 | 高 |
| `PIL`（`scm`/`qcom_scm_pas_*`，加载 LX7 固件） | XNU 侧没有 SCM 调用的现成实现 | 高 |
| `HFI`（LX7 与内核的信箱协议） | 完全私有协议，无文档 | 极高 |
| V4L2 / media controller | iOS 没有这套，得换成 IOKit | 高 |

### 4.3 结论

**"搬"是不成立的，只能"抄寄存器序列"。** 值得抄的是：每个块的初始化顺序、时钟/带宽投票点、
CSIPHY 的 lane 配置表（含 `m11a` 那版）、sensor 上电时序表。不值得抄的是整个框架 ——
它的价值在于告诉我们"硬件怎么点"，不在于"怎么组织代码"。

---

## 5. 固件：需要什么、在不在 linux-firmware、闭源到什么程度

### 5.1 需要哪些

| 固件 | 谁要 | 作用 | 能不能绕过 |
|---|---|---|---|
| `CAMERA_ICP.elf`（+ 分段 `.b16/.b17/.mdt`） | LX7 DSP（`cam_lx7`，DT 里 `fw_name = "CAMERA_ICP"`） | 跑 IPE/BPS 的整条离线 ISP，含 3A 调度 | **能绕**：不走 IPE/BPS 就没有它 |
| VFE/SFE 微码 | 上游说法是"没有"；下游 `cam_vfe`/`cam_sfe` 也不加载独立固件 | — | — |
| sensor 固件 | **不需要**（OV 系列是纯寄存器器件，只有 OTP/eeprom 数据在模组里） | — | — |
| CSIPHY 固件 | 不需要（纯寄存器 PHY） | — | — |

所以准确的表述是：**只要 RDI 裸数据，一个固件都不需要；想要"处理过的图像"，
就必须让 LX7 跑 `CAMERA_ICP.elf`。**

### 5.2 linux-firmware 里有没有

**没有。** Debian 的 `firmware-nonfree`（就是 linux-firmware 的打包）里 `qcom/` 目录顶层
48 个文件、0 个子目录，全是 Adreno GPU 的 `aXXX_gmu.bin` / `aXXX_sqe.fw` / `aXXX_pfp.fw`，
按 `cam|icp|vfe|sfe|isp` 正则过滤**命中 0** 【已验证，sources.debian.org 的目录 API】。
换句话说：**哪怕你想在 Linux 上跑完整 ISP，也拿不到固件** —— 那是 Android vendor 分区的东西。

### 5.3 闭源程度

`CAMERA_ICP.elf` 是签过名的 ELF（有 hash segment，PIL 会验），源码不公开，协议（HFI）只有
下游驱动里的结构体定义，没有任何协议文档。LineageOS 是从小米/高通原厂 `vendor` 分区里
`extract-files` 出来的【已验证，`proprietary-files.txt` 里赫然列着 `vendor/firmware/CAMERA_ICP.elf`】。

### 5.4 K60 上它在哪

在 `/vendor/firmware/`（super 分区的 vendor 逻辑卷里）。我们目前的 dump 只有
`boot_a.img` / `vendor_boot_a.img` / `dtbo` / 各 aop/dsp 等分区，**没有 vendor.img**，
所以"K60 上确实存在这个文件"这一条是【待确认】—— 判据写在 §10 第 5 条。

---

## 6. iOS 用户态要什么

### 6.1 链路长什么样（从 App 往下数）

```
Camera.app / 第三方 App
   └─ AVFoundation: AVCaptureSession / AVCaptureDevice
        └─ FigCapture 私有框架
             └─ XPC → cameracaptured（iOS 13+；更早是 mediaserverd 里的同一套代码）
                  └─ IOKit: AppleH<xx>CamIn  (IOService, FrameReceiver + BufferPool + UserClient)
                       └─ 硬件 CSI/ISP
```

`cameracaptured` 那一跳是**逆向出来的**，原始描述很具体：
`+[AVCaptureDevice devicesWithMediaType:]` 最终落到
`FigCaptureSourceRemoteCopyCaptureSources(1)`，XPC 调 `cameracaptured` 的
`_captureSourceServer_handleCopySourcesMessage`，后者遍历 `_sSourceList` 并用
`_captureSourceServer_createSerializedSource` 序列化每个 source
【已验证，vphone 项目的 `libvcamcaptured.m` 注释】。老的 `mediaserverd` 负责"发/收
IOSurface 背书的像素缓冲、JPEG 编码"，Apple 开发者论坛的说法可以佐证【已验证】。

### 6.2 内核侧到底要提供哪几个 IOKit 服务

从 iPhone X（iOS 11.2.6）的 IOKit 类表里能直接看到最小的那套【已验证，nixerr 的
`iokit.csv`】：

| 类 | 层级 | 作用 |
|---|---|---|
| `AppleH10CamIn` | `IOService` | 相机输入主服务（H10 = 该代 ISP 代号） |
| `AppleH10CamInFrameReceiver` | `OSObject` | 帧接收器 |
| `AppleH10CamInFrameReceiverBuffer` / `…BufferPool` | `OSObject` | 缓冲池 |
| `AppleH10CamInUserClient` / `…WorkLoop` | — | 用户态通道 |
| `AppleH10PearlCam` + `Interface` / `UserClient` + `PearlCamEventSource` | — | TrueDepth |
| `AppleJPEGDriver` / `AppleJPEGHal` | `IOService` | 静态照片编码 |
| `AppleM2ScalerCSCDriver` / `AppleM2ScalerCSCHal` | `IOService` | 缩放 + 色彩空间转换 |

命名规律是 **`AppleH<SoC代号>CamIn`**，iPhone X 上是 `H10`；同一张类表里 `Oscar` 一次都
没出现（`grep -i oscar` 命中 0），所以"要仿 AppleOscar"这种说法在这代机器上没有依据。
**这套 kext 是二进制闭源的，对外接口只能靠逆向**。

### 6.3 两条接法，一条现实一条不现实

| 接法 | 做法 | 工作量 | 风险 |
|---|---|---|---|
| A. 伪装 kext | 实现一个 `AppleHxxCamIn` 兼容的 IOService，让原版 `cameracaptured` 直接认 | 需要逆向整个 external method 表、属性协议、buffer 生命周期 | 极高，接口一改就崩 |
| B. patch 用户态 | 像 vphone 那样往 `cameracaptured` 注入/替换 source，把我们的 IOSurface 帧喂进去 | 中等：需要 dyld 注入 + 一个合成 source + 我们自己的内核服务 | 中高，但**可增量**、可回退 |

本项目已经在做用户态 patch（`UniXNU-pong/userland/ui_compat.c` 里有 ObjC 方法替换的
`install_objc_hooks` 骨架），接法 B 是同一套手艺的延伸。**结论：走 B，不走 A。**

### 6.4 有没有先例：没有

逐个查过：

- UniXNU 的 pong（SM8475，同 SoC）：「Wi-Fi, Bluetooth, **camera**, audio, sensors | not implemented」；
- UniXNU 的 umi（SM8250）：「Wi-Fi, Bluetooth, cellular, audio, **camera** and sensors |
  Not implemented or not qualified」；
- pandora（SM8850，目前最完整，能跑 App、GPU 走 Turnip、音频 DSP 通了）：相机仍是 ❌
  【本仓 `docs/REPO-SURVEY.md` 的 2026-11-01 盘点，逐台列了状态】；
- 本地 `~/hackios/refs/` 全部仓库：零相机驱动、零相机文档；
- `ios-mondrian/NOTES.md` 自己写着「Audio, USB, modem, **cameras** and the GPU are untouched」。

**明确写：没有先例。** 连"某台别的 SoC 上让 iOS 相机 App 出过画面"的公开记录都不存在。
唯一相关的是 vphone（在 Apple 自己的虚拟化环境里**合成**一个相机 source 给 `cameracaptured`）——
那是"喂假帧"的可行性证据，不是"驱动真硬件"的先例。

---

## 7. 工作量与风险

### 7.1 分阶段（假设：1 名熟悉 XNU/IOKit 且能上机的开发者，可参考上游寄存器序列）

| 阶段 | 内容 | 完成判据 | 人月【估计】 |
|---|---|---|---|
| **A. 台账与供电** | TLMM 拉 4 组 LDO、MCLK 24MHz 出波形、camcc 上电（MMCX/MXC 投票 + titan_top GDSC） | 用示波器/万用表在模组 FPC 上量到 2.8/1.86/1.1V 与 24MHz | 0.5–1 |
| **B. CCI + sensor ID** | CCI0/1 队列式 I2C、读 4 颗 sensor 的 chip ID、写最小 init 表进 streaming | 日志打出 `OV64B40 id=0x56xx` 之类，且 sensor 进入 streaming 后 MIPI 有差分活动 | 1–2 |
| **C. CSIPHY + CSID** | 时钟/lane 配置、settle 时间、CSID 解包、TG 测图案先跑通 | 不接 sensor，TG 打出的图案能从 CSID 读到正确的 virtual channel / DT | 1–2 |
| **D. VFE/SFE RDI 出帧** | WM/总线写、camnoc、SMMU 映射、frame-done 中断、环形 buffer | 内存里出现一帧 complete 的 RAW10，尺寸/stride 自洽 | 2–4 |
| **E. 软件后端** | 去马赛克（bilinear 够）＋ 固定/简易 AE（读帧均值调曝光增益）＋ 简单 AWB | 一张能看出内容、亮度合理的 RGB 图，落到文件 | 1–3 |
| **F. iOS 侧接入** | 自研 IOService + IOSurface + patch `cameracaptured` 合成 source | Camera.app 预览出画面（哪怕 5fps） | 3–8 |
| 合计 | | | **8.5–20 人月** |

参照系：上游把 SM8550 的 camss（只到 RDI）做进 mainline 用了 **9 个版本的补丁系列**，
而且那是有硬件文档、有高通工程师参与的 Linux 内核【已验证，v9 00/16】。我们这边没有文档、
没有 ISP、还要自己造 IOKit 那一层。

### 7.2 "绕过 ISP 直出"到底能不能让 App 看到画面

**能，但只能看到"能认出来的画面"，不是"照片"。** 逐条说：

1. **技术上通**：VFE RDI 就是把 CSI-2 收到的 payload 原样写内存，格式是 RAW10（可配
   packed/plain）。上游已经证明这条路在 680 上能跑（`RDI_WM()` 那段）。
2. **需要自己补 ISP 的最小集**：去马赛克 + 黑电平 + 一个能看的白平衡。这三样在
   1080p 分辨率上跑 CPU 是可行的（一帧 1920×1080 Bayer，双线性去马赛克在现代
   AArch64 上是毫秒级）。
3. **带宽要先降下来**：OV64B 全尺寸 9248×6944、10bit、4 lane，15fps 就是
   `9248×6944×15×10/4 ≈ 2.4 Gbps/lane`，贴 D-PHY 上限；2×2 binning 后
   `4624×3472@30` 约 1.2 Gbps/lane，这才是有余量的工作点【计算，静态推导】。
   演示目标应定在 **binning + 30fps + 1080p 级输出**。
4. **画质上限很低**：没有 IPE/BPS 就没有多帧降噪、没有 HDR、没有 PDAF 对焦、
   没有 OIS 联动（actuator 要自己写 CCI 命令）。夜拍基本不可用。
5. **对焦/AE 的现实做法**：AF 用固定超焦距或一次性 VCM 扫描 + 对比度峰值；
   AE 用"帧均值 → 调曝光/增益"的闭环，跑在用户态守护进程里最省事。

所以降级方案的定位是：**"取景器能出画面"的演示里程碑**，不是"相机可用"。

### 7.3 风险清单（按"会不会让项目死掉"排序）

| # | 风险 | 为什么致命 | 缓解 |
|---|---|---|---|
| 1 | **SFE 是什么，没人说得清** | K60 的 CSIPHY→ 前端实际走 SFE0/1，上游没有对应驱动；若 SFE 是"带在线处理的 IFE 变体"，RDI 通路的寄存器序列可能完全不同 | 先用 VFE0（微距/超广那条线若走 VFE 就走 VFE；否则用 TG 测图案验证 VFE 本身），再对照下游 `cam_sfe` 源码 |
| 2 | **CSIPHY v2.1.3 + 小米私有变体** | 主摄/前摄正好落在被改过的两个 PHY 上，上游无表 | 从 `MiCode` 的 mondrian 内核源里取 csiphy 表；或先只做微距/超广（csiphy0/1） |
| 3 | **camcc 80+ 时钟要手写** | XNU 没有 clk 框架，写错一个分频器就整块不响应，且没有 clk_summary 可看 | 我们先有 `dump/clk_summary.txt`（Android 侧实测频率），可以逐条对标 |
| 4 | **SMMU 上下文/SID** | 相机走 apps_smmu 的多个 SID（DT 里 ife/sfe 用 `0x800/0x820/0x840/0x860/0xc00/0xc20/0xc40/0xc60`），配错就是 IOMMU fault 或无写 | 复用 ArmSmmu4K；先做 identity map 的 bypass 试验 |
| 5 | **`cameracaptured` 的隐性要求** | 它可能要 CMCaptureSource 的完整属性集（格式表、方向、稳定器…），少一项就不出预览 | 走 vphone 那条"用 Apple 自己的 `FigCaptureSourceCreateFromBacking`"的路子 |
| 6 | **没有 ISP 就没有 3A 数据** | 下游 App 期待 AE/AWB 统计量，我们只有裸帧 | 明说降级；自研守护进程补最小 3A |
| 7 | **热与功耗** | 相机是整机最耗电的块之一，K60 的散热按 Android 调度设计 | 演示限时、限分辨率 |
| 8 | **没有先例可对照** | 出错时没有"别人也这样"的参照 | 把每个阶段的判据写死（§10），失败就回退一格 |

---

## 8. 对 K60 的具体适配点

这一节是给动手的人看的清单，全部【静态推导】+ DT 行号。

### 8.1 上电时序（主摄 OV64B40）

```
1. RPMh: MMCX / MXC 投票（camcc 的 power-domains）      ← 我们已有的投票器扩两个资源
2. camcc AHB 时钟 + titan_top GDSC (0xadf31dc) 使能
3. TLMM gpio2  = 1  → camera_wide_dvdd_ldo  1.1V
4. TLMM gpio70 = 1  → camera_wide_avdd_ldo  2.8V
5. TLMM gpio97 = 1  → camera_wide_avdd2_ldo 1.86V
6. camcc 出 MCLK2 24MHz（DT: clocks = <&camcc CAM_CC_MCLK2>，`use-shared-clk`）
7. TLMM gpio126 = 1 → RESET0 释放
8. CCI0 master1，sensor 地址按 OV64B 的 7bit 地址（0x10 系列，需以 datasheet/厂商源为准）
9. 读 chip ID 寄存器确认
```

每步之间按 DT 的 `regulator-enable-ramp-delay = <0x64>`（100µs）留余量，实际建议 1–5ms。

### 8.2 四颗的差异化参数（照这张表写代码就不会串）

| 项 | 主摄 | 前摄 | 超广 | 微距 |
|---|---|---|---|---|
| CSIPHY 实例 | 2（**小米变体**） | 4（**小米变体**） | 1 | 0 |
| CCI | cci0 m1 | cci1 m0 | cci1 m0 | cci0 m0 |
| MCLK | MCLK2 / gpio102 | MCLK5 / gpio105 | MCLK1 / gpio101 | MCLK0 / gpio100 |
| RESET gpio | 126 | 127 | 24 | 128 |
| 需要的轨 | 1.1 + 2.8 + 1.86 | 2.8 | 2.8 | 2.8 + PMIC 1.86 |
| 附属 | actuator + flash + eeprom | eeprom | eeprom + flash | eeprom + flash |
| 建议优先级 | 3（最终目标） | 2（最简单，单轨） | 4 | **1（最先，csiphy0 未被改）** |

**微距 OV02B10 是最佳第一目标**：csiphy0 是原版 v2.1.3、CCI0 master0 独占、2MP 带宽低。
唯一麻烦是它需要 PMIC 的 `pm8350c_l1` 1.86V（要投票），以及 2MP 的 RAW 图很窄。
如果嫌 PMIC 麻烦，**前摄 OV16A1Q 只要一路 2.8V GPIO**，但落在小米改过的 csiphy4 上。

### 8.3 中断表（GIC SPI，DT 第二格）

CSIPHY0-5：`0x1dd/0x1de/0x1df/0x1c0/0x7a/0x59`；
CSID0/1/2：`0x1d0/0x1d2/0x280`；CSID_lite0：`0x1d4`；
VFE0/1/2：`0x1d1/0x1d3/0x281`；VFE_lite1：`0x168`；SFE：`0x1ce`；
CCI0/1：`0x1cc/0x10f`；LX7：`0x1cf`；JPEG：`0x1da`；CPAS-CDM：`0x1cd`。

### 8.4 我们已经有的、可以直接用的

| 已有件 | 用途 |
|---|---|
| RPMh 投票器（触摸 IOVDD 线，0240–0248） | 扩 MMCX/MXC/camcc 资源，直接复用 |
| `ArmSmmu4K`（SMMU-FEASIBILITY 已论证是 `qcom,qsmmu-v500` + adreno-smmu） | 相机 SID 映射，但要新增 context bank |
| TLMM/GPIO 操作（按键 0251 那条） | 所有 LDO 使能脚 |
| 用户态 patch 手艺（`ui_compat.c` 的 ObjC 替换） | 接法 B 的注入器 |
| `dump/clk_summary.txt`、`dump/regmap/`、`dump/regulator_summary.txt` | 逐条对标 Android 侧实际频率/电压，这是别人没有的作弊纸 |

---

## 9. 如果只做一件事

**做 §7 阶段 B 的前半：在 XNU 里把 CCI 跑起来，读出 OV64B40 的 chip ID。**

具体到可执行的判据：

1. 上电：TLMM gpio2/70/97 + MCLK2 24MHz + gpio126；
2. CCI0 master1 发一笔 16 位寄存器地址、8 位数据的读事务（OV 系列是 16bit index）；
3. 日志能稳定打出同一个 chip ID 两次以上（排除偶然）。

为什么是这件事而不是"先让 CSIPHY 出帧"：

- **它验证了这条链上所有"可能白干"的前提**：RPMh 投票、camcc GDSC、TLMM、MCLK、
  CCI 时序。这些通了，后面 CSIPHY/CSID/VFE 的失败才可能是"块本身的问题"，
  否则你永远分不清是通路没电还是寄存器写错。
- **它的失败模式最干净**：读不到 ID = 供电/时钟/地址三选一，一天内能二分完。
  而 CSIPHY 不出帧的失败模式有十几种（lane 顺序、settle、clock lane 反相、DT 不匹配…）。
- **它是所有后续工作的复用地基**：CCI 驱动 + sensor init 表在手，四颗 sensor 都能试，
  甚至可以只在用户态做"点 sensor、出 MIPI"，不需要 VFE 就能用示波器确认 CSI 有活动。
- **它的成本最低**：0.3–0.5 人月，且不需要任何固件、不需要 SMMU、不需要碰 ISP。

如果这一步一周内通不了，说明问题在 camcc/GDSC（本文档第 2.6 节），
那就先把 `dump/clk_summary.txt` 里相机相关的行一条条对上来 —— 那也是必须做的。

---

## 10. 待上机判据

| # | 问题 | 判据 | 需要的动作 |
|---|---|---|---|
| 1 | RPMh MMCX/MXC 的资源 ID 与我们投票器的映射 | 投票后 `camcc` 的 `0xade0000` 可读且非 0 | 加打印，读一个已知非零寄存器 |
| 2 | GDSC titan_top 是否真的上电 | 写 `0xadf31dc` 的 GDSC_ENABLE 后轮询 `GDSC_STATUS` 置位 | 需要 GDSC 位定义（从 camcc 驱动源码抄） |
| 3 | MCLK2 是否 24MHz | 示波器量模组 FPC 上的 MCLK 脚 | 上机 |
| 4 | CCI 读 OV64B40 chip ID | 连续两次相同且 != 0x0000/0xffff | 阶段 B 判据 |
| 5 | K60 vendor 分区里有没有 `CAMERA_ICP.elf` | `dump` 出 `super`/vendor 后 `find /vendor/firmware -iname 'CAMERA_ICP*'` | 有设备时补 dump |
| 6 | SFE 到底等不等于上游 VFE680 | 用 CSID 的 TG 测图案打到 SFE0，看能否在内存拿到帧；失败则改用 VFE0-2 同样实验 | 阶段 C/D |
| 7 | CSIPHY v2.1.3 与上游 2.1.2 的差 | 对比小米内核源里 csiphy 的初始化表 vs 上游 `camss-csiphy-3ph-1-0.c` 的 `gen2 2.1.2` 表 | 纯静态，可离线做 |
| 8 | `cameracaptured`（iOS 16.7.16）的 source 属性集 | 反汇编/日志拿到它序列化 source 时读的字段 | 有 iOS 用户态时做 |

---

## 11. 结论重述（给决策用）

| 问题 | 答案 |
|---|---|
| 能不能做 | 能做出"预览有画面"，做不出"相机可用" |
| 值不值得做 | 作为独立里程碑值得（阶段 A+B 约 1.5–3 人月，可复用为 CCI/I2C 基础设施）；作为"让 K60 变成能拍照的 iOS 设备"不值得（≥ 8.5 人月且画质不可接受） |
| 最大不确定性 | SFE 的寄存器语义；没有它 CSIPHY→内存这条路可能整个要重画 |
| 最大的现成便宜 | CSIPHY/CSID/VFE0-2 的地址与上游 SM8550 **完全一致**，寄存器序列可以直接抄 |
| 优先级建议 | 排在触摸收口和音频之后（音频有 pandora 证明过 DSP 链路，成功率更高） |
| 如果只做一件事 | CCI 读 chip ID（§9） |

---

## 来源

**上游内核（mainline，2026-11 的 master）**

camss 驱动目录（含 `camss-vfe-680.c` / `camss-csid-680.c` / `camss-csiphy-3ph-1-0.c`） ( https://github.com/torvalds/linux/tree/master/drivers/media/platform/qcom/camss )

camss.c（compatible 表与 `enum camss_version`，确认无 sm8450） ( https://github.com/torvalds/linux/blob/master/drivers/media/platform/qcom/camss/camss.c )

camss-vfe-680.c（RDI-only 的 TODO 注释） ( https://github.com/torvalds/linux/blob/master/drivers/media/platform/qcom/camss/camss-vfe-680.c )

sm8450.dtsi（只有 camcc 与 cci，无 camss 节点） ( https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/sm8450.dtsi )

sm8550.dtsi（camss 节点与 csiphy/csid/vfe 地址表） ( https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/sm8550.dtsi )

i2c-qcom-cci.c（`qcom,sm8450-cci` 已在支持列表） ( https://github.com/torvalds/linux/blob/master/drivers/i2c/busses/i2c-qcom-cci.c )

media/i2c Makefile（有 ov08d10，无 ov64b40/ov02b10/ov16a1q） ( https://github.com/torvalds/linux/blob/master/drivers/media/i2c/Makefile )

Qualcomm Camera Subsystem driver 文档（camss 只做 CSIPHY/CSID/VFE-RDI 与原始数据落盘） ( https://docs.kernel.org/admin-guide/media/qcom_camss.html )

**上游邮件列表与提交**

media: qcom: camss: Add VFE680 support（提交信息写明 "as found on sm8450, x1e"） ( https://lkml.indiana.edu/hypermail/linux/kernel/2501.2/04204.html )

同一提交的 raspberrypi 镜像（含文件变更统计） ( https://github.com/raspberrypi/linux/commit/727970e9725c35366f501a890584e466ae34224a )

[PATCH v9 00/16] media: qcom: camss: Add sm8550 support（CSIPHY 3ph Gen2 v2.1.2 + CSID/VFE 680 系列） ( https://lwn.net/Articles/1004967/ )

[PATCH v4 00/22] Add support for the SDM845 Camera Subsystem（camss 只实现 CSIPHY/CSID/部分 VFE 的历史说明） ( https://www.mail-archive.com/linux-kernel@vger.kernel.org/msg2468566.html )

arm64: defconfig: Enable camcc and videocc on Qualcomm SM8450+（SM8450 只到 clock controller 这一层） ( https://lists.openwall.net/linux-kernel/2025/06/12/172 )

**Android / 厂商侧**

Qualcomm Spectra 相机内核模块（LineageOS 的 sm8450 modules 仓库，`camera_main.c` 子模块表） ( https://github.com/LineageOS/android_kernel_qcom_sm8450-modules/blob/lineage-20/qcom/opensource/camera-kernel/drivers/camera_main.c )

同仓库的 CAMERA.LA.2.0.r1 WAIPIO 合并记录（证明 waipio 相机内核在持续更新） ( https://github.com/LineageOS/android_kernel_qcom_sm8450-modules/commit/6f251b507268feb562e02e73dc7098a8f6e495ed )

sm8450 devicetrees 里的 waipio 相机 DT（`cam_smmu` / `cam_smmu_icp_resv_region` / iova 映射） ( https://review.lineageos.org/c/LineageOS/android_kernel_qcom_sm8450-devicetrees/+/499356/1/qcom/camera/waipio-camera.dtsi )

vendor 分区相机固件清单（`vendor/firmware/CAMERA_ICP.elf`） ( https://review.lineageos.org/c/LineageOS/android_device_oneplus_sm8350-common/+/322770/10/proprietary-files.txt )

SM8550 机型的相机固件清单（`CAMERA_ICP.elf` / `.b16` / `.mdt` 等分段） ( https://review.lineageos.org/c/LineageOS/android_device_motorola_sm8550-common/+/412547/2/proprietary-files.txt )

小米开源内核（mondrian-s-oss 分支，含 techpack 目录） ( https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/mondrian-s-oss )

小米开源内核缺 techpack 的问题单（官方源里 audio/camera/display techpack 缺失） ( https://github.com/MiCode/Xiaomi_Kernel_OpenSource/issues/5388 )

小米/kernel 组织的 camera-kernel 独立仓库（分支按机型，可作为 sensor/CSIPHY 表来源） ( https://github.com/MiCode/vendor_qcom_opensource_camera-kernel )

**固件仓库**

Debian `firmware-nonfree`（即 linux-firmware）的 `qcom/` 目录：48 个文件、0 子目录、无任何相机固件 ( https://sources.debian.org/src/firmware-nonfree/latest/qcom/ )

**iOS 侧**

iPhone X / iOS 11.2.6 的 IOKit 类表（`AppleH10CamIn`、`FrameReceiver`、`AppleJPEGDriver`、`AppleM2ScalerCSCDriver`） ( https://github.com/nixerr/nixerr.github.io/blob/master/static/iPhoneX_11.2.6_iokit_classes/iokit.csv )

vphone 的 `libvcamcaptured`（AVFoundation → FigCaptureSourceRemoteCopyCaptureSources → cameracaptured 的完整链路，及"用 FigCaptureSourceCreateFromBacking 合成 source"的做法） ( https://gitea.yiem.net/yiem/vphone-cli/raw/commit/2d283e76fcdfc6d654549c0c6b9d3097ba396a08/VPhoneGuestComponents/VCamCaptured/libvcamcaptured.m )

Apple 开发者论坛：mediaserverd 负责相机 IOSurface 像素缓冲与相机控制 ( https://developer.apple.com/forums/thread/7848 )

vcamios 的 tweak 架构说明（在 mediaserverd / cameracaptured 里替换像素缓冲） ( https://deepwiki.com/vinhnv2507/vcamios/2-core-tweak-architecture )

**移植先例（都写着 not implemented）**

UniXNU pong（Nothing Phone 2，SM8475）："Wi-Fi, Bluetooth, camera, audio, sensors | not implemented" ( https://unixnu.com/devices/pong )

UniXNU umi（小米 10，SM8250）："camera ... Not implemented or not qualified" ( https://github.com/reAAAq/UniXNU-umi )

iOS 16 on LG V60（SM8250）：进展只到 GUI + 触摸 + Wi-Fi/BT ( https://github.com/ch1tmdgus/ios-v60 )

UniXNU nx769j（SM8650，另一条路的完成度参照） ( https://unixnu.com/devices/nx769j )

HakiOS 的 Metal 计划（提到"相机的取景器"是 Metal 客户端之一，仍未做） ( https://hackios-web.vercel.app/en/docs/metal-plan )

**K60 硬件规格**

mondrian 设备规格（POCO F5 Pro / Redmi K60，64MP OIS + 8MP 超广 + 2MP 微距 + 16MP 前摄） ( https://github.com/mondrian-development/android_device_xiaomi_mondrian )

Redmi K60 影像规格（OV64B40 / OV08D10 / OV02B10 / OV16A1Q） ( https://www.ithome.com/0/664/834.htm )

Redmi K60 相机参数明细（主摄 1/2"、F1.79、OIS；超广 1/4"；微距 200 万） ( https://www.jiuphone.com/sjwt/22620.html )

**本机证据**

`~/hackios/dump/live.dts`（34801 行，全部 DT 行号引用自这里）

`~/hackios/dump/clk_summary.txt`、`~/hackios/dump/regulator_summary.txt`（Android 侧实测值，用于对标）

`~/hackios/refs/UniXNU/drivers/`（十三个组件，无相机）
