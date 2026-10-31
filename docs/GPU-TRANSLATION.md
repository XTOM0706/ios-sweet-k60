# GPU 转译层可行性评估：Metal → metal2vulkan → Vulkan/Turnip → K60（Adreno 730）

> 写于 2026-10-31。纯离线调研：**没有上机，没有 adb/fastboot，没有改内核代码，没有推送**。
> 范围：只回答"iOS 的 Metal 通过转译层跑在 Adreno 730 上"这条路值不值得走、要走需要什么。
> 证据分级沿用仓库习惯：**[实锤]** = 有直接代码/数据/可复查的公开源；
> **[强推断]** = 多条间接证据收敛；**[未验证]** = 只有结构类比，必须上机才能定。

---

## 0. 结论先行

### 0.1 一句话

**技术上成立，但它不是"接一个转译层"，而是"在 XNU 里重写一个 Adreno KMD（GMU/CP/SMMU/fence），
再把 Mesa Turnip 移植到自家内核 ABI"。** 转译层（metal2vulkan）本身是最便宜的一段——它是
纯用户态、只要求 Vulkan 1.2 + `timelineSemaphore`；真正贵的是它脚下的东西：
我们连 CM3 出复位都还没做到（放 `CM3_SYSRESET=0` 即整机复位），CP ring 与 fence 一行没有。
上游已经有人把 A740 这条路走完了（UniXNU/AdrenoMetal + Adreno740 + turnip-darwin-kmd），
可抄的代码约 2 万行；A730 只有社区自述，没有可复现的日志。

**建议：现在不做。** 把 GPU 当"下一个大件"之前，先把 CM3/CP 这条腿走通（见 §7 的"明天只做一件事"），
它是所有上层（HFI → CP → fence → Turnip → metal2vulkan → Metal.framework）唯一的硬前提，
而且不依赖任何上游代码。

### 0.2 三个层次

| 层次 | 要什么 | 我们缺什么 | 量级（全职单人） |
|---|---|---|---|
| (a) Vulkan 能跑 | 一个能提交 IB 并回 fence 的 KMD + 把 Turnip 的 KNL 后端接到该 ABI | GMU 出复位、CP ring、per-submit fence、SMMU 多 ASID、IRQ、OPP 投票 | **8–12 周**（抄得到设计）/ **4–6 个月**（按我们现在的基线） |
| (b) metal2vulkan 接上 | 一个 iOS 的 Metal 设备实现（`MTLAddDevice` 插件）+ IOSurface/scanout 通路 | Metal.framework 私有 SPI 的一大片方法、命令缓冲语义、pipeline cache | **3–6 个月**（参考实现 1.15 万行且只覆盖 CA/render server） |
| (c) 现实工作量 | (a)+(b)+A730 回归+性能 | 我们的内核基线比参考实现差一整代 | **单人 9–13 个月**（只覆盖 UI）；把第三方 app/compute 纳入到 18 个月；两人 6–9 个月。**主要风险不在代码量，在 RSCC/时钟域与 CM3 预序列** |

### 0.3 和"官方说法"的对照

- 上游 UniXNU 的组件页把 AdrenoMetal 的**已验证设备写成只有 sheng（A740/SM8550）**；
  其 README 的 News 也只宣称 sheng 有 GPU 加速。**[实锤]**
- A730 的说法来自设备仓 `Chengdong-Yu/UniXNU-pong` 的 README（中英两版一致）：
"Adreno 730 with Pong firmware, Turnip and Metal integration **used for desktop rendering**;
  remaining paths need work"。**这是自述，没有日志、没有 CI、没有复现件。**
- 所以"中文页写已接通 / 英文页写未确认"不是真矛盾，而是**两份文档的验证等级不同**：
  组件页只承认 sheng；设备页记录的是社区自述。按仓库规矩，A730 应记为 **[未验证]**。

---

## 1. 证据（一）：metal2vulkan 到底是什么状态

### 1.1 谁在维护、什么阶段

| 项 | 值 | 来源 |
|---|---|---|
| 仓库 | `steelbrain/metal2vulkan` | github.com/steelbrain/metal2vulkan |
| 维护者 | Anees Iqbal（`steelbrain`）：contributors 里 987 次提交中 986 次是他，另一人 1 次 | `contributors` API |
| 建仓 | 2026-07-22 | repo API |
| 最近提交 | 2026-10-29（`219f0136` "Re-pin the runtime sources at the landing tip"） | commits API |
| 规模 | 301 star / 32 fork / 2 个 open issue（#6 许可证、#8 合并请求） | repo API |
| 版本 | 仓库自称 0.2.0 beta；crates.io 上只有 0.1.0，且 README 明说 0.1.0 是"unrelated earlier design" | README / crates.io |
| 许可 | `GPL-2.0-or-later OR LGPL-3.0-or-later`（双许可，可选 LGPL） | README |

**[实锤]** 它是一个**活的、单人主导的 beta 项目**，2026-07 才起，5 个月里 API 还在变
（README 自己写 "may change during the `0.x` series. Version your cached outputs."）。

### 1.2 它支持哪些 GPU / 驱动

**它不"支持 GPU"，它支持"一个被描述出来的 Vulkan 设备"。**

- 翻译器要一个 Khronos profiles JSON（`--profile device.json`），按该设备的
  feature/limit 编译 SPIR-V；仓库里**没有 Adreno profile**。
  现有 profile 名单：AMD 780M（RADV）、AMD RDNA2/3、Intel ARL/Iris Xe/UHD、
  NVIDIA Pascal/RTX50、MoltenVK M3、lavapipe、SwiftShader、`vk12-floor`、`roadmap-2022`。
  **[实锤]**（`crates/metal2vulkan-validation/profiles/`）
- 运行时（`metal2vulkan-runtime`，Metal API on Vulkan）通过 `ash` 打开 Vulkan loader，
  **要求 Vulkan 1.2 且必须支持 `timelineSemaphore`**。
- 全仓（README + docs + 10 条 issue）**0 次**出现 `adreno` / `turnip` / `freedreno` /
  `qualcomm` / `kgsl`。**[实锤]**（对 5 份文档与 issue 列表 grep）
- GitHub 全站 `metal2vulkan adreno` 只有 2 条命中，都与本项目无关：
  UniXNU 的 Mali 组件提议 issue、pong 仓库的一个清理 PR。**[实锤]**

### 1.3 它依赖哪些 Vulkan 能力

| 能力 | 必需/可选 | 说明 |
|---|---|---|
| Vulkan 1.2 | **必需** | runtime README："A device needs Vulkan 1.2 and `timelineSemaphore`" |
| `timelineSemaphore` | **必需** | 同上；Turnip 在 A730 上支持（见 §2.2） |
| `dynamicRendering` / `synchronization2` / `maintenance4` | 强烈依赖 | 翻译器按设备能力决定是否走 render pass / 兼容路径 |
| `VK_EXT_external_memory_host` | 可选 | `new_buffer_with_bytes_no_copy` 走它；IOSurface 零拷贝导入的关键。驱动不支持时它会退化成"拷贝"，并公告 `copied-host-pages` |
| `VK_EXT_pci_bus_info` / `VK_EXT_physical_device_drm` | 可选 | 只用于设备 identity；Darwin 侧两个都没有，会走 `system_default_identity_with`（不影响渲染） |
| MSL 编译器 | **没有** | runtime README 明写："there is no MSL compiler"；输入必须是 metallib（内含 AIR），裸 AIR 要用 `Library::from_air` |

**这一条最关键：metal2vulkan 不做 MSL→AIR。** 它只吃 AIR。iOS 上的 `MTLCompileOptions`
运行时编译由系统的 `MTLCompilerService` 负责，那是 CPU 侧服务——理论上我们的移植里它
照样能产出 AIR，但**它是否能在我们的 rootfs/沙箱里跑起来，属于 [未验证]**。
系统框架自带的 `default.metallib` 之类是预编译好的 AIR，不受影响。

