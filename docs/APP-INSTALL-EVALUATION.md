# 自签 / TrollStore 那套能不能在自移植的 XNU 上落地（2026-11-01）

> 纸面论证。设备不在，结论都是"读代码 + 读镜像"得出的，没有上机验证。

## 结论

1. **TrollStore 的原理（CoreTrust 签名校验漏洞）不需要**。那条漏洞的意义是
   "在苹果的内核上骗过签名"，我们自己的内核根本不看签名。
2. **我们已经在关签名了**：`amfi_get_out_of_my_way=1` 加上 0142/0144 两个补丁，
   内核侧 `cs_validate_page` 与 exec 后的重校验都直接放行。launchd 之前被
   `CS_KILLED` 打死就是靠这两个补丁救回来的。
3. **真正的门不在签名，在安装管线和运行时**：`installd` / `amfid` /
   `MobileInstallation` 这套用户态还在，容器（`/private/var/containers`）
   有了但里面是空的；再加上没有 GPU、音频、网络，很多 App 就算装上也会白屏或崩。
4. **判断**：自签 IPA 装进去、跑起来（哪怕只有部分功能），**可行**；
   把它做成"日用"不可行。工作量集中在 stub 用户态签名验证 + 打通安装管线，
   **大约 2–4 周**能到"装一个自签 App、能启动、界面能动"。

## 1. TrollStore 到底做了什么

TrollStore 走的是 **CoreTrust** 的签名校验漏洞：用一张特制的证书链 + CMS blob，
让 CoreTrust 把任意签名的 App 判成"可信"，因此可以安装带任意 entitlements 的
IPA，且不需要开发者证书、不需要联网。它的配套是：

- 一个安装器（复制 .app 到 /Applications、注册 LaunchServices）
- 一个"持久化助手"（利用系统 App 的漏洞保持安装器存活）
- 对签名/entitlements 的绕过（依赖上面那个 CoreTrust 漏洞）

**关键洞察**：整个链条里，唯一与"苹果内核"强绑定的就是 CoreTrust 那个漏洞。
我们掌控内核，可以直接让 CoreTrust 的结论失效，或者干脆不让它参与。

## 2. 我们的内核现在关到了哪一步

| 关卡 | 状态 | 依据 |
| --- | --- | --- |
| 页级签名校验（`cs_validate_page`） | **已关**，`amfi_get_out_of_my_way=1` 时直接返回有效 | `0142-k60-amfi-allow-invalid.patch` |
| exec 后的签名重校验 | **已关**，失败也不再杀进程 | `0144-k60-exec-cs-bypass.patch` |
| AMFI 的 MAC 策略 | 我们的是**内置桩**（`bsd/kern/sweet_amfi.c`），不加载苹果的 AMFI kext | `0025/0035` 系补丁 |
| trust cache | 我们自己造（原始 Image4/AMFI 不存在） | `0015` 系补丁 |
| 根卷封印（seal） | 已解除 | `0029-sweet-unsealed-root-volume.patch` |

也就是说：**任何二进制，只要我们能把它放到盘上、exec 得到，内核就不会因为
签名问题杀它。**

## 3. 还剩下的关（全在用户态）

| 组件 | 在哪 | 它会干什么 | 我们要怎么处理 |
| --- | --- | --- | --- |
| `amfid` | `/usr/libexec/amfid`（镜像里确认存在） | 接收内核/库的验签请求，返回 cdhash 与 entitlements | 换成桩：一律返回"有效 + 请求的 ent"，或让调用方永远拿不到失败 |
| `installd` | `/usr/libexec/installd`（存在） | 安装 .app、分配容器、调用 amfid 验签 | 打补丁跳过验签，或直接绕过它：手工把 .app 放进 `/Applications` 再注册 |
| `MobileInstallation.framework` | 私有框架（存在） | App 安装/查询的公开入口 | 前一条绕过它即可；要让"设置里能看到"再补 |
| Sandbox | `sandboxd` + profiles | 决定 App 能碰什么 | 我们已经能跑 launchd 和系统进程，App 容器 profile 需要验证 |
| 容器 | `/private/var/containers/{Bundle,Data,Shared}`（目录存在，**里面是空的**） | App 的沙箱目录 | 首次启动前要建好 `Data/Application/<UUID>` 结构 |
| CoreTrust | 不在盘上（应在 dyld 共享缓存里） | 系统级信任评估 | 走不到就不管；走到了就 stub |

