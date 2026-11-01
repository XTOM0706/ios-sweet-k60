# 手上的仓库能干什么：装软件 / 相机 / 可复用件盘点（2026-11-01）

## 结论

1. **"能装软件"（跑起 App）目前只有一条线做到**：pandora（小米 17 Pro，
   SM8850，`ShueiTsuki/data-a-live`）。它到 iOS 16.7 主屏，App 能跑，
   GPU 走 Adreno 840 + Turnip + Metal 翻译层，音频 DSP 也通了。
2. **相机一个都没有**：本地仓库、UniXNU 全部设备文档、ios-sweet 侧全部
   是 `not implemented` / `pending`。这不是我们没找对仓库，是整条生态都
   没做出来。
3. 对 K60 最该复用的不是 pandora 的代码，而是 **pong**（同一颗 SM8475）
   ——我们现在这颗内核就是从它那套长出来的；pandora 值得抄的是
   "GPU + 音频 + 用户态"那条链路的做法。

## 本地仓库一览

`~/hackios/refs/` 下：

| 目录 | 补丁数 | 性质 | 对我们的价值 |
| --- | --- | --- | --- |
| `UniXNU/` | 71 | 上游主仓（含全部设备文档、驱动组件源） | 组件库 + 设备档案，查"有没有人做过"的第一站 |
| `UniXNU-pong/` | 109 | Nothing Phone 2（SM8475） | **同 SoC**，触摸/GENI/RPMh 代码来源 |
| `ios-sweet-upstream/` | 81 | dipa-studio/ios-sweet（SM7150 系） | 我们仓库的上游，补丁编号体系一致 |
| `ios-mondrian/` | 202 | 同 K60 的另一条移植线 | 触点/引脚/时钟的交叉验证 |
| `ios-mayfly/` | 81 | SM7150 另一台 | 参考价值低 |
| `AdrenoMetal/` | 16 | Metal→Vulkan 桥的 XNU 侧 | GPU 转译的现成实现 |
| `Adreno740/` | 0 | GPU 用户态组件 | 只面向 740 |
| `sm8550-common/`、`ArmSmmu4K/` | 2 / 0 | 平台件 | 有限 |

## 上游设备矩阵（挑有用的）

| 设备 | SoC | 主屏 | App | GPU | 相机 | 备注 |
| --- | --- | --- | --- | --- | --- | --- |
| pandora（小米 17 Pro） | SM8850 | ✅ 16.7 | ✅ | ✅ Turnip + 翻译层，~110 fps | ❌ | 唯一"能装软件"的完整样本 |
| pong（Nothing Phone 2） | **SM8475** | ✅ 16.6.1 | 未陈述 | 源码在内，硬件加速未确认 | ❌ | 同 SoC，我们现在的基底 |
| tb128fu（联想平板） | SM6225 | ✅ 17.7.11 | 未陈述 | "GPU hand-off" 出帧 | ❌ | 软件渲染 + 交棒 |
| umi（小米 10） | SM8250 | 未验证 | ❌ | ❌ | ❌ | |
| j716f / oneplus13t | 各家 | UEFI 阶段 | ❌ | ❌ | ❌ | 文档里的 "camera" 指拿相机拍屏幕，不是相机功能 |

来源：`~/hackios/refs/UniXNU/docs/devices/*.md`。

## "能装软件"到底指什么

主屏能起 ≠ App 能装。要跑第三方 App 至少要：AMFI/签名绕过、沙箱（container）
落盘、dyld 共享缓存、以及能分配足够的可写 Data。我们这条线已经有
`amfi_get_out_of_my_way=1`、可写 userdata 镜像、launchd 能起 SpringBoard，
缺的是"装"这一步的完整验证（pandora 是唯一有 App 运行记录的项目）。

## 相机：为什么一处都没有

- UniXNU 的驱动组件只有 Adreno740 / AdrenoMetal / AppleKeyStoreState /
  ArmCpu / ArmDTPlatform / ArmVirtPlatform / DarwinBoot / GopFramebuffer /
  QcomGeniSpi / RamBlock / SoftwareAES / VirtioMmioBlock / VirtioMmioConsole。
  没有 ISP / CSIPHY / CCI / sensor 任何一块。
- 设备文档里相机一栏清一色 `not implemented`。
- 结论：相机不是"找个仓库搬过来"，是要从 CSIPHY/CSID 出帧开始新写。
  具体路线、K60 的 sensor/供电清单、以及工作量估计，见
  `docs/CAMERA-FEASIBILITY.md`。

## 对 K60 的复用清单

| 能力 | 现成可复用 | 还缺 |
| --- | --- | --- |
| 显示 / 锁屏 | ✅ 我们自己的 0218 一路 | — |
| 触摸 | ✅ 0240–0248（GENI/RPMh/引脚） | IOVDD 投票上机验证 |
| 按键 | ✅ 0251（PON periph 0x13 + pm8350 GPIO6） | 上机对位号 |
| 壁纸 | ✅ 0249（先铺后叠） | 根卷带 bgra（已注入） |
| GPU | 内核侧只有 GMU CX；`AdrenoMetal` 桥有现成源码 | KMD（CM3/CP/fence/SMMU）、Turnip 移植 |
| 音频 | 无 | 从 pandora 抄 DSP 链路 |
| 相机 | 无 | 整条新写，见相机文档 |
| Wi-Fi/蓝牙 | 无 | 独立大工程 |

## 移植建议（按性价比）

1. **先把触摸收口**（等设备）：IOVDD → ID → 报告 → HID。
2. **GPU 抄 pandora 的架构，不抄代码**：它的价值是证明
   "Turnip + Metal 翻译层 + 内核 KMD" 这条路能到 110 fps，说明我们
   昨晚那份 `GPU-TRANSLATION.md` 的结论方向没错，成本在 KMD。
3. **相机单独立项**：先把 CSIPHY/CSID 出第一帧作为里程碑，不碰 ISP。
4. 音频优先级排在相机之前——pandora 已经证明 DSP 那条链路可行。

## 待上机（设备不在时攒着）

- `img/xnu-probe-all.img`：RSC / PON / GPIO6 只读探测
- `img/xnu-rpmh-touch.img`：IOVDD 投票 + SPI + Goodix ID
- 详见 `UNIWORK/k60-ios-2026-11-01/TO-DO-上机.md`