### 1.4 有没有人在 Adreno 730 上跑通过

**没有公开证据。** 分三层说清楚：

1. **metal2vulkan 仓库内**：无 issue、无 PR、无 CI、无 A730 痕迹。0 条。
2. **谁真的在用它**：只有 `UniXNU/AdrenoMetal`（15 star，2026-10-20 建仓）。
   它的 README 只有两句：*"The frontend uses metal2vulkan and Turnip with the kernel GPU
   interface and selected OS ABI. GPU composition has been verified on sheng with A740."*
   **[实锤]**
3. **A730 的自述**：`UniXNU-pong`（SM8475，同 K60 的 SoC）README 写"used for desktop
   rendering; remaining paths need work"。同仓 `migration/format2/README.zh.md` 里
   GPU 段落也只是"已有桌面硬件渲染记录"一句。**没有日志文件、没有截图时间戳、没有复现步骤。**

### 1.5 AdrenoMetal 这一层实际有多大（决定 (b) 的成本）

| 文件 | 行数 | 作用 |
|---|---|---|
| `NativeBridgeMetalFrontend.mm` | 2,075 | ObjC++ 的 Metal 前端 → metal2vulkan runtime 的桥 |
| `bridge/NativeMetalObjects.rs` | 2,208 | Metal 对象（device/queue/buffer/texture/encoder…）的 Rust 侧 |
| `HostMetalCommandServer.mm` | 658 | 命令缓冲跨进程服务 |
| `NativeTurnipSmoke.h` | 653 | 直接 `dlopen` ICD、`vk_icdGetInstanceProcAddr` 的冒烟测试 |
| `NativeAdrenoMetalPlugin.mm` | 451 | `IOAcceleratorES → MetalPluginName → NSBundle → principal class → initWithAcceleratorPort:` 的插件入口 |
| 其余（wire/scanout/present/cache…） | ~5,500 | IOSurface 导入、scanout 交接、pipeline cache 导出 |
| **合计（排除 host 测试工具）** | **≈11,500** | |

**[实锤]** 也就是说，"Metal API 这一层"是**一万行量级**的私有 SPI 适配，而且只覆盖
CoreAnimation / render server 用到的那一片；游戏用的 compute/indirect/argument buffer
还在 `METAL-COVERAGE` 之外。

### 1.6 它还带 16 个 metal2vulkan 补丁

`AdrenoMetal/bridge/patches/` 里 16 个（16 KB～24 KB 量级）：
`0001-optional-constant-output-members`、`0002-resource-wait-storage-dependencies`、
`0003-cache-device-entry-point-priming`、`0004-fold-constant-global-field-loads`、
`0005-keep-module-decodes-and-profile-fingerprints`、`0006-skip-draws-without-a-pipeline`、
`0007-accept-two-plane-rgb-surfaces`、`0008-alias-padded-rows-of-written-textures`、
`0009-keep-final-pass`、`0010-fewer-allocations-per-draw`、`0011-keep-draw-commands`、
`0012-keep-draw-lists`、`0013-skip-unchanged-rebinds`、`0014-map-tracker-keys`、
`0015-two-plane-shadow-as-memory-lies`、`0016-tile-stores-within-rows`。

**[强推断]** 其中 `0004`/`0005` 是**正确性/可用性**补丁（QuartzCore uber shader 特化、
module decode 复用），不是纯优化。这意味着"上游 metal2vulkan + 我们的 Turnip"不能直接用，
要么跟他们的补丁集走，要么自己重做一遍。

---

## 2. 证据（二）：Turnip 对 Adreno 730 的支持程度

### 2.1 A730 在 Mesa 里的身份

`src/freedreno/common/freedreno_devices.py`：

```python
add_gpus([
        # These are named as Adreno730v3 or Adreno725v1.
        GPUId(chip_id=0x07030002, name="FD725"),
        GPUId(chip_id=0xffff07030002, name="FD725"),
    ], A6xxGPUInfo(
        CHIP.A7XX,
        [a7xx_base, a7xx_gen1, GPUProps(QCTDD09112208 = True)],
        num_ccu = 4, tile_align_w = 64, tile_align_h = 32,
        tile_max_w = 1024, tile_max_h = 1024, num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024, wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16, highest_bank_bit = 16,
        raw_magic_regs = a730_raw_magic_regs,
    ))
```

**[实锤]** K60 的 `qcom,chipid = 0x07030002` 正是这一条，落地为 `CHIP.A7XX + a7xx_gen1`。
注意它被命名为 `FD725`（注释："Adreno730v3 or Adreno725v1"），另有一条
`0x07030001 → FD730`（"KGSL, no speedbin data"）。**这就是 issue #10274 的根源**：
同一颗 8+ Gen 1 在不同 speedbin 下报不同 chip id，驱动按 chip id 选表。

### 2.2 Vulkan 版本与关键扩展：比预期好得多

`src/freedreno/vulkan/tu_device.cc`：

```c
#define TU_API_VERSION VK_MAKE_VERSION(1, 4, VK_HEADER_VERSION)
...
((pdevice->info->chip >= 7) ? TU_API_VERSION : <更低>)
```

**[实锤]** Turnip 在 a7xx 上对外报 **Vulkan 1.4**，远高于 metal2vulkan 要求的 1.2。
扩展表里与本任务相关的项（除注明外都是无 gen 门控的 `= true`）：

| 扩展 | 状态 | 对转译层意味着什么 |
|---|---|---|
| `VK_KHR_timeline_semaphore` | true | metal2vulkan runtime 的硬要求，满足 |
| `VK_EXT_descriptor_buffer` | true | **"descriptor buffer 是缺口"这个说法不成立** |
| `VK_EXT_graphics_pipeline_library` | true | Metal 的 pipeline 提前编译可映射 |
| `VK_KHR_dynamic_rendering` + `_local_read` | true | Metal 的 render pass 可直接映射 |
| `VK_KHR_synchronization2` / `maintenance4..9` | true | 事件/fence 语义够用 |
| `VK_KHR_push_descriptor` / `descriptor_indexing` | true | argument buffer 可映射 |
| `VK_KHR_fragment_shading_rate` | `has_attachment_shading_rate`（a7xx_base，730 有） | — |

**A730 真正缺的（都是 `a7xx_gen2` 才有，730 是 gen1）：**

| 特性 | 740 | 730 | 对 iOS/Metal 的实际影响 |
|---|---|---|---|
| `has_ray_intersection`（`VK_KHR_ray_query`） | ✔ | ✘ | Metal 3 光追；iOS 16 的 UI 不用 → 无影响 |
| `has_64b_ssbo_atomics` | ✔ | ✘ | Metal 没有 64 位 atomic → 无影响 |
| `has_primitive_shading_rate` | ✔ | ✘ | iOS 不暴露 VRS → 无影响 |
| `has_image_processing` | ✔ | ✘ | Metal imageblock 的部分形态，CA 不用 → 低 |
| `has_hw_bin_scaling` | ✔ | ✘ | 分箱缩放，纯性能 |
| `ubwc_unorm_snorm_int_compatible` | ✔ | ✘ | **格式兼容性**，见 §4.4；可能影响某些 render target |
| `has_event_write_sample_count` | ✔ | ✘ | occlusion query 精度，低 |
| `has_implicit_fragface_fragcoord_ij_linear` | ✔ | ✘ | 片元坐标插值，低 |

**[强推断]** A730 相对 A740 的缺口**不阻塞 Metal 转译**——缺的全是 iOS 16 UI 用不到的东西。
真正的差异是"没有被回归覆盖"，不是"能力不够"。

### 2.3 已知缺陷与回归覆盖（这才是风险）