注意：上面这些"存在"是从根镜像里实际 ls 出来的；`CoreTrust` 没有独立的
framework 文件，属于共享缓存里的私有件，需要时用 `nm` 在缓存上确认。

## 4. 自签 IPA 的两种落地方式

**A. 走系统安装管线（推荐先做）**

1. 用 `codesign`（或 ldid）给 .app 自签，写一份"什么都申请"的 entitlements。
2. 把 .app 放到 `/Applications/`，`chown -R mobile:mobile`。
3. 让 `installd` 注册（或直接写 `LaunchServices` 数据库）。
4. 启动时内核不看签名（已具备），`amfid` 若被调用就返回有效。

这套不需要 CoreTrust，也不需要 TrollStore。

**B. 借 TrollStore 的壳**

只在"我们要在真实 iOS 设备上分发"时才有意义；在自移植系统上属于绕远路。
但它有一个可借鉴点：**持久化助手**的思路——用一个系统自带的、有特权
entitlement 的进程去装 App，避免自己实现安装器。我们这边 `installd` 就是
现成的那个进程。

## 5. 装上去之后，什么会让 App 崩

按我们现在的硬件能力排：

| 缺的东西 | 影响 | 严重程度 |
| --- | --- | --- |
| GPU / Metal | 任何用 Metal 画界面的 App 直接白屏或崩（SwiftUI 大量用 Metal） | **致命**，绝大多数 App 中招 |
| 音频（ADSP） | 启动时 `AVAudioSession` 初始化失败，部分 App 立刻退出 | 高 |
| 网络（Wi-Fi/蜂窝） | 联网类 App 卡在加载；`CFNetwork` 本身可用，但没链路 | 高 |
| 传感器 / CoreMotion | 游戏、健康类崩溃或降级 | 中 |
| 相机 / AVFoundation 采集 | 相机类 App 直接不可用 | 中 |
| 定位 | 地图、外卖类不可用 | 中 |
| 推送 / APNs | 依赖网络的进一步恶化 | 低 |

结论：**签名不是瓶颈，GPU 才是**。这也是为什么 pandora 能"跑 App"——
它的 GPU 合成已经通了；我们现在连 backboardd 都在用 CPU 画。

## 6. 建议路线

| 阶段 | 目标 | 判据 | 依赖 |
| --- | --- | --- | --- |
| 1 | 手工装一个自签 App 进 `/Applications` 并启动 | 日志出现该 App 的 `exec` 与首帧 | 自带 App 能跑（现状） |
| 2 | stub `amfid`（一律有效） | 安装不再因签名失败 | 需要改用户态二进制 |
| 3 | 建好容器目录结构 | App 能写自己的 `Documents` 不崩 | 阶段 2 |
| 4 | 跑一个纯 CPU 渲染的简单 App（如计算器类） | 界面能动、能点 | 阶段 1–3 |
| 5 | 等 GPU 通了再谈"正常 App" | SwiftUI 界面不白屏 | GPU 里程碑 |

## 7. 风险

- `amfid` 是平台二进制，受我们自己造 trust cache 的约束；替换它可能触发
  launchd 的校验路径，需要先在 QEMU/真机上小步验证。
- 容器目录结构随 iOS 版本变化，16.7 与 16.1 有差异，抄的时候要对版本。
- 关掉签名校验会让"任何二进制都能跑"——包括我们自己写的调试件，
  这是特性也是风险，建议只在内核启动参数打开时生效（现状已是如此）。
