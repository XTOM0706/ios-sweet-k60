# pandora（小米 17 Pro / SM8850）拆解：哪些能照搬、哪些要改（2026-11-01）

仓库：`ShueiTsuki/data-a-live`（2026-10-30 起归档只读，接续在
`yechenan/data-a-live`），本地克隆 `~/hackios/refs/data-a-live`（13 MB，
334 个补丁）。目标机 iPhone X 身份、iOS 16.7.16、xnu-8796。

## 0. 先纠正一个前提

**pandora 的相机也没做。** README 原话：
「Wi-Fi、蜂窝网络、相机、背屏 还没做。iOS 的相机要用 A11 的 ISP 和苹果的
私有接口，很难接到高通的相机硬件上」；HANDOFF（10-30）的"还没做"清单里
也列着相机。所以它是「能把 App 跑起来 + GPU 全合成」的完整样本，不是
「相机可用」的样本。相机路线见 `CAMERA-FEASIBILITY.md`。

另一个修正：README 说"喇叭没声音"，但 HANDOFF（更新到 10-29）说两个喇叭
已经能放系统声音（k0296–k0298），以 HANDOFF 为准。

## 1. 一句话结论

**能照搬的是架构和工具链，不能照搬的是硬件层。** 它的价值在于把
「xnu + 自写 KMD + Turnip + Metal 翻译层」这条链在真机上跑通了，
每一步都有可复查的日志和补丁；K60 与它的差别集中在
GPU 代号（Adreno 730 vs 840）、SMMU 页大小（4K）、以及 SoC 平台件
（SM8475 vs SM8850）。

## 2. 它的栈长什么样

| 层 | 内容 | 文件 |
| --- | --- | --- |
| 启动 | `fastboot boot` 临时启动；根文件系统是安卓 data 分区里一个固定位置的 16 GiB 文件（`tools/pinfile`） | `tools/boot_ios.sh`、`tools/pinfile` |
| 引导菜单 | 写一次 `boot_b`，开机按音量上进 iOS | `bootmgr/` |
| 内核 | xnu-8796 + 334 个补丁（119 个 pandora 专用，80 个 sweet 继承，其余音频/功放/温控/USB/GPU） | `xnu/patches/` |
| 驱动 | 直接编进内核的 C/C++ 源码 | `wsl/*.c`、`wsl/*.cpp`、`wsl/adreno/` |
| 用户态桥 | NativeMetal 插件 + 改过的 metal2vulkan + Turnip | `docs/GPU_PLAN.md` |
| 日志 | 内核日志写固定文件，回到安卓后用工具取；USB 串口实时看 | `pandora_log*.c` |

规模感：`pandora_gpu.c` 4047 行、`PandoraAdrenoGpu.cpp` 650 行、
`PandoraAudio.cpp` 1067 行、`pandora_dpu.c` 968 行，SMMU 那一组
`wsl/adreno/ArmSmmu4K*.c` 另算。

## 3. GPU 链路（最值得抄的部分）

分五段，每段都能在 `docs/GPU_PLAN.md` 找到对应日志编号：

1. **内核 GPU 驱动**（`wsl/pandora_gpu.c`）：上电 → GMU 装固件 →
   SMMU → CP（命令处理器）→ zap 着色器 → 1 GiB 显存池 →
   对用户态暴露 **NativeAdreno ABI v1**。
2. **Vulkan**：Mesa 的 Turnip（`tu_knl_drm_msm` 那套接口被换成
   NativeAdreno ABI）。
3. **Metal 桥**：UniXNU 的 **NativeMetal** 插件（`AdrenoMetal` 仓库）
   + 他们改过的 **metal2vulkan**。
4. **合成**：backboardd 用 GPU 合成整个界面（`--clients "*"` 可以放开到
   其它进程），系统界面 110 fps / 120 Hz 屏。
5. **显示**：`pandora_dpu.c` 直接拿 GPU 画的 buffer 扫描输出（早期是
   CPU 每帧拷整屏，4–5 ms）。

关键数字（照抄时对表用）：GPU 寄存器基址 `0x3d00000`、GMU `0x3d37000`、
rscc `0x3d50000`、UBWC v6、频率 160–1200 MHz 共 18 档、
一帧 GPU 合成 3–4 ms（726 MHz）。

已知坑（都有补丁编号，抄代码时一起抄）：