| 事实 | 证据 |
|---|---|
| Mesa 的 freedreno CI **没有 a730/a740 设备**：设备清单是 a200/a306/a530/a618/a660/a702/a750 | `src/freedreno/ci/` 文件列表 |
| **a725/a730 专属崩溃**：2025-04 报（标题写 "since september"），bisect 到 MR 30995，**a740 起不受影响**；次日关闭 | Mesa issue #13075 |
| **a730 GPU id 误判**（部分环境认成 a725），表现为显存花纹/花屏/崩溃，2023-12 开，**至今 open** | Mesa issue #10274 |
| a730 及以下性能回归（ANB/AHB 合入后掉 40%） | Mesa issue #11233 |
| Turnip ANB/AHB 合入后崩 | Mesa issue #11171 |
| a730 上模拟器崩溃（缺 BuildId） | Mesa issue #9982 |

**[实锤]** 结论：Turnip 在 A730 上**是可用的**（社区大量 8 Gen 1 手机在跑），但
**A730 是"二等公民"**：没有 CI，历史 bug 密度高于 a740，且 10274 这种"认错芯片"的问题
会直接打到我们脸上（我们的 chip id 恰好是 0x07030002）。

### 2.4 Turnip 需要什么内核接口

Turnip 有三个 KNL 后端，meson 选项 `freedreno-kmds` 的 choices 是
`['msm', 'kgsl', 'virtio', 'wsl']`（注意：**没有 darwin**，UniXNU 是打补丁加的）。

#### (A) drm/msm 路线（Linux 主线 / postmarketOS）

Turnip 实际用到的 uapi（`tu_knl_drm_msm.cc` + `include/uapi/drm/msm_drm.h`）：

| ioctl | 语义 | 备注 |
|---|---|---|
| `DRM_MSM_GET_PARAM` / `SET_PARAM` | 设备参数 | Turnip 读 14 个 param：`CHIP_ID`、`GPU_ID`、`GMEM_BASE`、`GMEM_SIZE`、`TIMESTAMP`、`FAULTS`、`SUSPENDS`、`UBWC_SWIZZLE`、`MACROTILE_MODE`、`HIGHEST_BANK_BIT`、`UCHE_TRAP_BASE`、`PRIORITIES`、`RAYTRACING`、`HAS_PRR`；写 `EN_VM_BIND` |
| `DRM_MSM_GEM_NEW` | 分配 BO | 4 KiB 粒度；单对象上限由内核决定（uapi 里没有对应 param，Turnip 试到失败为止） |
| `DRM_MSM_GEM_INFO` | 取 offset/size/flags | |
| `DRM_MSM_GEM_CPU_PREP` / `CPU_FINI` | CPU 读写前后的 fence 等待 / 收尾 | 等价"wait fence + cache 维护的边界" |
| `DRM_MSM_GEM_SUBMIT` | 提交 IB 列表 + BO 列表 + in/out syncobj | 返回 fence |
| `DRM_MSM_WAIT_FENCE` | 等 fence（超时） | |
| `DRM_MSM_SUBMITQUEUE_NEW` / `CLOSE` | 每队列一个 GPU 上下文 | |
| `DRM_MSM_VM_BIND` | 显式 VM 映射/解映射（`MAP`/`UNMAP`/`MAP_NULL`） | 优先路径；旧内核退到 `GEM_INFO` 的 `MSM_INFO_SET_IOVA`（Turnip 仍保留 `has_set_iova` 分支） |
| DRM 核心：`SYNCOBJ_CREATE/DESTROY/WAIT/TRANSFER`、`PRIME_HANDLE_TO_FD`、`mmap` | dma-fence / dma-buf / CPU 映射 | 这些**不是** msm 私有的，是 DRM 框架提供的 |

#### (B) kgsl 路线（Android 事实标准）

`tu_knl_kgsl.cc` 用的：`IOCTL_KGSL_DEVICE_GETPROPERTY`、`IOCTL_KGSL_DRAWCTXT_CREATE/DESTROY`、
`IOCTL_KGSL_GPUOBJ_ALLOC/FREE/INFO/IMPORT`、`IOCTL_KGSL_GPUMEM_ALLOC_ID/FREE_ID/GET_INFO/BIND_RANGES`、
`IOCTL_KGSL_GPU_COMMAND`、`IOCTL_KGSL_GPU_AUX_COMMAND`、`IOCTL_KGSL_TIMESTAMP_EVENT`、
`IOCTL_KGSL_CMDSTREAM_READTIMESTAMP_CTXTID`、`IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID`、
`DMA_HEAP_IOCTL_ALLOC`。**[实锤]**

#### (C) 自定 ABI 路线（UniXNU 的选择，推荐抄）

UniXNU 在 `patches/mesa/turnip-darwin-kmd.patch`（56 KB，1,439 行 diff）里加了
`src/freedreno/vulkan/tu_knl_darwin.cc`（1,112 行），直接走 IOKit user client，
**不经过 libdrm、不经过 ioctl**。它依赖的 ABI 在 `Adreno740/NativeAdrenoABI.h`，
只有 **16 个 method**（见 §3.2）。

**[强推断]** 对我们来说 (C) 明显更便宜：

- (A)/(B) 要复刻的是**几十个 ioctl + DRM 框架语义**（drm_file、prime fd、syncobj、
  dma-fence、dumb buffer、render node 权限），其中一半是 Linux 特有的抽象，
  XNU 根本没有对应物，做出来还要写 shim 模拟文件描述符语义。
- (C) 的语义边界干净：BO 的 handle/iova 由用户态决定，fence 就是一个 64 位
  CP timestamp，没有 syncobj 对象生命周期。

---

## 3. 证据（三）：在 XNU 上要提供什么

### 3.1 先回答"metal2vulkan 需不需要内核配合"

**不需要。** 它是纯用户态库（Rust + `ash`），只跟 Vulkan ICD 说话；ICD 才是碰内核的那一层。
所以问题不是"给 metal2vulkan 写内核接口"，而是"给 Turnip 写内核接口"。
**[实锤]**（runtime README 的依赖与"Nothing links against Vulkan at build time"说明）

### 3.2 最小可跑通接口清单（照 NativeAdrenoABI，16 个方法）

| # | method | 输入/输出 | 语义要点 | K60 侧现状 | 难度 |
|---|---|---|---|---|---|
| 1 | `Query` | → `Params{abi_version, input_bits, output_bits, flags, max_bo_bytes}` | 协商 ABI 版本与地址位宽 | 无 | 低 |
| 2 | `GetDeviceInfo` | → `{chip_id, gmem_bytes, gmem_base, uche_trap_base, va_start, va_size, highest_bank_bit, ubwc_swizzle, macrotile_mode, ubwc_version, flags}` | Turnip 无法自己推导的硬件常量，必须由板级代码给 | 部分有（DT/固件里能查） | 低 |
| 3 | `BoAlloc` | `{bytes, align_log2, contiguous}` → `{handle}` | 4 KiB 倍数、≤256 MiB/对象；`contiguous` 要一整段物理 | 无（要 pmap/IOBufferMemoryDescriptor） | 中 |
| 4 | `BoImport` | `{address, bytes}` → `{handle}` | **pin 调用者自己的页**（IOSurface 共享内存），直到 `BoFree` | 无 | 中高 |
| 5 | `BoMap` | `{handle, iova, permissions, writeback, shareability}` → `{iova}` | 用户态选 IOVA，落 4 KiB stage-1 页表；GPU 侧 non-cacheable | **部分有**（0196 起的 SMMU context，但只有 GMU 用） | 中 |
| 6 | `BoCpuMap` / `BoCpuUnmap` | `{handle}` → `{token, bytes}` | 通过 `clientMemoryForType` 给用户态一块 CPU 映射 | 无 | 低 |
| 7 | `BoFree` | `{handle}` | 解映射、放页、清 TLB | 无 | 低 |
| 8 | `CreateQueue` / `CloseQueue` | → `{queue}` | 每队列一个 GPU 上下文（独立 IB ring + 独立 fence 序列） | 无 | 中 |
| 9 | `Submit` | `{queue, wait_fence, wait_timeout_ns, buffers[≤128]{handle,iova,bytes}}` → `{fence}` | 一组 IB 一次提交；fence 是 CP timestamp 值 | 无（**这是最大的空洞**） | 高 |
| 10 | `WaitFence` | `{fence, timeout_ns}` | 0 表示只 poll 一次 | 无 | 中（要 IRQ） |
| 11 | `GetTimestamp` | → `{ticks}` | GPU always-on counter | 无 | 低 |
| 12 | `GetStatus` | → `{faults, suspends}` | 单调计数，变化即 device lost | 无 | 低 |
| 13 | `SetMetadata` / `GetMetadata` | `{handle, bytes, text[64]}` | shader cache key / 调试标记 | 无 | 低 |

**内核内部还必须有的（不在 ABI 里，但没它 ABI 没意义）：**

| 项 | 内容 | K60 现状 |
|---|---|---|
| GMU 冷启动 | 载 ITCM/DTCM、`CM3_BOOT_CONFIG`、`CM3_SYSRESET=0`、轮询 `FW_INIT`、HFI 队列 | **卡住**：0187 上传实测通过（135 字、0 mismatch），0188 只清 CX halt 安全；**放复位即整机复位** |
| HFI | 队列、消息、功耗票（CX/MX/MXC）、频率档位 | 0（表和 iova 有了，0194 做过，未跑通） |
| CP ring | ring buffer、IB 提交、`CP_*` 包、preemption（可选） | 0 |
| fence | 每 submit 一个 timestamp 槽；CP 写回 → IRQ → 唤醒等待者 | 0 |
| IRQ | kgsl `0x12c`、GMU/HFI `0x130`/`0x131` | 0 |
| SMMU | 4 KiB stage-1、多 ASID/多 domain、fault 上报（`gfsr`）、TLB 维护 | **部分**：22 个 CB、`pages=32`、`gfsr=0`；GMU 的 SID 5/0x405 已建 route；**没有 per-client ASID** |
| 地址空间 | 客户端 VA 窗口（`va_start/va_size`）、GMEM/UCHE trap 窗口 | 未定 |
| GPU OPP | 频率/电压投票（GMU HFI 或 RPMh） | 0（RPMh 正在 0248 探） |
| fault 恢复 | GPU hang 检测、context lost、reset | 0 |
| cache 维护 | GPU 映射 non-cacheable；CPU 侧 clean/invalidate 由用户态做 | 未定 |

**[强推断]** 这 16 个方法（表里合并成 13 行）的**语义边界**是这条路唯一的好消息：
它比 drm/msm 干净一个数量级，UniXNU 的 1,112 行 KNL 就是证明——如果 ABI 设计得对，
Turnip 侧真的只要一千行。

### 3.3 为什么"只做 metal2vulkan"这个说法会骗人

把链路摊开看，每一段都是独立的大件：

```
iOS app ── Metal.framework ──(私有 SPI)──> 设备插件（MTLAddDevice）
                                              │
                                    metal2vulkan-runtime（Rust, Vulkan 1.2）
                                              │  AIR→SPIR-V（翻译器）
                                              ▼
                                        Vulkan ICD = Turnip
                                              │  SPIR-V→NIR→IR3
                                              ▼
                                    KNL 后端（drm/msm | kgsl | darwin 自定）
                                              ▼
                                    内核 KMD（BO/IOVA/submit/fence/GMU/CP/SMMU）
                                              ▼
                                        Adreno 730 硬件
```

**金属层（Metal.framework 插件）和内核层（KMD）各自都是一万行量级的工程，
中间那一层才是 metal2vulkan。** 现在的讨论容易把"中间那层已经有开源了"误当成
"整条路已经有大半"。

---

## 4. Adreno 730 与 740 的差异里，哪些会影响 Turnip / metal2vulkan

### 4.1 GMU 固件：不同文件，同一代际

| 项 | A730（K60） | A740（sheng/SM8550） |
|---|---|---|
| Linux 侧 family | `ADRENO_7XX_GEN1` | `ADRENO_7XX_GEN2` |
| GMU 镜像 | `gmu_gen70000.bin` | `gmu_gen70200.bin` |
| SQE 微码 | `a730_sqe.fw` | `a740_sqe.fw` |
| **GMEM** | **2 MiB**（`SZ_2M`） | **3 MiB**（`3 * SZ_1M`） |
| ZAP | `a730_zap.mbn`（板级签名） | `a740_zap.mbn`（sheng 的 `qcom/sm8550/sheng/a740_zap.mbn`） |

**[实锤]** 上表前三列直接来自 Linux 的 `a7xx_catalog.c`（a730 条目 `chip_ids 0x07030001`、
`.fw = {SQE="a730_sqe.fw", GMU="gmu_gen70000.bin"}`、`.gmem = SZ_2M`；
a740 条目 `0x43050a01 "C510v2"`、`gmu_gen70200.bin`、`3 * SZ_1M`），
且 `linux-firmware` 的 `qcom/` 目录里这四个文件名都存在。
我们仓库里的 `gpu-fw/gmu_gen70000.bin`（57,272 B）与 A730 匹配是对的
（0187 实测：上传 135 字、回读 0 mismatch；`gmu dtcm version=0x40000007
hfi_version=0x20010000`）。

**[实锤] 一个必须记住的坑**：Linux 的 catalog 只登记 `0x07030001`，
**没有 `0x07030002`**；而 K60 的 DT 写的是 `0x07030002`，Mesa 的
`freedreno_devices.py` 才有这一条（→ `FD725`）。也就是说"内核对 chip id 的认知"
和"Turnip 对 chip id 的认知"在这颗芯片上本来就不一致：

- 走 drm/msm 复刻路线时，内核要自己加 quirk 条目，否则 probe 直接找不到芯片；
- 走自定 ABI 时**只要把 `chip_id = 0x07030002` 原样交给用户态**，Turnip 自己查表即可，
  不需要内核参与匹配。这是 §3.2 里"内核只给常量、不做策略"的又一个理由。

**[强推断]** GMU 代际相同（都是 gen7），所以 Pong/Adreno740 的 GMU/HFI 代码可以当模板；
但**固件内部的块表与 HFI 消息集可能不同**，不能假设"换 bin 就能跑"。

### 4.2 ZAP：可以跳过，这是最重要的一个减法

Linux 的 `adreno_gpu.c` 里 zap 加载是可失败退出的：

```c
static bool zap_available = true;
...
	zap_available = false;   /* 没有 zap-shader 节点 / 没有 scm / 不是必需 */
...
int adreno_zap_shader_load(...) {
	if (!zap_available) return 0;   /* 短路，直接放行 */
	return zap_shader_load_mdt(gpu, adreno_gpu->info->zapfw, pasid);
}
```

**[实锤]** 并且 **UniXNU 的 Adreno740 KMD 里 `zap` 出现 0 次**（对 122 个文件 grep），
它照样在 sheng 上跑出了 GPU 合成桌面。

**[强推断]** ZAP 不是 UI 渲染的前置条件（它服务于受保护内容/DRM 路径）。
这对我们是关键结论：**不需要 TrustZone/SCM 就能有 GPU**，省掉一整块不可控的信任链工作。
代价是受保护视频/DRM 类路径不可用——iOS 16 UI 不需要。

### 4.3 SMMU：同一套 qsmmu-v500，但 ASID 数量决定多进程上限

| 项 | K60 实测 | 影响 |
|---|---|---|
| 类型 | `qcom,qsmmu-v500` + `qcom,adreno-smmu`（**不是** ARM SMMU-400） | 不能用上游 `ArmSmmu4K` 那套 SMMU-400 模板直接套 |
| context bank | **22 个**（DT override 写 21，硬件报 22，以硬件为准） | 足够给 render server + 客户端分 domain |
| SMR | 24 | |
| 页表 | `pages=32`、stride 4 KiB | 4 KiB stage-1，与 Turnip 的假设一致 |
| GMU 的 SID | `5` / `0x405`，一开始无 route | 0196 已建；这是 CM3 能碰内存的前提 |