- Turnip 把一次提交拆成 16 段导致 BR/BV 互等卡死 → 改成一次 128 段（0194）
- 每次提交刷全部导入图像缓存，一帧 27 ms → 去掉（0195）
- GPU 固定最低频 → 开可选档（0196）、再做 DCVS（0198）
- IOSurface 平面查询返回"每行 0 字节"，插件拒绝建图 →
  插件侧按 64 字节对齐推算（`tools/metal/adapt_ios16.py`），
  根因是内核 IOSurface 驱动缺格式表

## 4. 为什么 App Store 应用打不开

README 写得很直白：**大多打不开，这些应用要靠苹果芯片里的安全模块解密**。
能开的是系统自带应用（设置、控制中心、相册）。这对我们有直接参考价值：
K60 这条线现在连"装"都谈不上，先对齐"自带应用能开"这个水位。

## 5. 与 K60 的对照表

| 部分 | pandora | K60 现状 | 结论 |
| --- | --- | --- | --- |
| xnu 版本 | 8796 + 334 补丁 | 8796 + 202 补丁 | 补丁体系同源，可直接对照 |
| 显示 | 自写 DPU 驱动，GPU 直出 | AppleCLCD 通路 + CPU 合成 | **不用抄**，我们已通 |
| 触摸 | `pandora_touch.c`（I2C/SPI 依平台） | 0240–0248（QUPv3 SE4 + RPMh） | 各写各的，接口层可借 |
| 按键 | `pandora_keys.c`（PON） | 0251（PON periph 0x13 + pm8350 GPIO6） | **思路一致，可交叉验证** |
| GPU KMD | `pandora_gpu.c` 4047 行，gen8 | 只有 GMU CX（0188） | **架构照搬、寄存器全换** |
| SMMU | `ArmSmmu4K*.c`，4K 页 | 无 | **直接可用**（同名仓库就在咱们 `refs/ArmSmmu4K`） |
| Vulkan | Turnip + NativeAdreno ABI | 无 | ABI 换代号，逻辑可抄 |
| Metal | NativeMetal 插件 + 改版 metal2vulkan | 无 | **可照搬**（插件是同一份 UniXNU 代码） |
| 音频 | ADSP + GPR/GLINK + 功放（喇叭已通） | 无 | 阶段 2 再抄 |
| 相机 | **没做** | 没做 | 都要新写 |
| 根文件 | data 分区上 16 GiB 固定文件 | 我们自己 dd 到 userdata | 它的 `pinfile` 思路更干净 |

## 6. 能直接拿的三样

1. **SMMU 4K 那一组**（`wsl/adreno/ArmSmmu4K*.c`）：我们的 `refs/ArmSmmu4K`
   就是同一份东西，K60 也需要 4K 页 SMMU，直接引。
2. **NativeMetal 插件与改版 metal2vulkan**：与 SoC 无关，只跟 Metal 版本
   和 Turnip 接口有关。
3. **工具链**：`tools/pinfile`（把根镜像固定在 data 分区某处）、
   `tools/gpu_test.sh`（跑 N 秒后自动重启回安卓并取内核日志）、
   `wsl/local_build.sh`（WSL 里只重编改过的文件，省掉每次 7 分钟 CI）。

## 7. 必须改的

- GPU 寄存器表整个换：Adreno 730 是 gen7（`0x07030002`），GMU 版本、
  CP 固件名（`a730_*` vs `gen80200_*`）、zap 位置、频率表全不同。
- 我们的 GPU 需要 **SMMU 多 ASID** 和 per-process 页表（它到 0332 才做到
  15 个客户端、之前共用一个地址空间）。
- 显示不用抄：我们有 AppleCLCD 那条路，只要把"GPU 画的 buffer"喂给它。

## 8. 迁移顺序（结合设备不在的现状）

1. 先拿 SMMU + NativeAdreno ABI 头文件，把 K60 的 GMU 从 "CX only"
   推进到 CP 起来（内核侧，可离线写）。
2. 把 Turnip 的 KNL 后端对着我们的 ABI 改（离线）。
3. 上机验证 GPU 计算 → 三角形 → 合成（要设备）。
4. 用户态：先跑它的 `conformance.py`，看我们缺哪些 Metal 方法。

## 9. 待上机（设备不在，攒着）

- `img/xnu-probe-all.img`、`img/xnu-rpmh-touch.img`（见 UNIWORK 待办）
- GPU：等第 1 步代码落地后编一颗带 `sweetadreno=1` 的，看 GMU → CP 日志