**[实锤]**（`GPU-PORT-NOTES.md` §24.2 的真机读数）

**对 Turnip 的影响**：Turnip 假设"用户态选 IOVA + 内核建 4 KiB 映射 + 每队列一个上下文"。
22 个 CB 够用，但**必须实现 per-client ASID 与 TLB 按 ASID 失效**，否则多进程
（backboardd + 各 app）会互相踩。UniXNU 的做法是"≤512 页逐页 TLBIVAL，更多按 ASID 失效"。

### 4.4 UBWC：同是 4.0，但一个格式兼容位不同

| 项 | 值 |
|---|---|
| `ubwc_version`（ABI 字段） | 4（UBWC 4.0），730/740 相同 |
| `ubwc_swizzle` | 由板级给（K60 需从 DT/kgsl 配置确认） |
| `highest_bank_bit` | 730 = 16，740 = 16（相同） |
| `gmem_bytes` | **730 = 2 MiB，740 = 3 MiB**（ABI 必须给对；Turnip 用它算每个 bin 的 tile 预算） |
| `num_ccu` | **730 = 4，740 = 6**（不同，但 Turnip 按 chip id 从自己的表取，ABI 不用传） |
| `tile_align_w` / `tile_max_w` | **730 = 64 / 1024，740 = 96 / 2016**（同上，表内自带） |
| `ubwc_unorm_snorm_int_compatible` | **740 有，730 没有** |
| `supports_uav_ubwc` | 两者都有（a7xx_gen1 就有） |

**[强推断] ABI 的边界要划清**：Turnip 按 **chip_id** 在自己的表里选整条 profile
（`num_ccu`、tile 尺寸、`raw_magic_regs` 全从表里来，不需要内核传）；
内核要传的是**表里没有的、只能从板级量出来的**那些：`gmem_bytes/gmem_base`、
`uche_trap_base`、`va_start/va_size`、`highest_bank_bit`、`ubwc_swizzle`、
`macrotile_mode`、`ubwc_version`。**这条边界弄反了（比如内核自报 num_ccu 或 tile 尺寸）
会直接进入"能跑但花屏"的泥潭。**

**[强推断]** `ubwc_unorm_snorm_int_compatible` 影响"同一块 UBWC 内存用不同格式视图"
时的兼容判定。Metal 的纹理视图/别名用法比 Vulkan 激进，**这里可能是 A730 上
出现"颜色不对/花屏"的第一嫌疑**（对照 issue #10274 的症状：花纹、flicker）。
上机时先 `TU_DEBUG=noubwc` 对照一次，能立刻分开"UBWC 配置错"与"别的问题"。

### 4.5 时钟/电源：730 与 740 的真正差别不在 GPU 核，在板级

K60 的 Cape GPUCC 与 Pong（同为 SM8475）**完全同源**，我们已经逐条比对过
（`GPU-PORT-NOTES.md` §3：GDSC、两条 PLL、3 条 root、7 条 branch 与
`PongAdrenoClocks.c` 逐字一致），M2 实测 `GPUCC verified: 7/7`。
**A730 的时钟不是问题。**

问题是 **RSCC（`0x3d50000`）+ 父级电轨（VDD_CX/MX/MXC）**：只读也会把整机打死
（0200 实测），而厂商在第一次碰 RSCC 前必做
`gen7_rscc_wakeup_sequence()`（`GMU+0x26C1C` 写 2、轮询 `GMU+0x26C20` bit1）。
这一条与 730/740 无关，是"我们还没做"，但它是 CM3 出复位的直接前置
（详见 `docs/RSCC-CLOCK-RE.md`，以及 0199/0200 的两次机毁记录）。

### 4.6 差异总结：什么会挡路，什么不会

| 差异 | 挡路？ | 依据 |
|---|---|---|
| GMU/SQE 固件文件名不同 | **会**（必须换对文件） | 我们已有正确的 `gmu_gen70000.bin` |
| ZAP 必需性 | **不会**（可跳过） | Linux 可短路；Adreno740 从不碰 zap |
| `a7xx_gen1` vs `gen2` 特性 | **不会**（缺的都是 iOS UI 不用的） | §2.2 表 |
| UBWC 格式兼容位 | **可能会**（花屏类症状） | §4.4 |
| SMMU ASID 数量/路由 | **会**（多进程） | §4.3 |
| 时钟序列（GPUCC） | **不会**（已 7/7） | §4.5 |
| RSCC/父级电轨 | **会**（现在就是它挡着 CM3） | §4.5 |
| Turnip 无 A730 CI | **会**（回归风险） | §2.3 |

---

## 5. 分阶段计划（可执行、每段有判据）

> 前提：设备可用、代理可用、CI 出内核。所有阶段都**不写 `boot_a`**，只 `fastboot boot`。
> 每阶段的"停"都是硬停：判据不过就不要往下一段叠。

### Stage 0：把"量具"钉死（0.5–1 天）

- 目标：`sweetadreno=1 sweetadreno_power=1 sweetadreno_gmu=1` 一次启动里拿到
  `GPUCC verified: 7/7` + `gmu upload wrote … 0 mismatch` + `GMU is held in reset`。
- 这已经是既有能力（0188 + 0190 组合），**先复现一遍，确认没有退化**。
- 判据：三行齐全；`gmu load returned 0`；机器活到日志线程之后。
- 失败即停：`0/7 branches` 或 `readback mismatch` → 回到时钟/固件，不上 CM3。

### Stage 1：RSCC 访问前提 + CM3 出复位（2 周～2 个月，最大不确定性）

- 做：① VDD_CX/MX/MXC 父级投票；② `gen7_rscc_wakeup_sequence()`
  （`GMU+0x26C1C=2`，轮询 `GMU+0x26C20` bit1）；③ 只读 RSCC 8 个字；
  ④ 通过后 `gmu=2` 放 `CM3_SYSRESET`。
- 判据（按顺序，任何一步不过就停）：

| 步 | 通过条件 | 不通过说明 |
|---|---|---|
| 只读 RSCC | 8 个字非 `0xffffffff`、机器不死 | 时钟/电轨还没到位，别碰 CM3 |
| `gmu=2` | `gmu boot rc=0 fw_init=0x100` | 缺 RSCC/PDC 预序列，或固件块表不对（回看 0187 的 4 段） |
| HFI | `HFI is up` / 收到一条 HFI 消息 | 队列 iova 或中断线（0x130/0x131）没通 |

- 风险：这一段的失败模式是**整机复位且日志消失**。0190 的"延迟 120 s + 写前 flush"
  已经解决观测问题，务必保留。

### Stage 2：CP ring + 第一条 fence（3–5 周）

- 做：SQE 微码（`a730_sqe.fw`）→ CP ring 分配（BO + IOVA）→ 提交一个只含
  `CP_NOP`/`CP_WAIT_FOR_IDLE` 的 IB → CP 把 timestamp 写到内存 → IRQ → 读回。
- 判据：一个 300 行的内核自测（`sweetadreno_cptest=1`）打印
  `CP fence 0x… completed in … us`，且**同一个 fence 值**等于 `GetTimestamp` 前后的差值合理。
- 为什么先做这个而不是 Turnip：**Vulkan 的 90% 复杂度都在"提交 + fence"**，
  这一条通了，Turnip 的 KNL 只是把它翻译成 §3.2 的 16 个方法。

### Stage 3：KMD user client（3–5 周）

- 做：照 §3.2 的 13 行接口（16 个方法）实现 user client；BO 用 `IOBufferMemoryDescriptor`
  + `inTaskWithPhysicalMask`（contiguous）/`inTaskWithOptions`（分散）；
  `BoImport` 用 `IOMemoryDescriptor::withAddressRange` + wire；
  `BoMap` 落 4 KiB stage-1；`BoCpuMap` 走 `clientMemoryForType`。
- 判据：用户态小工具能 alloc→map→CPU 写→submit（空 IB）→wait→CPU 读回，
  数据一致；`GetStatus` 在故意触发 GPU fault 后 `faults` 递增。
- 关键设计决定：**IOVA 由用户态选**（Turnip 要这个），内核只校验落在 `va_start..va_start+va_size`。

### Stage 4：Turnip 上机（2–4 周）

- 做：抄 `turnip-darwin-kmd.patch` 的结构写 `tu_knl_k60.cc`（或直接改 darwin 后端，
  把 `IOConnectCallStructMethod` 换成我们的 user client）；meson 交叉编译
  `-Dvulkan-drivers=freedreno -Dfreedreno-kmds=<我们的> -Dplatforms=` 关掉 WSI；
  先跑 `vulkaninfo` 级别的探针，再跑一个 compute dispatch，再跑三角形。
- 判据：`vkCreateDevice` 成功；`vkQueueSubmit` + `vkWaitSemaphores` 完成；
  三角形像素正确（写进一个 BO，CPU 读回比对）。
- 失败分类：`VK_ERROR_INCOMPATIBLE_DRIVER` = ABI/服务名不对；
  device lost = GPU fault，看 `GetStatus`；黑屏但无 fault = UBWC/格式（`TU_DEBUG=noubwc` 对照）。

### Stage 5：metal2vulkan 起来（1–2 周）

- 做：把 `metal2vulkan` + `metal2vulkan-runtime` 交叉编译到 aarch64-darwin
  （Rust 1.98+；不链 Vulkan，运行时 `dlopen` ICD）；
  用 `DeviceDescription::from_khronos_json` 吃一份**从我们设备 dump 出来的 profile**
  （不要用仓库里的桌面 profile，feature/limit 会错）；
  先跑仓库自带的 compute 例子（`examples/compute_dispatch.rs`）。
- 判据：`new_library_with_data(default.metallib)` → `new_compute_pipeline_state_with_function`
  → dispatch → 结果与 CPU 参考一致。
- 注意：这一步**还不需要 Metal.framework**，输入是 metallib 文件，绕开了插件与 SPI。

### Stage 6：Metal.framework 接管（3–6 个月，按需）

- 做：`IOAcceleratorES → MetalPluginName → NSBundle → principal class →
  initWithAcceleratorPort:` 的插件（我们 0178 的 `IOAcceleratorES`/`AGXAccelerator`
  空壳正是挂点）；`MTLAddDevice` 注册；IOSurface 导入（`BoImport` 路径）；
  scanout 交接（GPU 画到面板页）。
- 顺序：先让 backboardd 拿到设备但**只做 clear**（判据：屏幕变纯色），
  再让 CA 走 GPU 合成（判据：`SWEET FB: … swaps` 且 `nonempty` 非零），
  最后才是第三方 app。
- 判据：`sweetaccel=0` 与 `=1` 两次启动的帧率/CPU 占用对比；GPU 路径必须明显更省 CPU，
  否则说明它在偷偷走软件路径。

### 5.1 与现有软件渲染线的关系

**不冲突，不要停。** 0212 已经用 `CA::OGL::SW` 把锁屏画出来了（43,000 swaps、
单帧 21.3 ms、`nonempty 2741/2741`）。GPU 线的价值是"把 CPU 从光栅化里解放出来"，
不是"让画面出现"。所以 GPU 任何一阶段失败，系统都还能用软件渲染跑——
这是继续投入的安全网，也是"不划算"判断能被推翻的唯一理由（如果软件渲染在真实
使用中成为瓶颈）。

---

## 6. 工作量与风险

### 6.1 参考实现的真实规模（不是我估的，是数出来的）

| 件 | 规模 | 说明 |
|---|---|---|
| `UniXNU/Adreno740`（KMD 核心，排除 tests） | **7,227 行** C++ | 60 个文件：VM 569、UserClient 681、Publish 761、Submit 407、Ring 350、Service 643、Memory ~500… |
| 同仓含测试 | 11,369 行 | |
| `turnip-darwin-kmd.patch` | 1,439 行 diff（`tu_knl_darwin.cc` 1,112 行） | 新增一个 KNL 后端 + meson + WSI |
| `UniXNU/AdrenoMetal`（前端，排除 host 工具） | **≈11,500 行** | ObjC++ 2,075 + Rust 2,208 + wire/scanout/cache ≈5,500 |
| `AdrenoMetal/bridge/patches` | 16 个补丁，156 KB | metal2vulkan 侧的可用性/性能补丁 |
| `UniXNU` 的 mesa 补丁集 | 9 个，共 ~100 KB | 其中 `turnip-builtin-shader-pack` 18 KB、`turnip-darwin-kmd` 56 KB |
| **合计可抄代码** | **≈2 万行**，且是"已经知道怎么做"的人写的 | |

### 6.2 我们的乘法系数（为什么不能按 2 万行估）

| 因素 | 影响 |
|---|---|
| 我们的 KMD 基线更差 | 参考实现从"GMU 已起、CP 已跑"开始；我们卡在 CM3 出复位，**上游代码里没有对应章节**（他们的 A740 从来没遇到 RSCC 墙） |
| 板级不同 | qsmmu-v500 的 22 个 CB、SID 5/0x405、hbb、ubwc_swizzle 都要自己量 |
| 工具链 | Turnip/metal2vulkan 要在 Linux 上交叉编到 aarch64-darwin；我们只有 CI 的 macOS runner |
| 调试带宽 | 一次迭代 = 编译 15 min + 刷机 + 3 min 启动 + 拉日志；一天最多 10 轮 |
| A730 无 CI | Turnip 的回归只能靠我们自己上机 |

### 6.3 估算（全职，且设备在手）

| 段 | 乐观 | 现实 |
|---|---|---|
| Stage 1（RSCC + CM3） | 2 周 | **1–2 个月**（撞墙概率最高） |
| Stage 2（CP + fence） | 3 周 | 1–1.5 个月 |
| Stage 3（user client） | 3 周 | 1 个月 |
| Stage 4（Turnip 上机） | 2 周 | 1 个月 |
| Stage 5（metal2vulkan） | 1 周 | 2–3 周 |
| **小计 (a)+(b) 的"能跑 Vulkan + 能跑 compute"** | **≈3 个月** | **5–7 个月** |
| Stage 6（Metal 接管 UI） | 2 个月 | **3–6 个月** |
| **总计** | 5 个月 | **9–13 个月** |

**这不是"一个周末的活"，也不是"接个库"。** 参考团队（有 A740 硬件、有 Linux 上游、
有 sheng 板级源码）从建仓（2026-10-20）到"GPU 合成桌面"用了约 11 天——
但那 11 天背后是他们已经把 KMD/Turnip/metal2vulkan 全写完了，
而且 A740 的板级输入是现成的。

### 6.4 风险表（按"概率 × 影响"排序）

| # | 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|---|
| 1 | RSCC/父级电轨没搞对，碰一下就整机复位 | **高**（已发生 2 次） | 每轮白跑 + 设备掉线 | 严格按 `RSCC-CLOCK-RE.md` 的投票 + wakeup 序列；只读先行；写前 flush |
| 2 | CM3 出复位仍缺厂商预序列（PDC/RSCC/bus） | 高 | CM3 永远起不来 | 对照 kgsl 未 strip 的 `msm_kgsl.ko` 反汇编；`gmu=2` 前先做 RSCC |
| 3 | SQE 微码/CP 初始化与 A730 不符 | 中 | fence 永远不回 | 用 `a730_sqe.fw`；先 NOP IB，别先上 3D |
| 4 | SMMU 多 ASID/TLB 维护错 | 中 | 多进程互相踩、随机崩 | 先单客户端；ASID 失效照 UniXNU 的 ≤512 页规则 |
| 5 | UBWC/格式兼容（730 缺 unorm_snorm_int） | 中 | 花屏/颜色错，但"能跑" | `TU_DEBUG=noubwc` 对照；必要时禁用 UBWC |
| 6 | iOS Metal 私有 SPI 随版本变（16.7 vs 18.x） | 中 | 插件加载失败/断言 | 先只做 `MTLAddDevice` + clear；不要一次追全 API |
| 7 | `MTLCompilerService` 在我们的 rootfs 跑不起来 | 中 | 运行时编译 MSL 的 app 全崩（系统 UI 不受影响） | 先只吃预编译 metallib；把这条当"已知限制" |
| 8 | metal2vulkan 是单人 beta，API 会变 | 中 | 升级即破 | 钉 commit；接受 16 个补丁的维护成本 |
| 9 | 许可证 | 低 | GPL/LGPL 传染 | metal2vulkan 可选 LGPL-3.0；Turnip MIT；Adreno740 MIT。动态链接 + LGPL 可接受，但要留记录 |
| 10 | 性能不达预期（转译 + A730 + 无 CI 优化） | 中 | "跑起来了但更卡" | 参考 sheng：冷启动 43 s 编译 77 个 pipeline；我们只会更慢。先量再优化 |

### 6.5 "现在不划算"的明确表述

**结论：以 2026-10-31 的状态，不建议把 GPU 转译层当主线。**

依据（三条，都可复查）：

1. **前置条件未满足**：CM3 出复位是整条链的唯一硬前提，我们现在放复位即整机复位
   （0196/0197 实测，日志停在 `console ring flushed before releasing CM3`）。
   在这个前提下讨论 Turnip/metal2vulkan 没有意义。
2. **投入产出比差**：9–13 个月全职，换来的是"把一个已经能用软件渲染跑锁屏的系统
   变成 GPU 渲染"。而项目当前的阻塞是触摸（RSC 一读就死）与稳定性，不是帧率。
3. **上游证据等级不够**：A730 只有一句自述（"used for desktop rendering"），
   没有日志；唯一被上游承认的验证设备是 A740。按仓库规矩不能拿它当既有成果。

**什么情况下值得重启这条路**（给未来的自己）：

- 软件渲染在真实使用中成为瓶颈（例如滚动/动画 CPU 占用长期 >80%，或 `swaps` 掉到 20 fps 以下）；
- 或者 CM3/CP 那条腿已经被别的目的（例如 GPU 频率、显示）走通，剩下的只是 Turnip；
- 或者上游把 A730 纳入 Mesa CI，回归成本大幅下降。

---

## 7. 如果明天只做一件事

**做 `gen7_rscc_wakeup_sequence()` + 父级电轨投票，然后只读 RSCC 8 个字。**

理由：它是唯一的硬门。CM3 出不来，HFI/CP/fence/Turnip/metal2vulkan/Metal 全部停在纸上；
CM3 出来了，后面每一段都有上游代码可抄。而且这一步**不依赖任何上游项目**，
成本是几十行寄存器写 + 一次上机。

具体动作（照 `docs/RSCC-CLOCK-RE.md` §0 的两条）：

```
1. 父级电轨：VDD_CX / VDD_MX / VDD_MXC 投票（缺一条就一步都别跑）
2. GPU RSC 上电握手：GMU+0x26C1C ← 2；轮询 GMU+0x26C20 bit1
3. 只读 RSCC(0x3d50000) 8 个字，落盘后再决定是否 gmu=2
```

判据：`sweetadreno_rscc=1` 那次启动里，8 个字读完、机器不死、日志有 `RSCC readable`；
然后同一次启动续跑 `gmu=2`，目标是 `gmu boot rc=0 fw_init=0x100`。

**不要**在同一次启动里同时改映射、改时钟、改固件段表——那会让"停在哪"重新变成猜谜。

---

## 8. 一张速查表

| 问题 | 答案 | 证据 |
|---|---|---|
| metal2vulkan 是库还是驱动？ | 纯用户态库（翻译器 + Metal API on Vulkan），不需要内核配合 | §1.2 |
| 它要求什么 Vulkan？ | 1.2 + `timelineSemaphore` | §1.3 |
| A730 的 Turnip 报什么版本？ | Vulkan 1.4（chip ≥ 7） | §2.2 |
| A730 缺 descriptor buffer 吗？ | **不缺**，`EXT_descriptor_buffer = true` | §2.2 |
| 有人在 A730 上跑通 metal2vulkan 吗？ | 只有社区自述，无日志 | §1.4 |
| 需要 TrustZone/ZAP 吗？ | 不需要（可短路；Adreno740 从不碰 zap） | §4.2 |
| 我们离"Vulkan 能跑"多远？ | CM3 → CP → fence → KMD → Turnip，≈5–7 个月现实估计 | §6.3 |
| 该不该现在做？ | **不该**，除非软件渲染成为瓶颈 | §6.5 |
| 明天做什么？ | RSCC wakeup + 只读 8 字 | §7 |

---

## 9. 复跑清单（本文结论都是这样取到的）

```sh
# 0) 认证
export GH_TOKEN=$(cat ~/.config/gh-token)

# 1) metal2vulkan：元数据 / 贡献者 / issue
curl -sS -H "Authorization: token $GH_TOKEN" \
  https://api.github.com/repos/steelbrain/metal2vulkan | jq '{stargazers_count,forks_count,created_at,pushed_at}'
curl -sS -H "Authorization: token $GH_TOKEN" \
  "https://api.github.com/repos/steelbrain/metal2vulkan/contributors?per_page=10"
curl -sS -H "Authorization: token $GH_TOKEN" \
  "https://api.github.com/repos/steelbrain/metal2vulkan/issues?state=all&per_page=50" | jq -r '.[].title'

# 2) metal2vulkan：全仓有没有 Adreno 痕迹（应 0 命中）
curl -sS -H "Authorization: token $GH_TOKEN" -H "Accept: application/vnd.github.raw" \
  https://api.github.com/repos/steelbrain/metal2vulkan/contents/README.md -o /tmp/m2v-README.md
grep -ic "adreno\|turnip\|freedreno\|kgsl" /tmp/m2v-README.md     # 0

# 3) Turnip：A730 条目、VK 版本、扩展表
curl -sSL "https://gitlab.freedesktop.org/mesa/mesa/-/raw/main/src/freedreno/common/freedreno_devices.py" -o /tmp/fd.py
grep -n -B2 -A14 "0x07030002" /tmp/fd.py
curl -sSL "https://gitlab.freedesktop.org/mesa/mesa/-/raw/main/src/freedreno/vulkan/tu_device.cc" -o /tmp/tudev.cc
grep -n "TU_API_VERSION\|descriptor_buffer\|timeline_semaphore" /tmp/tudev.cc | head

# 4) Turnip：内核接口（两个后端各用了哪些 ioctl/param）
curl -sSL "https://gitlab.freedesktop.org/mesa/mesa/-/raw/main/src/freedreno/vulkan/tu_knl_drm_msm.cc" -o /tmp/tumsm.cc
grep -o "DRM_MSM_[A-Z_]*\|MSM_PARAM_[A-Z_0-9]*" /tmp/tumsm.cc | sort -u
curl -sSL "https://gitlab.freedesktop.org/mesa/mesa/-/raw/main/src/freedreno/vulkan/tu_knl_kgsl.cc" -o /tmp/tukgsl.cc
grep -o "IOCTL_KGSL_[A-Z_]*\|DMA_HEAP_IOCTL_[A-Z_]*" /tmp/tukgsl.cc | sort -u

# 5) A730 vs A740 的固件与 GMEM（Linux 侧权威表）
curl -sSL "https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/drivers/gpu/drm/msm/adreno/a7xx_catalog.c" -o /tmp/a7cat.c
grep -n -A10 "0x07030001\|0x43050a01" /tmp/a7cat.c

# 6) AdrenoMetal / Adreno740 的规模（决定工作量估算）
git clone --depth 1 https://github.com/UniXNU/Adreno740.git /tmp/refs/Adreno740
git clone --depth 1 https://github.com/UniXNU/AdrenoMetal.git /tmp/refs/AdrenoMetal
find /tmp/refs/Adreno740 -name '*.cpp' -o -name '*.h' | grep -v tests | xargs wc -l | tail -1
wc -l /tmp/refs/AdrenoMetal/NativeBridgeMetalFrontend.mm /tmp/refs/AdrenoMetal/bridge/NativeMetalObjects.rs

# 7) ZAP 是否被参考实现依赖（应 0 命中）
grep -rn -i "zap\|scm_call\|trustzone\|qsee" /tmp/refs/Adreno740 | wc -l
```

**判读**：第 2 步若不是 0，说明我漏看了 issue（结论要改）；第 7 步若不为 0，
说明"ZAP 可跳过"这条要重新评估——这是本文对路线选择影响最大的一个判断。

---

## 来源

metal2vulkan 主仓 ( https://github.com/steelbrain/metal2vulkan )
metal2vulkan README（beta、许可、无 MSL 编译器） ( https://github.com/steelbrain/metal2vulkan/blob/master/README.md )
metal2vulkan 架构说明 ( https://github.com/steelbrain/metal2vulkan/blob/master/docs/ARCHITECTURE.md )
metal2vulkan runtime README（Vulkan 1.2 + timelineSemaphore） ( https://github.com/steelbrain/metal2vulkan/blob/master/crates/metal2vulkan-runtime/README.md )
metal2vulkan 运行期移植指南（host memory / no-copy） ( https://github.com/steelbrain/metal2vulkan/blob/master/docs/RUNTIME-PORTING.md )
metal2vulkan 校验 profile 目录（无 Adreno） ( https://github.com/steelbrain/metal2vulkan/tree/master/crates/metal2vulkan-validation/profiles )
metal2vulkan on crates.io（0.1.0 是另一套旧设计） ( https://crates.io/crates/metal2vulkan )
metal2vulkan issue 列表（10 条，无 Adreno） ( https://github.com/steelbrain/metal2vulkan/issues )
UniXNU/AdrenoMetal（Metal 前端 + metal2vulkan，验证设备 sheng/A740） ( https://github.com/UniXNU/AdrenoMetal )
AdrenoMetal README（"verified on sheng with A740"） ( https://github.com/UniXNU/AdrenoMetal/blob/main/README.md )
AdrenoMetal ObjC++ 前端（2,075 行） ( https://github.com/UniXNU/AdrenoMetal/blob/main/NativeBridgeMetalFrontend.mm )
AdrenoMetal 插件入口（IOAcceleratorES → MetalPluginName） ( https://github.com/UniXNU/AdrenoMetal/blob/main/NativeAdrenoMetalPlugin.mm )
AdrenoMetal metal2vulkan 补丁集 ( https://github.com/UniXNU/AdrenoMetal/tree/main/bridge/patches )
UniXNU/Adreno740（XNU 侧 A740 KMD，核心 7,227 行） ( https://github.com/UniXNU/Adreno740 )
NativeAdrenoABI.h（16 个 method 的 KMD 线协议） ( https://github.com/UniXNU/Adreno740/blob/main/NativeAdrenoABI.h )
UniXNU 主仓 ( https://github.com/UniXNU/UniXNU )
UniXNU README（News 只宣称 sheng 的 GPU 加速） ( https://github.com/UniXNU/UniXNU/blob/main/README.md )
build-turnip-darwin.py（-Dfreedreno-kmds=darwin） ( https://github.com/UniXNU/UniXNU/blob/main/scripts/build-turnip-darwin.py )
turnip-darwin-kmd.patch（tu_knl_darwin.cc 1,112 行） ( https://github.com/UniXNU/UniXNU/blob/main/patches/mesa/turnip-darwin-kmd.patch )
UniXNU 文档：AdrenoMetal（依赖与验证设备） ( https://unixnu.com/drivers/adreno-metal )
UniXNU 文档：Adreno740 运行路径（运行时链路、发布、内存与 TLB） ( https://unixnu.com/zh/drivers/adreno740 )
UniXNU 文档：GPU 与 Metal 适配流程（固件表、接口、判据） ( https://unixnu.com/zh/porting/gpu )
UniXNU 文档：pong 设备页（GPU 标 implemented） ( https://github.com/UniXNU/UniXNU/blob/main/docs/zh/devices/pong.md )
UniXNU 文档：pong 设备页英文版 ( https://github.com/UniXNU/UniXNU/blob/main/docs/devices/pong.md )
UniXNU issue：Mali 组件提议（唯一的 metal2vulkan+adreno 命中） ( https://github.com/UniXNU/UniXNU/issues/127 )
Chengdong-Yu/UniXNU-pong（SM8475 A730 设备仓） ( https://github.com/Chengdong-Yu/UniXNU-pong )
pong README（中文，GPU 行"渲染已用于桌面"） ( https://github.com/Chengdong-Yu/UniXNU-pong/blob/main/README.md )
pong README（英文，同句"used for desktop rendering"） ( https://github.com/Chengdong-Yu/UniXNU-pong/blob/main/README_EN.md )
Mesa Freedreno/Turnip 文档 ( https://docs.mesa3d.org/drivers/freedreno.html )
freedreno_devices.py（a730 = 0x07030002，FD725，a7xx_gen1） ( https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/freedreno/common/freedreno_devices.py )
tu_device.cc（TU_API_VERSION 1.4、扩展表、descriptor buffer） ( https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/freedreno/vulkan/tu_device.cc )
tu_knl_drm_msm.cc（drm/msm 后端的 ioctl 与 param） ( https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/freedreno/vulkan/tu_knl_drm_msm.cc )
tu_knl_kgsl.cc（kgsl 后端） ( https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/freedreno/vulkan/tu_knl_kgsl.cc )
mesa meson.options（freedreno-kmds = msm/kgsl/virtio/wsl） ( https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/meson.options )
mesa freedreno CI 设备清单（无 a730/a740） ( https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/freedreno/ci )
mesa issue #10274：a730 GPU id 误判（open） ( https://gitlab.freedesktop.org/mesa/mesa/-/work_items/10274 )
mesa issue #13075：a725/a730 崩溃，a740 起不受影响 ( https://gitlab.freedesktop.org/mesa/mesa/-/work_items/13075 )
mesa issue #11233：a730 及以下性能回归 ( https://gitlab.freedesktop.org/mesa/mesa/-/work_items/11233 )
mesa issue #11171：ANB/AHB 合入后 Turnip 崩溃 ( https://gitlab.freedesktop.org/mesa/mesa/-/work_items/11171 )
Mesa Matrix（Turnip 各代特性矩阵） ( https://mesamatrix.net/ )
Linux drm/msm uapi 头（DRM_MSM_* 命令与结构） ( https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/include/uapi/drm/msm_drm.h )
Linux adreno_gpu.c（zap 可短路） ( https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/drivers/gpu/drm/msm/adreno/adreno_gpu.c )
linux-firmware qcom 目录（gmu_gen70000/70200、a730/a740_sqe.fw） ( https://gitlab.com/kernel-firmware/linux-firmware/-/tree/main/qcom )

本地证据（本仓库，可复查）：

- `docs/GPU-PORT-NOTES.md` §3（GPUCC 与 Pong 逐条一致）、§20（M1/M2 实测 7/7）、
  §21（0187 上传 135 字 0 mismatch）、§22（放复位整机复位）、§24（SMMU 22 CB / SID 5 无 route）、
  §27（cache 窗口 16 MB 与 0205/0207）。
- `docs/ADRENO-PLAN.md`（寄存器地图、M1–M4 里程碑）。
- `docs/ADRENO-0206-BACKUP.md`（0207 vs 0206 的两条路与判据）。
- `docs/NEXT.md`（锁屏达成 0212；触摸 RSC 仍死）。
- `~/hackios/docs/RSCC-CLOCK-RE.md`（RSCC 一碰就死的两条根因：父级电轨 + wakeup 序列）。
- `~/hackios/docs/SMMU-FEASIBILITY.md`（qsmmu-v500 的可行性）。
